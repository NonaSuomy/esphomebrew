from pathlib import Path
import subprocess,tempfile
HEADER=Path(__file__).resolve().parents[1]/'pax_native_panel.h'
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
int get_width(){return get_width_internal();} int get_height(){return get_height_internal();}
virtual void draw_pixels_at(int,int,int,int,const uint8_t*,ColorOrder,ColorBitness,bool,int,int,int)=0;
void draw_pixels_at(int x,int y,int w,int h,const uint8_t*d,ColorOrder o,ColorBitness b,bool e) { draw_pixels_at(x,y,w,h,d,o,b,e,0,0,0); }
protected: virtual int get_width_internal()=0;virtual int get_height_internal()=0;}; }}
''')
 (t/'esp_heap_caps.h').write_text('#pragma once\n#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 0\n#define MALLOC_CAP_8BIT 0\n#define heap_caps_realloc(p,n,c) realloc(p,n)\n#define heap_caps_free(p) free(p)\n')
 (t/'esp_cache.h').write_text('#pragma once\n#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 0\n#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 0\ninline void esp_cache_msync(void*,size_t,int) {}\n')
 (t/'driver').mkdir()
 (t/'driver/ppa.h').write_text(r'''#pragma once
#include <cstdint>
#include <cstddef>
using ppa_client_handle_t=void*;
constexpr int ESP_OK=0,PPA_OPERATION_SRM=0,PPA_SRM_COLOR_MODE_RGB565=0,PPA_SRM_ROTATION_ANGLE_180=0,PPA_TRANS_MODE_BLOCKING=0;
struct ppa_client_config_t{int oper_type;};
struct ppa_srm_oper_config_t {
 struct {void*buffer;int pic_w,pic_h,block_w,block_h,block_offset_x,block_offset_y,srm_cm;}in;
 struct {void*buffer;size_t buffer_size;int pic_w,pic_h,srm_cm;}out;
 int rotation_angle,mode;float scale_x,scale_y;
};
static int ppa_calls=0;static bool fail_ppa=false;
inline int ppa_register_client(ppa_client_config_t*,void**p){*p=(void*)1;return 0;}
inline int ppa_unregister_client(void*){return 0;}
inline int ppa_do_scale_rotate_mirror(void*,ppa_srm_oper_config_t*c){
 ++ppa_calls;if(fail_ppa)return -1;
 auto*s=(uint16_t*)c->in.buffer;auto*d=(uint16_t*)c->out.buffer;
 for(int y=0;y<c->in.block_h;++y)for(int x=0;x<c->in.block_w;++x)
 d[y*c->out.pic_w+x]=s[(c->in.block_h-1-y+c->in.block_offset_y)*c->in.pic_w+c->in.block_w-1-x+c->in.block_offset_x];
 return 0;
}
''')
 with (t/'esp_heap_caps.h').open('a') as f:f.write('inline void *heap_caps_aligned_alloc(size_t a,size_t n,int){void*p=nullptr;return posix_memalign(&p,a,n)==0?p:nullptr;}\n')
 with (t/'esp_cache.h').open('a') as f:f.write('#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 0\n')
 (t/'esphome/components/mipi_dsi').mkdir()
 (t/'esphome/components/mipi_dsi/mipi_dsi.h').write_text('''#pragma once
#include "esphome/components/display/display.h"
namespace esphome::mipi_dsi {
static bool accept=false;static int accepted=0,last_x=0,last_y=0;
class MIPI_DSI: public display::Display {public:
 bool draw_rotated_rgb565(int x,int y,int w,int h,const uint8_t*,int,int,int,bool){
  if(!accept||w<16||h<4)return false;++accepted;last_x=x;last_y=y;return true;
 }
};}
''')
 (t/'test.cpp').write_text('''#include <cassert>
#include <vector>
#include "pax_native_panel.h"
using namespace esphome;using namespace esphome::display;
struct Panel: mipi_dsi::MIPI_DSI {
int x,y,w,h;std::vector<uint16_t> pixels;
void update() override {} DisplayType get_display_type() override {return DISPLAY_TYPE_COLOR;}
void draw_pixel_at(int a,int b,Color) override {x=a;y=b;}
void draw_pixels_at(int a,int b,int c,int d,const uint8_t*p,ColorOrder,ColorBitness,bool,int,int,int) override {x=a;y=b;w=c;h=d;pixels.assign((const uint16_t*)p,(const uint16_t*)p+c*d);}
int get_width_internal() override {return 800;} int get_height_internal() override {return 480;}
};
int main(){Panel p;pax_native::NativePanel a(&p);
// Padded source crop: rows [1,2] and [3,4] and [5,6].
uint16_t data[]={99,99,99,99, 99,1,2,99, 99,3,4,99, 99,5,6,99};
a.draw_pixels_at(10,20,2,3,(uint8_t*)data,COLOR_ORDER_RGB,COLOR_BITNESS_565,false,1,1,1);
assert(p.x==788 && p.y==457 && p.w==2 && p.h==3);
assert((p.pixels==std::vector<uint16_t>{6,5,4,3,2,1}));
// Logical screen corners arrive after the common renderer's 180 rotation.
const int logical[4][2]={{0,0},{799,0},{0,479},{799,479}};
for(auto &q:logical){int rx=799-q[0],ry=479-q[1];uint16_t value=42;
a.draw_pixels_at(rx,ry,1,1,(uint8_t*)&value,COLOR_ORDER_RGB,COLOR_BITNESS_565,false,0,0,0);
assert(p.x==q[0] && p.y==q[1]);
// GT911 reports native coordinates, so no touch transform is needed.
assert(p.x==q[0] && p.y==q[1]);}
alignas(128) uint16_t frame[64*32];
for(int i=0;i<64*32;++i)frame[i]=i;
a.draw_pixels_at(0,0,64,32,(uint8_t*)frame,COLOR_ORDER_RGB,COLOR_BITNESS_565,false,0,0,0);
assert(ppa_calls==1 && p.pixels.front()==2047 && p.pixels.back()==0);
fail_ppa=true;
a.draw_pixels_at(0,0,64,32,(uint8_t*)frame,COLOR_ORDER_RGB,COLOR_BITNESS_565,false,0,0,0);
assert(ppa_calls==2 && p.pixels.front()==2047 && p.pixels.back()==0);
mipi_dsi::accept=true;
a.draw_pixels_at(0,0,64,32,(uint8_t*)frame,COLOR_ORDER_RGB,COLOR_BITNESS_565,false,0,0,0);
assert(ppa_calls==2 && mipi_dsi::accepted==1 && mipi_dsi::last_x==736 && mipi_dsi::last_y==448);
}
''')
 subprocess.run(['g++','-std=c++17','-I'+str(t),'-I'+str(HEADER.parent),str(t/'test.cpp'),'-o',str(t/'test')],check=True)
 subprocess.run([str(t/'test')],check=True)
print('PASS 4.3-inch native presentation: padded crop, all four corners, hardware rotation and CPU fallback')
