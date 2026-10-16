from pathlib import Path
import subprocess,tempfile
HEADER=Path(__file__).resolve().parents[1]/'m3_lcd101_adapter.h'
with tempfile.TemporaryDirectory() as tmp:
 t=Path(tmp);(t/'esphome/components/display').mkdir(parents=True)
 (t/'esphome/components/display/display.h').write_text('''#pragma once
#include <cstdint>
#include <cstdlib>
#define ESP_LOGE(...) ((void)0)
namespace esphome { struct Color {}; namespace display {
enum ColorOrder { COLOR_ORDER_RGB }; enum ColorBitness { COLOR_BITNESS_565 }; enum DisplayType { DISPLAY_TYPE_COLOR };
class Display { public: ~Display()=default;virtual void update()=0; virtual DisplayType get_display_type()=0;
virtual void draw_pixel_at(int,int,Color)=0;
virtual void draw_pixels_at(int,int,int,int,const uint8_t*,ColorOrder,ColorBitness,bool,int,int,int)=0;
void draw_pixels_at(int x,int y,int w,int h,const uint8_t*d,ColorOrder o,ColorBitness b,bool e) { draw_pixels_at(x,y,w,h,d,o,b,e,0,0,0); }
protected: virtual int get_width_internal()=0;virtual int get_height_internal()=0;}; }}
''')
 (t/'esp_heap_caps.h').write_text('#pragma once\n#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 0\n#define MALLOC_CAP_8BIT 0\n#define heap_caps_realloc(p,n,c) realloc(p,n)\n#define heap_caps_free(p) free(p)\n')
 (t/'esp_cache.h').write_text('#pragma once\n#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 0\n#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 0\ninline void esp_cache_msync(void*,size_t,int) {}\n')
 (t/'test.cpp').write_text('''#include <cassert>
#include <vector>
#include "m3_lcd101_adapter.h"
using namespace esphome;using namespace esphome::display;
struct Panel: Display {
int x,y,w,h;std::vector<uint16_t> pixels;
void update() override {} DisplayType get_display_type() override {return DISPLAY_TYPE_COLOR;}
void draw_pixel_at(int a,int b,Color) override {x=a;y=b;}
void draw_pixels_at(int a,int b,int c,int d,const uint8_t*p,ColorOrder,ColorBitness,bool,int,int,int) override {x=a;y=b;w=c;h=d;pixels.assign((const uint16_t*)p,(const uint16_t*)p+c*d);}
int get_width_internal() override {return 800;} int get_height_internal() override {return 1280;}
};
int main(){Panel p;pax_m3::LandscapePanel a(&p);
// Padded source crop: rows [1,2] and [3,4] and [5,6].
uint16_t data[]={99,99,99,99, 99,1,2,99, 99,3,4,99, 99,5,6,99};
a.draw_pixels_at(10,20,2,3,(uint8_t*)data,COLOR_ORDER_RGB,COLOR_BITNESS_565,false,1,1,1);
assert(p.x==777 && p.y==10 && p.w==3 && p.h==2);
assert((p.pixels==std::vector<uint16_t>{5,3,1,6,4,2}));
// Logical screen corners arrive after the common renderer's 180 rotation.
const int logical[4][2]={{0,0},{1279,0},{0,799},{1279,799}};
for(auto &q:logical){int rx=1279-q[0],ry=799-q[1];uint16_t value=42;
a.draw_pixels_at(rx,ry,1,1,(uint8_t*)&value,COLOR_ORDER_RGB,COLOR_BITNESS_565,false,0,0,0);
assert(p.x==q[1] && p.y==1279-q[0]);
// Inverse mapping is the GT911 swap_xy + mirror_x transform.
assert(1279-p.y==q[0] && p.x==q[1]);}
}
''')
 subprocess.run(['g++','-std=c++17','-I'+str(t),'-I'+str(HEADER.parent),str(t/'test.cpp'),'-o',str(t/'test')],check=True)
 subprocess.run([str(t/'test')],check=True)
print('PASS M3 panel rotation: padded crop, all four display corners and inverse touch mapping')
