#pragma once
#include "pax_wifi_scan.h"
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#if defined(USE_WIFI) && defined(USE_ESP32)
#include "esphome/components/wifi/wifi_component.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esphome/core/log.h"
namespace pax_wifi_scan {
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static pax_wifi_scan_status_t snapshot{PAX_WIFI_SCAN_MAGIC,0,0,0,{0},{}};
static bool queued=false, event_done=false, registered=false;
static uint32_t event_status=0;
static unsigned drain_loops=0;
static int64_t deadline=0;
static bool busy(){portENTER_CRITICAL(&lock);bool result=queued || snapshot.state==1;portEXIT_CRITICAL(&lock);return result;}
static void finished_event(void *,esp_event_base_t,int32_t,void *data){
 auto *event=static_cast<wifi_event_sta_scan_done_t *>(data);
 portENTER_CRITICAL(&lock);
 if(snapshot.state==1){event_status=event->status;event_done=true;}
 portEXIT_CRITICAL(&lock);
}
static void fail(const char *message){
 ESP_LOGW("pax_wifi_scan","%s",message);
 portENTER_CRITICAL(&lock);snapshot.state=-1;snapshot.count=0;
 std::snprintf(snapshot.message,sizeof(snapshot.message),"%s",message);snapshot.generation++;
 event_done=false;portEXIT_CRITICAL(&lock);drain_loops=0;
}
/* Runs on ESPHome's main loop. ESPHome alone consumes the IDF AP records;
 * read its retained, complete result vector after the queued Wi-Fi event has
 * had two main-loop passes to drain. Never change its connection state. */
static void loop(esphome::wifi::WiFiComponent *wifi,bool connecting){
 if(!registered){
  if(esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,finished_event,nullptr)!=ESP_OK){
   portENTER_CRITICAL(&lock);queued=false;portEXIT_CRITICAL(&lock);fail("Wi-Fi scan service unavailable; enter manually");return;
  }
  registered=true;
 }
 bool start=false,done=false;uint32_t status=0;
 portENTER_CRITICAL(&lock);if(queued){queued=false;start=true;}done=event_done;status=event_status;portEXIT_CRITICAL(&lock);
 if(start){
  if(!wifi || wifi->is_disabled()){fail("Wi-Fi is unavailable; enter the network name manually");return;}
  if(connecting){fail("Wi-Fi is connecting; try scanning again shortly");return;}
  wifi_scan_config_t config{};config.show_hidden=true;
  config.scan_type=WIFI_SCAN_TYPE_ACTIVE;config.scan_time.active.min=80;config.scan_time.active.max=160;
  portENTER_CRITICAL(&lock);event_done=false;portEXIT_CRITICAL(&lock);drain_loops=0;
  deadline=esp_timer_get_time()+20000000;
  auto error=esp_wifi_scan_start(&config,false);
  ESP_LOGD("pax_wifi_scan","Scan start: error=%d",static_cast<int>(error));
  if(error!=ESP_OK)fail("Wi-Fi is busy; try again or enter the name manually");
  return;
 }
 if(!busy())return;
 if(esp_timer_get_time()>=deadline){fail("Scan timed out; try again or enter the name manually");return;}
 if(!done)return;
 ESP_LOGD("pax_wifi_scan","Scan event: status=%u",static_cast<unsigned>(status));
 if(status){fail("Scan failed; try again or enter the name manually");return;}
 if(++drain_loops<2)return;
 pax_wifi_scan_status_t result{};result.magic=PAX_WIFI_SCAN_MAGIC;result.state=2;
 for(const auto &ap:wifi->get_scan_result())
  pax_wifi_scan_add(&result,ap.get_ssid().c_str(),ap.get_rssi(),ap.get_channel(),ap.get_with_auth());
 ESP_LOGD("pax_wifi_scan","Scan complete: retained=%u displayed=%u",static_cast<unsigned>(wifi->get_scan_result().size()),static_cast<unsigned>(result.count));
 std::snprintf(result.message,sizeof(result.message),result.count ? "Choose a network, or enter its name manually" : "No named networks found; rescan or enter manually");
 portENTER_CRITICAL(&lock);result.generation=snapshot.generation+1;snapshot=result;event_done=false;portEXIT_CRITICAL(&lock);drain_loops=0;
}
struct Stream {bool writing;size_t offset;pax_wifi_scan_request_t request;pax_wifi_scan_status_t response;};
static ssize_t read(void *cookie,char *out,size_t count){
 auto *s=static_cast<Stream *>(cookie);size_t n=std::min(count,sizeof(s->response)-s->offset);
 std::memcpy(out,reinterpret_cast<char *>(&s->response)+s->offset,n);s->offset+=n;return n;
}
static ssize_t write(void *cookie,const char *in,size_t count){
 auto *s=static_cast<Stream *>(cookie);if(count>sizeof(s->request)-s->offset)return -1;
 std::memcpy(reinterpret_cast<char *>(&s->request)+s->offset,in,count);s->offset+=count;return count;
}
static int close(void *cookie){
 auto *s=static_cast<Stream *>(cookie);int result=0;
 if(s->writing){
  if(s->offset!=sizeof(s->request) || s->request.magic!=PAX_WIFI_SCAN_MAGIC)result=-1;
  else {portENTER_CRITICAL(&lock);
   if(queued || snapshot.state==1)result=-1;
   else {snapshot.state=1;snapshot.count=0;snapshot.generation++;std::strcpy(snapshot.message,"Scanning nearby Wi-Fi networks...");queued=true;}
   portEXIT_CRITICAL(&lock);}
 }
 std::free(s);return result;
}
static FILE *open(const char *mode){
 if(!mode || (std::strcmp(mode,"rb") && std::strcmp(mode,"wb")))return nullptr;
 auto *s=static_cast<Stream *>(std::calloc(1,sizeof(Stream)));if(!s)return nullptr;s->writing=mode[0]=='w';
 portENTER_CRITICAL(&lock);s->response=snapshot;portEXIT_CRITICAL(&lock);
#ifdef ESP_PLATFORM
 FILE *f=funopen(s,s->writing?nullptr:+[](void *c,void *o,size_t n)->ssize_t{return read(c,static_cast<char *>(o),n);},s->writing?+[](void *c,const void *i,size_t n)->ssize_t{return write(c,static_cast<const char *>(i),n);}:nullptr,nullptr,close);
#else
 cookie_io_functions_t io{};io.read=s->writing?nullptr:read;io.write=s->writing?write:nullptr;io.close=close;
 FILE *f=fopencookie(s,mode,io);
#endif
 if(!f){std::free(s);return nullptr;}setvbuf(f,nullptr,_IONBF,0);return f;
}
}
#else
namespace pax_wifi_scan {static bool busy(){return false;}static FILE *open(const char *){return nullptr;}}
#endif
