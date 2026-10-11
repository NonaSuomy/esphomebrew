#pragma once
#include "pax_wifi_settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#ifdef ESP_PLATFORM
#include "esp_wifi.h"
#endif
namespace pax_wifi {
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static pax_wifi_request_t pending{};
static bool queued=false;
static pax_wifi_status_t snapshot{PAX_WIFI_SETTINGS_MAGIC,0,0,{0},0,0,{0}};
struct Stream { bool writing; size_t offset; pax_wifi_request_t request; pax_wifi_status_t response; };
static ssize_t read(void *cookie,char *out,size_t count){auto *s=static_cast<Stream *>(cookie);size_t n=std::min(static_cast<size_t>(count),sizeof(s->response)-s->offset);std::memcpy(out,reinterpret_cast<char *>(&s->response)+s->offset,n);s->offset+=n;return n;}
static ssize_t write(void *cookie,const char *in,size_t count){auto *s=static_cast<Stream *>(cookie);if(static_cast<size_t>(count)>sizeof(s->request)-s->offset)return -1;std::memcpy(reinterpret_cast<char *>(&s->request)+s->offset,in,count);s->offset+=count;return count;}
static int close(void *cookie){auto *s=static_cast<Stream *>(cookie);int result=0;
 if(s->writing){
  if(s->offset!=sizeof(s->request)||!pax_wifi_request_valid(&s->request))result=-1;
  else {portENTER_CRITICAL(&lock);if(queued || snapshot.status==1)result=-1;else{pending=s->request;queued=true;snapshot.status=1;}portEXIT_CRITICAL(&lock);}
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
 static bool changing=false;static int64_t deadline=0;static esphome::wifi::WiFiAP previous,target;
 pax_wifi_request_t request{};bool apply=false;
 portENTER_CRITICAL(&lock);if(queued){request=pending;pax_wifi_wipe(&pending,sizeof(pending));queued=false;apply=true;}portEXIT_CRITICAL(&lock);
 if(apply){previous=wifi->get_sta();target=esphome::wifi::WiFiAP{};target.set_ssid(request.ssid);target.set_password(request.replace_password ? esphome::StringRef(request.password) : previous.get_password());
  wifi->disable();wifi->set_sta(target);wifi->enable();deadline=esp_timer_get_time()+45000000;changing=true;
  pax_wifi_wipe(&request,sizeof(request));
 }
 int status=0;
 if(changing){char ssid[33]={0};wifi->wifi_ssid_to(ssid);
  if(wifi->is_connected() && target.get_ssid()==ssid){wifi->save_wifi_sta(target.get_ssid(),target.get_password());changing=false;status=2;}
  else if(esp_timer_get_time()>=deadline){wifi->disable();wifi->set_sta(previous);wifi->enable();changing=false;status=-1;}
 }
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
 if(!changing){previous.set_password("");target.set_password("");}
}
}
#else
namespace pax_wifi { static FILE *open(const char *){return nullptr;} static void loop(){} }
#endif
