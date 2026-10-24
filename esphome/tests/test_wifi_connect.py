"""Runtime credentials must survive startup, and offline changes must not roll back."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class WifiConnect(unittest.TestCase):
 def test_selected_target_success_offline_timeout_late_success_and_rollback(self):
  text=(ROOT/'components/pax_loader/pax_wifi_stream.h').read_text()
  loop=text[text.index('static void loop(){'):text.index('\n}\n#else\nnamespace pax_wifi')]
  code=r'''
#include <cassert>
#include <cstring>
#include <string>
#include "pax_wifi_settings.h"
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static int lock;static int64_t now;
static int64_t esp_timer_get_time(){return now;}
namespace esphome {using StringRef=std::string;namespace wifi {
struct WiFiAP {
 std::string ssid,password;
 void set_ssid(const std::string &s){ssid=s;}void set_password(const std::string &s){password=s;}
 const std::string &get_ssid()const{return ssid;}const std::string &get_password()const{return password;}
};
struct WiFiComponent {
 WiFiAP current{"old-away-network","old-password"},saved=current;
 bool connected=false,disabled=false;std::string actual;int writes=0,enables=0,attempts=0;
 bool is_connected(){return connected;}bool is_disabled(){return disabled;}
 WiFiAP get_sta(){return current;}void set_sta(const WiFiAP &ap){current=ap;}
 void enable(){enables++;disabled=false;current=saved;} // Production start() reloads NVS.
 void start_connecting(const WiFiAP &ap){assert(current.ssid==ap.ssid);connected=false;actual.clear();attempts++;}
 void wifi_ssid_to(char *out){strcpy(out,actual.c_str());}
 void save_wifi_sta(const std::string &ssid,const std::string &password){saved={ssid,password};current=saved;writes++;}
};
static WiFiComponent device;static WiFiComponent *global_wifi_component=&device;
}}
namespace pax_wifi_scan {static void loop(esphome::wifi::WiFiComponent *,bool){}}
namespace pax_wifi {
static pax_wifi_request_t pending;static bool queued;static pax_wifi_status_t snapshot;
static void maintain_backup(esphome::wifi::WiFiComponent *,bool,bool){}
static void refresh_time_after_reconnect(bool){}
'''+loop+r'''
static void request(const char *ssid,const char *password){
 pending={};pending.magic=PAX_WIFI_SETTINGS_MAGIC;pending.replace_password=1;
 strcpy(pending.ssid,ssid);strcpy(pending.password,password);queued=true;snapshot.status=1;now+=1000000;loop();
}
}
int main(){
 using namespace pax_wifi;auto &wifi=esphome::wifi::device;
 request("nearby-network","new-password");assert(wifi.current.ssid=="nearby-network" && wifi.current.password=="new-password" && wifi.writes==0 && wifi.enables==0);
 now+=46000000;loop();assert(snapshot.status==-2 && wifi.current.ssid=="nearby-network" && wifi.writes==0);
 // A late DHCP success still saves and backs up the selected network.
 wifi.connected=true;wifi.actual="nearby-network";now+=1000000;loop();assert(snapshot.status==2 && wifi.writes==1 && wifi.saved.ssid=="nearby-network");
 // Changes from a proven live link retain the existing recovery behavior.
 request("unreachable-network","wrong-password");now+=46000000;loop();assert(snapshot.status==-1 && wifi.current.ssid=="nearby-network" && wifi.writes==1);
 // Disabled Wi-Fi can reload NVS only before the new target is installed.
 wifi.disabled=true;wifi.connected=false;request("another-network","another-password");assert(wifi.enables==1 && wifi.current.ssid=="another-network" && wifi.writes==1);
 wifi.connected=true;wifi.actual="another-network";now+=1000000;loop();assert(snapshot.status==2 && wifi.saved.ssid=="another-network" && wifi.writes==2);
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'test.cpp').write_text(code)
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'components/pax_loader'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
