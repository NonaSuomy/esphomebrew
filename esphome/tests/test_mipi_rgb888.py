"""Exercise the actual MIPI presentation method with host driver stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
component = root / "dependencies/esphome/config/components/mipi_dsi"
source = (component / "mipi_dsi.cpp").read_text()
method = source.split("void MIPI_DSI::write_to_display_(", 1)[1].split("bool MIPI_DSI::check_buffer_()", 1)[0]
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    code = r'''#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "frame_buffer_handoff.h"
using esphome::mipi_dsi::FrameBufferHandoff;
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
int64_t esp_timer_get_time(){return 0;}
constexpr int ESP_OK=0, portMAX_DELAY=0;
using esp_err_t=int;
const char *esp_err_to_name(int) { return "error"; }
int xSemaphoreTake(int,int) { return 1; }
namespace display { constexpr int COLOR_BITNESS_565=1; }
int result=0, presentations=0, expected_width=3, expected_height=2;
const uint8_t *presented=nullptr;
esp_err_t esp_lcd_panel_draw_bitmap(int,int x,int y,int w,int h,const void *p) {
  assert(x==0 && y==0 && w==expected_width && h==expected_height);
  presented=static_cast<const uint8_t*>(p); ++presentations; return result;
}
using ppa_client_handle_t=void*;
constexpr int PPA_OPERATION_SRM=0,PPA_SRM_COLOR_MODE_RGB565=0,PPA_SRM_COLOR_MODE_RGB888=1,PPA_SRM_ROTATION_ANGLE_0=0,PPA_SRM_ROTATION_ANGLE_180=1,PPA_TRANS_MODE_BLOCKING=0;
constexpr int ESP_CACHE_MSYNC_FLAG_DIR_C2M=0,ESP_CACHE_MSYNC_FLAG_DIR_M2C=0,ESP_CACHE_MSYNC_FLAG_UNALIGNED=0;
void esp_cache_msync(void*,size_t,int){}
struct ppa_client_config_t{int oper_type;};
struct ppa_srm_oper_config_t {
 struct {void*buffer;int pic_w,pic_h,block_w,block_h,block_offset_x,block_offset_y,srm_cm;}in;
 struct {void*buffer;size_t buffer_size;int pic_w,pic_h,block_offset_x,block_offset_y,srm_cm;}out;
 int rotation_angle,mode;float scale_x,scale_y;
};
int ppa_calls=0;bool ppa_fail=false;
int ppa_register_client(ppa_client_config_t*,void**p){*p=(void*)1;return 0;}
int ppa_do_scale_rotate_mirror(void*,ppa_srm_oper_config_t*c){
 ++ppa_calls;if(ppa_fail)return 1;
 auto*s=(uint16_t*)c->in.buffer;auto*d=(uint8_t*)c->out.buffer;
 int ow=int(c->in.block_w*c->scale_x+.5f),oh=int(c->in.block_h*c->scale_y+.5f);
 for(int y=0;y<oh;++y)for(int x=0;x<ow;++x){
  int sy=int(y/c->scale_y),sx=int(x/c->scale_x);
  if(c->rotation_angle){sy=c->in.block_h-1-sy;sx=c->in.block_w-1-sx;}
  auto p=s[(sy+c->in.block_offset_y)*c->in.pic_w+sx+c->in.block_offset_x];
  auto*t=d+((y+c->out.block_offset_y)*c->out.pic_w+x+c->out.block_offset_x)*3;
  int b=p&31,g=(p>>5)&63,r=p>>11;t[0]=(b<<3)|(b>>2);t[1]=(g<<2)|(g>>4);t[2]=(r<<3)|(r>>2);
 }return 0;
}
class MIPI_DSI {
 public:
  bool draw_rotated_rgb565(int,int,int,int,const uint8_t*,int,int,int,bool);
  bool draw_scaled_rgb565(int,int,int,int,const uint16_t*,int,int,const uint8_t**);
  bool convert_rgb565_to_rgb888_(const uint8_t*,uint8_t*,int,int,int,int,int,int,int,bool,bool=false,int=0,int=0);
  ppa_client_handle_t rgb_conversion_client_=nullptr;
  bool wait_for_frame_buffer_() { return true; }
  void write_to_display_(int,int,int,int,const uint8_t*,int,int,int,bool=false);
  bool software_rgb888_=true, double_buffer_=true;
  int width_=3,height_=2,color_depth_=display::COLOR_BITNESS_565;
  int handle_=0,io_lock_=0,frame_buffer_lock_=0;
  FrameBufferHandoff frame_buffer_handoff_;
  uint8_t *frame_buffers_[2];
};
void MIPI_DSI::write_to_display_(''' + method + r'''
int main() {
  uint8_t first[18]{},second[18]{};
  MIPI_DSI d; d.frame_buffers_[0]=first; d.frame_buffers_[1]=second;
  const uint8_t pixels[]={0,0xF8,0xE0,7,0x1F,0,0xFF,0xFF,0,0,0xE0,0xFF};
  d.write_to_display_(0,0,3,2,pixels,0,0,0);
  const uint8_t expected[]={0,0,255,0,255,0,255,0,0,255,255,255,0,0,0,0,255,255};
  assert(presented==second && !std::memcmp(second,expected,18));
  d.frame_buffer_handoff_.submitted();
  assert(!d.frame_buffer_handoff_.frame_complete());
  assert(d.frame_buffer_handoff_.frame_complete());
  // Big-endian source, one padded pixel before the requested column.
  const uint8_t partial[]={0,0,0,0x1F};
  d.write_to_display_(1,0,1,1,partial,1,0,0,true);
  uint8_t changed[18]; std::memcpy(changed,expected,18);
  changed[3]=255;changed[4]=0;changed[5]=0;
  assert(presented==first && !std::memcmp(first,changed,18));
  result=1;d.write_to_display_(0,0,3,2,pixels,0,0,0);
  assert(!d.frame_buffer_handoff_.pending());
  const int before=presentations;
  d.write_to_display_(-1,0,3,2,pixels,0,0,0);
  assert(presentations==before);
  result=0;expected_width=32;expected_height=8;
  MIPI_DSI large;large.width_=32;large.height_=8;large.rgb_conversion_client_=(void*)1;
  alignas(128) uint8_t a[32*8*3]{},b[32*8*3]{};
  alignas(128) uint16_t frame[32*8];for(auto &p:frame)p=0xf800;
  large.frame_buffers_[0]=a;large.frame_buffers_[1]=b;
  large.write_to_display_(0,0,32,8,(uint8_t*)frame,0,0,0);
  assert(ppa_calls==1&&presented==b);
  for(int i=0;i<32*8;++i)assert(b[i*3]==0&&b[i*3+1]==0&&b[i*3+2]==255);
  large.frame_buffer_handoff_.cancel();ppa_fail=true;
  large.write_to_display_(0,0,32,8,(uint8_t*)frame,0,0,0);
  assert(ppa_calls==2);
  large.frame_buffer_handoff_.cancel();ppa_fail=false;frame[255]=0x001f;
  assert(large.draw_rotated_rgb565(0,0,32,8,(uint8_t*)frame,0,0,0,false));
  assert(ppa_calls==3&&b[0]==255&&b[1]==0&&b[2]==0&&b[765]==0&&b[766]==0&&b[767]==255);
  frame[255]=0xf800;
  assert(!large.draw_rotated_rgb565(0,0,32,8,(uint8_t*)frame,0,0,0,true));
  // Restore uniform colours for the original fallback comparison below.
  b[0]=0;b[1]=0;b[2]=255;
  for(int i=0;i<32*8;++i)assert(b[i*3]==0&&b[i*3+1]==0&&b[i*3+2]==255);
  alignas(128) uint16_t small[16*4];for(auto&p:small)p=0xf800;small[63]=0x001f;
  large.frame_buffer_handoff_.cancel();const uint8_t*front=nullptr;
  assert(large.draw_scaled_rgb565(0,0,32,8,small,16,4,&front));
  assert(front==b&&b[0]==0&&b[2]==255&&b[765]==255&&b[767]==0);
  large.frame_buffer_handoff_.cancel();small[63]=0xf800;
  assert(large.draw_scaled_rgb565(8,2,16,4,small,16,4,&front));
  for(int y=0;y<8;++y)for(int x=0;x<32;++x){
   auto*q=b+(y*32+x)*3;bool visible=y>=2&&y<6&&x>=8&&x<24;
   assert(q[0]==0&&q[1]==0&&q[2]==(visible?255:0));
  }
  large.frame_buffer_handoff_.cancel();ppa_fail=true;
  assert(!large.draw_scaled_rgb565(0,0,32,8,small,16,4,&front));
  assert(!large.frame_buffer_handoff_.pending());
}
'''
    (folder / "test.cpp").write_text(code)
    subprocess.run(["g++", "-std=c++17", "-I" + str(component), str(folder / "test.cpp"), "-o", str(folder / "test")], check=True)
    subprocess.run([str(folder / "test")], check=True)
print("PASS RGB888 presentation: colors, endian/offsets, partial preservation, handoff, PPA conversion, fused rotation, direct scaling/letterboxing and CPU fallback")
