"""Compile the native interface selector against deterministic driver APIs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'components/pax_loader/pax_loader.cpp').read_text()
START = SOURCE.index('static int native_network_mode =')
END = SOURCE.index('#ifdef PAX_LOADER_USE_LVGL', START)

STUB = r'''
#include <cassert>
#include <cstdint>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
struct esp_netif_t{};
static esp_netif_t wifi_netif,eth_netif;static esp_netif_t *route=nullptr;
static void esp_netif_set_default_netif(esp_netif_t *n){route=n;}
namespace pax_c6 {static bool updating=false;static bool busy(){return updating;}}
namespace pax_firmware {static bool updating=false;static bool busy(){return updating;}}
namespace pax_wifi_scan {static bool scanning=false;static bool busy(){return scanning;}}
namespace pax_wifi {static bool queued=false;static struct {int status;}snapshot{};}
struct Driver {
 bool disabled=false,failed=false;
 bool is_failed(){return failed;}bool is_disabled(){return disabled;}
 void enable(){if(!failed)disabled=false;}void disable(){disabled=true;}
 esp_netif_t *get_esp_netif(){return &eth_netif;}
 esp_netif_t *get_esp_netif_sta(){return &wifi_netif;}
};
namespace esphome {
namespace wifi {static Driver driver;static Driver *global_wifi_component=&driver;}
namespace ethernet {static Driver driver;static Driver *global_eth_component=&driver;}
struct Preferences {
 bool save_ok=true,sync_ok=true;int last_mode=-1,syncs=0;
 struct Object {Preferences *owner;template<class T>bool save(T *saved){if(owner->save_ok)owner->last_mode=saved->mode;return owner->save_ok;}};
 template<class T>Object make_preference(uint32_t,bool){return {this};}
 bool sync(){syncs++;return sync_ok;}
};
static Preferences preferences;static Preferences *global_preferences=&preferences;
}
using namespace esphome;
'''

class NetworkInterface(unittest.TestCase):
    def test_modes_busy_operations_and_persistence(self):
        main = r'''
int main(){
 assert(native_network_capabilities()==3);
 assert(native_network_select(1)==0 && native_network_mode==1);
 assert(!wifi::driver.disabled && ethernet::driver.disabled && route==&wifi_netif);
 assert(preferences.last_mode==1);
 assert(native_network_select(2)==0 && native_network_mode==2);
 assert(wifi::driver.disabled && !ethernet::driver.disabled && route==&eth_netif);
 assert(preferences.last_mode==2);
 assert(native_network_select(0)==0 && !wifi::driver.disabled && !ethernet::driver.disabled);
 for(int operation=0;operation<5;operation++){
  pax_wifi::queued=operation==0;pax_wifi::snapshot.status=operation==1?1:0;
  pax_wifi_scan::scanning=operation==2;pax_c6::updating=operation==3;pax_firmware::updating=operation==4;
  assert(native_network_select(2)==-2 && native_network_mode==0);
  assert(!wifi::driver.disabled && !ethernet::driver.disabled);
 }
 pax_wifi::queued=false;pax_wifi::snapshot.status=0;pax_wifi_scan::scanning=false;pax_c6::updating=false;pax_firmware::updating=false;
 assert(native_network_select(-1)==-1 && native_network_select(3)==-1);
 ethernet::driver.failed=true;
 assert(native_network_capabilities()==1 && native_network_select(2)==-1);
 ethernet::driver.failed=false;
 preferences.save_ok=false;
 assert(native_network_select(2)==-1 && native_network_mode==0);
 assert(!wifi::driver.disabled && !ethernet::driver.disabled);
 preferences.save_ok=true;preferences.sync_ok=false;
 assert(native_network_select(1)==-1 && native_network_mode==0);
 preferences.sync_ok=true;global_preferences=nullptr;
 assert(native_network_select(1)==-1 && native_network_mode==0);
}
'''
        self.compile_run(['USE_WIFI', 'USE_ETHERNET'], main)

    def test_single_interface_profiles_reject_unavailable_choice(self):
        for define, mode, other, caps in [('USE_WIFI', 1, 2, 1), ('USE_ETHERNET', 2, 1, 2)]:
            self.compile_run([define], f'''int main(){{
assert(native_network_capabilities()=={caps});
assert(native_network_select({other})==-1 && native_network_mode==0);
assert(native_network_select({mode})==0 && native_network_mode=={mode});
}}''')

    def compile_run(self, defines, main):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)
            (path/'test.cpp').write_text(STUB+SOURCE[START:END]+main)
            subprocess.run(['c++','-std=c++17', *['-D'+d for d in defines], str(path/'test.cpp'), '-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

if __name__=='__main__':
    unittest.main()
