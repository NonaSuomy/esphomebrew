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
constexpr int ESP_OK=0, portMAX_DELAY=0;
using esp_err_t=int;
const char *esp_err_to_name(int) { return "error"; }
int xSemaphoreTake(int,int) { return 1; }
namespace display { constexpr int COLOR_BITNESS_565=1; }
int result=0, presentations=0;
const uint8_t *presented=nullptr;
esp_err_t esp_lcd_panel_draw_bitmap(int,int x,int y,int w,int h,const void *p) {
  assert(x==0 && y==0 && w==3 && h==2);
  presented=static_cast<const uint8_t*>(p); ++presentations; return result;
}
class MIPI_DSI {
 public:
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
}
'''
    (folder / "test.cpp").write_text(code)
    subprocess.run(["g++", "-std=c++17", "-I" + str(component), str(folder / "test.cpp"), "-o", str(folder / "test")], check=True)
    subprocess.run([str(folder / "test")], check=True)
print("PASS RGB888 presentation: colors, endian/offsets, partial preservation, handoff and driver failure")
