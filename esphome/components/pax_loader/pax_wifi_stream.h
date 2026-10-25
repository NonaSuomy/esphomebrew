#pragma once
#include "pax_wifi_settings.h"
#include "pax_wifi_scan_stream.h"
#include "pax_wifi_backup.h"
#include "esphome/core/log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#ifdef ESP_PLATFORM
#include "esp_wifi.h"
#if defined(SNTP_SERVER_COUNT)
#include "esp_sntp.h"
#endif
#endif
namespace pax_wifi {
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static pax_wifi_request_t pending{};
static bool queued=false;
static constexpr const char *BACKUP="/sdcard/roms/pax/wifi.bin";
/* Called only from the main loop after SD mounts. The SD copy is the startup
 * recovery source, including after an erase or a different firmware build. */
static void maintain_backup(esphome::wifi::WiFiComponent *wifi, bool changing, bool user_apply) {
 static int64_t next=0;static bool restored=false;
 if(user_apply)restored=true;
 int64_t now=esp_timer_get_time();if(now<next || changing)return;next=now+5000000;
 struct stat info{};if(stat("/sdcard",&info)!=0 || !S_ISDIR(info.st_mode))return;
 pax_wifi_request_t saved{};
 bool present=pax_wifi_backup::load(BACKUP,saved);
 bool recovered=false;
 if(!present){recovered=pax_wifi_backup::load("/sdcard/roms/pax/wifi.bin.prev",saved) ||
   pax_wifi_backup::load("/sdcard/roms/pax/wifi.bin.tmp",saved);present=recovered;}
 if(!restored){
  restored=true;
  if(present && !user_apply){
   auto current=wifi->get_sta();
   if(current.get_ssid()!=saved.ssid || current.get_password()!=saved.password){
    bool was_disabled=wifi->is_disabled();
    wifi->disable();wifi->save_wifi_sta(saved.ssid,saved.password);if(!was_disabled)wifi->enable();
    ESP_LOGI("pax_wifi","Restored Wi-Fi settings from SD");
   }
  }
 }
 // Only back up a proven connection, never a failed or still staged change.
 char connected[33]={0};wifi->wifi_ssid_to(connected);auto current=wifi->get_sta();
 if(wifi->is_connected() && current.get_ssid()==connected && connected[0]){
  pax_wifi_request_t request{};request.magic=PAX_WIFI_SETTINGS_MAGIC;request.replace_password=1;
  std::strncpy(request.ssid,current.get_ssid().c_str(),32);std::strncpy(request.password,current.get_password().c_str(),64);
  if(recovered || !present || std::strcmp(saved.ssid,request.ssid) || std::strcmp(saved.password,request.password)){
   mkdir("/sdcard/roms",0700);mkdir("/sdcard/roms/pax",0700);
   if(pax_wifi_backup::save(BACKUP,request))ESP_LOGI("pax_wifi","Backed up working Wi-Fi settings to SD");
   else ESP_LOGW("pax_wifi","Could not save Wi-Fi backup to SD; will retry");
  }
  pax_wifi_wipe(&request,sizeof(request));
 }
 pax_wifi_wipe(&saved,sizeof(saved));
}
// Discard SNTP's offline retry backoff as soon as Wi-Fi has an IP.
// The ESP-IDF wrapper schedules the restart on the TCP/IP task.
static void refresh_time_after_reconnect(bool connected) {
#if defined(USE_ESP32) && defined(SNTP_SERVER_COUNT)
 static bool was_connected=false;
 if(connected && !was_connected){
  if(esp_sntp_restart())ESP_LOGI("pax_clock","Wi-Fi connected; requesting time sync");
  else ESP_LOGW("pax_clock","Time sync service not initialized");
 }
 was_connected=connected;
#else
 (void)connected;
#endif
}
static pax_wifi_status_t snapshot{PAX_WIFI_SETTINGS_MAGIC,0,0,{0},0,0,{0}};
struct Stream { bool writing; size_t offset; pax_wifi_request_t request; pax_wifi_status_t response; };
static ssize_t read(void *cookie,char *out,size_t count){auto *s=static_cast<Stream *>(cookie);size_t n=std::min(static_cast<size_t>(count),sizeof(s->response)-s->offset);std::memcpy(out,reinterpret_cast<char *>(&s->response)+s->offset,n);s->offset+=n;return n;}
static ssize_t write(void *cookie,const char *in,size_t count){auto *s=static_cast<Stream *>(cookie);if(static_cast<size_t>(count)>sizeof(s->request)-s->offset)return -1;std::memcpy(reinterpret_cast<char *>(&s->request)+s->offset,in,count);s->offset+=count;return count;}
static int close(void *cookie){auto *s=static_cast<Stream *>(cookie);int result=0;
 if(s->writing){
  if(s->offset!=sizeof(s->request)||!pax_wifi_request_valid(&s->request))result=-1;
  else {portENTER_CRITICAL(&lock);if(queued || snapshot.status==1 || pax_wifi_scan::busy())result=-1;else{pending=s->request;queued=true;snapshot.status=1;}portEXIT_CRITICAL(&lock);}
 }
 pax_wifi_wipe(s,sizeof(*s));std::free(s);return result;
}
static FILE *open(const char *mode){
 if(!mode || (std::strcmp(mode,"rb") && std::strcmp(mode,"wb")))return nullptr;
 auto *s=static_cast<Stream *>(std::calloc(1,sizeof(Stream)));if(!s)return nullptr;s->writing=mode[0]=='w';
 portENTER_CRITICAL(&lock);s->response=snapshot;portEXIT_CRITICAL(&lock);
#ifdef ESP_PLATFORM
 FILE *f=funopen(s,s->writing ? nullptr : +[](void *cookie,void *out,size_t count)->ssize_t{return read(cookie,static_cast<char *>(out),count);},s->writing ? +[](void *cookie,const void *in,size_t count)->ssize_t{return write(cookie,static_cast<const char *>(in),count);} : nullptr,nullptr,close);
#else
 cookie_io_functions_t io{};io.read=s->writing?nullptr:read;io.write=s->writing?write:nullptr;io.close=close;
 FILE *f=fopencookie(s,mode,io);
#endif
 if(!f){std::free(s);return nullptr;}setvbuf(f,nullptr,_IONBF,0);return f;
}
/* Run all Wi-Fi transitions in ESPHome's loop, never on the PAX worker. */
static void loop(){
 auto *wifi=esphome::wifi::global_wifi_component;if(!wifi)return;
 static bool changing=false,previous_connected=false,waiting_target=false;static int64_t deadline=0;static esphome::wifi::WiFiAP previous,target;
 pax_wifi_request_t request{};bool apply=false;
 portENTER_CRITICAL(&lock);if(queued){request=pending;pax_wifi_wipe(&pending,sizeof(pending));queued=false;apply=true;}portEXIT_CRITICAL(&lock);
 pax_wifi_scan::loop(wifi,changing || apply);
 maintain_backup(wifi,changing,apply);
 if(apply){previous_connected=wifi->is_connected();previous=wifi->get_sta();target=esphome::wifi::WiFiAP{};target.set_ssid(request.ssid);target.set_password(request.replace_password ? esphome::StringRef(request.password) : previous.get_password());
  // enable() reloads saved credentials. If initialization is needed, do it
  // before installing the unsaved target; never reload preferences after it.
  if(wifi->is_disabled())wifi->enable();
  wifi->set_sta(target);wifi->start_connecting(target);
  deadline=esp_timer_get_time()+45000000;changing=true;waiting_target=true;
  pax_wifi_wipe(&request,sizeof(request));
 }
 int status=0;
 if(waiting_target){char ssid[33]={0};wifi->wifi_ssid_to(ssid);
  if(wifi->is_connected() && target.get_ssid()==ssid){wifi->save_wifi_sta(target.get_ssid(),target.get_password());changing=false;waiting_target=false;status=2;}
  else if(changing && esp_timer_get_time()>=deadline){
   changing=false;
   if(previous_connected){wifi->set_sta(previous);wifi->start_connecting(previous);waiting_target=false;status=-1;}
   else status=-2; // Keep retrying the selected network; no working link to restore.
  }
 }
 refresh_time_after_reconnect(wifi->is_connected());
 static int64_t last_refresh=-1000000;
 const int64_t now=esp_timer_get_time();
 if(!status && now-last_refresh<1000000)return;
 last_refresh=now;
 auto current=wifi->get_sta();
 uint32_t channel=0,frequency=0;char security[48]={0},connected_ssid[33]={0};
#ifdef ESP_PLATFORM
 wifi_ap_record_t ap{};
 if(wifi->is_connected() && esp_wifi_sta_get_ap_info(&ap)==ESP_OK){
  std::memcpy(connected_ssid,ap.ssid,32);connected_ssid[32]=0;
  channel=ap.primary;frequency=pax_wifi_channel_mhz(channel);
  const char *auth=pax_wifi_auth_name(ap.authmode),*cipher=pax_wifi_cipher_name(ap.pairwise_cipher);
  std::snprintf(security,sizeof(security),"%s%s%s",auth,*cipher ? " / " : "",cipher);
 }
#endif
 portENTER_CRITICAL(&lock);snapshot.magic=PAX_WIFI_SETTINGS_MAGIC;std::strncpy(snapshot.ssid,connected_ssid[0] ? connected_ssid : current.get_ssid().c_str(),32);snapshot.ssid[32]=0;snapshot.password_set=!current.get_password().empty();snapshot.channel=channel;snapshot.frequency_mhz=frequency;std::memcpy(snapshot.security,security,sizeof(security));if(status)snapshot.status=status;portEXIT_CRITICAL(&lock);
 if(!waiting_target){previous.set_password("");target.set_password("");}
}
}
#else
namespace pax_wifi { static FILE *open(const char *){return nullptr;} static void loop(){} }
#endif
