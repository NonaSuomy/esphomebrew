from pathlib import Path
import subprocess
import tempfile
component = Path(__file__).resolve().parents[1]/'components/waveshare43'
with tempfile.TemporaryDirectory() as directory:
    t=Path(directory)
    def stub(name,text):
        p=t/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    stub('esphome/core/component.h', r"""#pragma once
#include <cstdint>
namespace esphome { class Component { public: virtual void setup(){} virtual float get_setup_priority() const {return 0;}
void mark_failed(){failed=true;} bool is_failed()const{return failed;} void status_set_warning(){} void status_clear_warning(){} bool failed=false; }; }
""")
    stub('esphome/core/log.h', '#pragma once\n#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n')
    stub('esphome/core/hal.h', '#pragma once\n#include <cstdint>\ninline void delay(int){} inline uint32_t millis(){return 0;}\n')
    stub('esphome/components/i2c/i2c.h', r"""#pragma once
#include <vector>
#include <utility>
#include <cstring>
namespace esphome::i2c {
inline uint8_t count=0; inline uint8_t record[6]={0x81,0xEC,0,0xDD,0xFF,0xFF};
inline std::vector<std::pair<unsigned,unsigned>> reads,writes;
class I2CDevice {public:
bool write_byte(uint8_t r,uint8_t v){writes.emplace_back(r,v);return true;}
bool read_byte(uint8_t r,uint8_t*p){return read_bytes(r,p,1);}
bool read_bytes(uint8_t r,uint8_t*p,unsigned n){reads.emplace_back(r,n);
if(r==0xA8){*p=0;return true;} if(r==2 && n==1){*p=count;return true;}
if(r==3 && n==6){std::memcpy(p,record,6);return true;} return false;}
}; }
""")
    stub('esphome/components/touchscreen/touchscreen.h',r"""#pragma once
#include "esphome/core/component.h"
#include <vector>
#include <array>
namespace esphome::touchscreen {class Touchscreen:public Component {public:
virtual void update_touches(){}; bool skip_update_=false; int x_raw_min_=0,x_raw_max_=799,y_raw_min_=0,y_raw_max_=479;
std::vector<std::array<int,3>> points; void add_raw_touch_position_(int id,int x,int y){points.push_back({id,x,y});}
}; }
""")
    stub('test.cpp',r"""#include <cassert>
#include "waveshare43.h"
using namespace esphome; using namespace esphome::i2c;
class Touch:public waveshare43::Touch {public: using waveshare43::Touch::update_touches;};
int main(){
Touch t; t.setup();reads.clear();
t.update_touches(); assert(reads.size()==1 && reads[0]==std::make_pair(2U,1U));assert(t.points.empty());
reads.clear();count=1;t.update_touches();assert(reads.size()==2 && reads[0]==std::make_pair(2U,1U) && reads[1]==std::make_pair(3U,6U));
assert((t.points[0]==std::array<int,3>{0,492,221}));
t.points.clear();count=15;reads.clear();t.update_touches();assert(t.points.empty()&&t.skip_update_&&reads.size()==1);
count=1;record[2]=0xF0;t.update_touches();assert(t.points.empty());
waveshare43::Control c;c.setup();const std::vector<std::pair<unsigned,unsigned>> expected={{0x86,0},{0x85,0},{0x83,0},{0x81,4},{0x82,0x80},{0x83,1},{0x86,0x80}};
assert(writes==expected);writes.clear();c.finish_setup();assert(writes[0]==std::make_pair(0x83U,0x0DU)&&writes[1]==std::make_pair(0x83U,0x0FU));
c.set_brightness(0.25f);assert(writes.back()==std::make_pair(0x86U,64U));c.set_brightness(0.75f);assert(writes.back()==std::make_pair(0x86U,191U));
}
""")
    subprocess.run(['g++','-std=c++17','-I'+str(t),'-I'+str(component),str(t/'test.cpp'),str(component/'waveshare43.cpp'),'-o',str(t/'test')],check=True)
    subprocess.run([str(t/'test')],check=True)
print('PASS Waveshare43: direct status/block reads, coordinate decode, invalid data, power/reset and brightness')
