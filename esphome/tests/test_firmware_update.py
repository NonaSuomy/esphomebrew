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
if __name__=='__main__': unittest.main()
