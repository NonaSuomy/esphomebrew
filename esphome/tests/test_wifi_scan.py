"""Exercise the scan virtual FILE, driver handoff and retained result publication."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class WifiScan(unittest.TestCase):
 def test_async_scan_deduplication_empty_error_and_timeout(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'esphome/components/wifi').mkdir(parents=True)
   (p/'esphome/core').mkdir()
   (p/'esphome/core/log.h').write_text('#pragma once\n#define ESP_LOGD(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n')
   (p/'esphome/components/wifi/wifi_component.h').write_text(r'''
#pragma once
#include <vector>
#include <string>
namespace esphome::wifi {
struct AP {std::string ssid;int rssi,channel;bool secured;
 const std::string &get_ssid() const{return ssid;}int get_rssi() const{return rssi;}
 int get_channel() const{return channel;}bool get_with_auth() const{return secured;}};
struct WiFiComponent {bool disabled=false;std::vector<AP> results;
 bool is_disabled() const{return disabled;}const std::vector<AP> &get_scan_result() const{return results;}};
}
''')
   (p/'esp_wifi.h').write_text(r'''
#pragma once
#include <stdint.h>
using esp_event_base_t=int;constexpr int ESP_OK=0,WIFI_EVENT=1,WIFI_EVENT_SCAN_DONE=2,WIFI_SCAN_TYPE_ACTIVE=3;
struct wifi_event_sta_scan_done_t {uint32_t status;};
struct wifi_scan_config_t {bool show_hidden;int scan_type;struct {struct {int min,max;}active;}scan_time;};
static int starts,result;static int64_t now;
static int64_t esp_timer_get_time(){return now;}
static int esp_wifi_scan_start(wifi_scan_config_t *c,bool block){assert(!block && c->show_hidden);starts++;return result;}
''')
   (p/'esp_event.h').write_text(r'''
#pragma once
static int registrations;
static int esp_event_handler_register(int,int,void (*)(void *,int,int32_t,void *),void *){registrations++;return 0;}
''')
   code=r'''
#include <cassert>
#include <cstring>
#define USE_WIFI
#define USE_ESP32
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#include "pax_wifi_scan_stream.h"
using namespace pax_wifi_scan;
static int request(uint32_t magic=PAX_WIFI_SCAN_MAGIC){FILE *f=pax_wifi_scan::open("wb");assert(f);pax_wifi_scan_request_t q{magic};assert(fwrite(&q,1,sizeof(q),f)==sizeof(q));return fclose(f);}
static pax_wifi_scan_status_t status(){FILE *f=pax_wifi_scan::open("rb");assert(f);pax_wifi_scan_status_t s{};assert(fread(&s,1,sizeof(s),f)==sizeof(s));assert(fgetc(f)==EOF);fclose(f);assert(pax_wifi_scan_valid(&s));return s;}
static void complete(esphome::wifi::WiFiComponent &wifi,uint32_t error=0){wifi_event_sta_scan_done_t event{error};finished_event(nullptr,0,0,&event);loop(&wifi,false);if(!error){assert(status().state==1);loop(&wifi,false);}}
int main(){
 static_assert(sizeof(pax_wifi_scan_ap_t)==48);
 esphome::wifi::WiFiComponent wifi;
 assert(status().state==0 && request(0)!=0 && !busy());
 assert(request()==0 && request()!=0 && busy());loop(&wifi,false);assert(starts==1 && registrations==1 && status().state==1);
 wifi.results={{"Same",-75,1,true},{"Same",-30,11,true},{"Open",-40,6,false},{"",-20,1,true}};
 complete(wifi);auto s=status();assert(s.state==2 && s.count==2 && !strcmp(s.aps[0].ssid,"Same") && s.aps[0].channel==11 && s.aps[1].secured==0);
 assert(request()==0);loop(&wifi,false);wifi.results.clear();complete(wifi);assert(status().state==2 && status().count==0);
 assert(request()==0);loop(&wifi,false);complete(wifi,1);assert(status().state==-1);
 assert(request()==0);loop(&wifi,false);now+=21000000;loop(&wifi,false);assert(status().state==-1);
 result=1;assert(request()==0);loop(&wifi,false);assert(status().state==-1);result=0;
 assert(request()==0);loop(&wifi,true);assert(status().state==-1);
 wifi.disabled=true;assert(request()==0);loop(&wifi,false);assert(status().state==-1);wifi.disabled=false;
 pax_wifi_scan_status_t capped{};capped.magic=PAX_WIFI_SCAN_MAGIC;capped.state=2;
 for(int i=0;i<60;i++){char name[33];snprintf(name,sizeof(name),"Network %d",i);pax_wifi_scan_add(&capped,name,-100+i,1,true);}
 assert(capped.count==24 && capped.aps[0].rssi==-41 && capped.aps[23].rssi==-64 && pax_wifi_scan_valid(&capped));
 capped.count=25;assert(!pax_wifi_scan_valid(&capped));
}
'''
   (p/'test.cpp').write_text(code)
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(p),'-I'+str(ROOT/'components/pax_loader'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
