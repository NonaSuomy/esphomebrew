"""Fault-inject the same streaming transaction used by the on-device updater."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class FirmwareUpdateTests(unittest.TestCase):
    def test_only_verified_complete_image_becomes_bootable(self):
        with tempfile.TemporaryDirectory() as folder:
            executable=Path(folder)/'firmware-update-test'
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'components/pax_loader'),
                str(ROOT/'tests/components/pax_loader/firmware_update_test.cpp'),'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True)
    def test_settings_backend_is_reused_and_values_restore(self):
        source=(ROOT/'components/pax_loader/pax_loader.cpp').read_text()
        def block(start):
            first=source.index('{',start); depth=1; end=first+1
            while depth:
                if source[end]=='{': depth+=1
                elif source[end]=='}': depth-=1
                end+=1
            return source[start:end]
        setup=block(source.index('if(esphome::global_preferences){'))
        start=source.index('  SavedRuntimeSettings current{')
        end=source.index('  pax_wifi::loop();',start)
        loop=source[start:end]
        definitions=source[source.index('struct SavedRuntimeSettings'):source.index('static int32_t s_brightness')]
        test=r'''#include <cassert>
#include <cstdint>
#include <cstring>
#define ESP_LOGI(...) ((void)0)
struct Value { uint32_t magic;uint8_t volume,automatic,reserved[2]; };
static Value stored{0x50524631U,55,0,{0,0}};
namespace esphome {
struct ESPPreferenceObject {
 template<class T> bool load(T *out){memcpy(out,&stored,sizeof(*out));return true;}
 template<class T> bool save(T *in){memcpy(&stored,in,sizeof(*in));return true;}
};
struct Preferences {int allocations=0;template<class T> ESPPreferenceObject make_preference(uint32_t,bool){allocations++;return {};}};
Preferences instance;Preferences *global_preferences=&instance;
}
'''+definitions+r'''
struct Loader {
 int master_volume_=70;bool auto_brightness_enabled_=false;
 void set_auto_brightness_enabled(bool value){auto_brightness_enabled_=value;}
 void setup(){'''+setup+r'''}
 void loop(){'''+loop+r'''}
};
int main(){
 Loader loader;loader.setup();assert(loader.master_volume_==55);
 for(int i=0;i<1000;i++){loader.master_volume_=i%101;loader.loop();}
 assert(esphome::instance.allocations==1);
 Loader reboot;reboot.setup();assert(reboot.master_volume_==loader.master_volume_);
 stored.magic=0;Loader invalid;invalid.setup();assert(invalid.master_volume_==70);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder);(path/'test.cpp').write_text(test)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
    def test_brightness_reads_the_restored_backlight(self):
        source=(ROOT/'components/pax_loader/pax_loader.cpp').read_text()
        start=source.index('int32_t PaxLoader::svc_settings_brightness_get()')
        method=source[start:source.index('\n}',start)+2]
        code=r'''#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#define PAX_LOADER_USE_LVGL 1
static int32_t s_brightness=100;
struct Number {float state=85;};
struct PaxLoader {static PaxLoader *active_;Number *catalog_brightness_number_;static int32_t svc_settings_brightness_get();};
PaxLoader *PaxLoader::active_=nullptr;
'''+method+r'''
int main(){
 Number number;PaxLoader loader{&number};PaxLoader::active_=&loader;
 assert(PaxLoader::svc_settings_brightness_get()==85);
 number.state=5;assert(PaxLoader::svc_settings_brightness_get()==5);
 number.state=NAN;assert(PaxLoader::svc_settings_brightness_get()==5);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder);(path/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
    def test_failed_settings_sync_prevents_installation(self):
        source=(ROOT/'components/pax_loader/pax_firmware_update.cpp').read_text()
        start=source.index('void loop(bool app_closed)')
        method=source[start:source.index('\n}',start)+2]
        code=r'''#include <cassert>
#include <cstdint>
#define PAX_FIRMWARE_WAITING 4U
#define PAX_FIRMWARE_INSTALLING 5U
#define PAX_FIRMWARE_ERROR 7U
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdPASS 1
int lock=0,tasks=0;int64_t reboot_at=0;unsigned state=4;
bool pending(){return state==4;}
void report(unsigned next,const char *){state=next;}
void install_worker(void *){}
int xTaskCreate(void (*)(void *),const char *,int,void *,int,void *){tasks++;return 1;}
int64_t esp_timer_get_time(){return 0;}
namespace esphome {
struct Prefs {bool ok=true;int calls=0;bool sync(){calls++;return ok;}} prefs;
Prefs *global_preferences=&prefs;
struct Application {int reboots=0;void safe_reboot(){reboots++;}} App;
}
'''+method+r'''
int main(){
 loop(false);assert(!tasks && !esphome::prefs.calls);
 esphome::prefs.ok=false;loop(true);assert(!tasks && state==7 && !esphome::App.reboots);
 state=4;esphome::global_preferences=nullptr;loop(true);assert(!tasks && state==7);
 state=4;esphome::global_preferences=&esphome::prefs;esphome::prefs.ok=true;
 loop(true);assert(tasks==1 && state==5 && !esphome::App.reboots);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder);(path/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
    def test_network_preflight_uses_active_dns_before_starting_worker(self):
        source=(ROOT/'components/pax_loader/pax_loader.cpp').read_text()
        start=source.index('bool PaxLoader::request_firmware_update(')
        method=source[start:source.index('\n}',start)+2]
        code=r'''#include <cassert>
#include <cstdio>
#include "pax_firmware_update.h"
int restored=0;
bool restore_active_dns_if_missing_(){restored++;return true;}
namespace pax_c6 {bool busy(){return false;}}
namespace pax_firmware {FILE *open(const char *){assert(restored==1);return tmpfile();}}
struct PaxLoader {bool install_task_handle_=false,web_storage_busy_=false;bool request_firmware_update(uint32_t,uint64_t);};
'''+method+r'''
int main(){PaxLoader loader;assert(loader.request_firmware_update(PAX_FIRMWARE_CHECK,0));
 loader.web_storage_busy_=true;assert(!loader.request_firmware_update(PAX_FIRMWARE_CHECK,0));assert(restored==1);}
'''
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder);(path/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'components/pax_loader'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
        start=source.index('void *PaxLoader::svc_file_open(')
        prefix=source[start:source.index('PAX_C6_UPDATE_FILE',start)]
        self.assertLess(prefix.index('restore_active_dns_if_missing_'),prefix.index('pax_firmware::open'))
if __name__=='__main__': unittest.main()
