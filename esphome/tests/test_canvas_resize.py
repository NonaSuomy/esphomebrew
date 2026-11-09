"""Exercise production framebuffer allocation/canvas transitions with guarded heaps."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'components/pax_loader/pax_loader.cpp').read_text()
def method(signature):
 start = source.index(signature)
 opening = source.index('{', start)
 level = 1
 end = opening + 1
 while level:
  level += (source[end] == '{') - (source[end] == '}')
  end += 1
 return source[start:end]
methods = '\n'.join(method(signature) for signature in (
 'void PaxLoader::free_frame_buffers_()',
 'bool PaxLoader::resize_frame_buffers_(int width, int height)',
 'bool PaxLoader::allocate_frame_buffers_(int width, int height)',
 'bool PaxLoader::apply_canvas_(int width, int height, bool blank_panel)',
 'int PaxLoader::set_canvas_(int width, int height)',
 'bool PaxLoader::restore_legacy_canvas_()',
 'bool PaxLoader::prepare_canvas_for_app_(const std::string &source)',
))
code = r'''#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include "pax_canvas.h"
namespace canvas=esphome::pax_loader::canvas;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_DMA 2
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 2
#define pdMS_TO_TICKS(n) (n)
constexpr int pdTRUE=1;
std::map<void*,size_t> live;int allocations=0,fail_at=0,lock_depth=0;bool grant=true;
void *heap_caps_aligned_calloc(size_t alignment,size_t count,size_t bytes,int){
 if(++allocations==fail_at)return nullptr;void*p=nullptr;
 assert(posix_memalign(&p,alignment,count*bytes)==0);memset(p,0,count*bytes);live[p]=count*bytes;return p;
}
void heap_caps_free(void*p){if(p){assert(live.erase(p)==1);free(p);}}
void esp_cache_msync(void*p,size_t bytes,int){assert(live.at(p)>=bytes);}
int xSemaphoreTakeRecursive(void*,int){if(!grant)return 0;++lock_depth;return pdTRUE;}
void xSemaphoreGiveRecursive(void*){assert(lock_depth>0);--lock_depth;}
namespace display { constexpr int COLOR_ORDER_RGB=0,COLOR_BITNESS_565=0; }
struct Display {
 int rows=0,calls=0;
 void draw_pixels_at(int,int,int w,int h,const uint8_t*p,int,int,bool){assert(live.at((void*)p)>=size_t(w)*h*2);rows+=h;++calls;}
};
struct PaxLoader {
 uint16_t *framebuffer_=nullptr,*rotated_framebuffer_=nullptr,*ppa_framebuffer_=nullptr;
 uint16_t *initialized_panel_framebuffers_[2]{};
 const uint8_t*native_rgb888_frame_=nullptr;
 bool last_frame_direct_=false,canvas_chosen_=false;
 uint16_t direct_frame_[4]{};
 size_t frame_alloc_bytes_=0;int max_canvas_w_=800,max_canvas_h_=480,default_canvas_w_=1024,default_canvas_h_=600;
 int offered_canvas_w_=0,offered_canvas_h_=0;
 canvas::Geometry geometry_{};void*display_mutex_=(void*)1;Display*display_=nullptr;
 void free_frame_buffers_();bool resize_frame_buffers_(int,int);bool allocate_frame_buffers_(int,int);
 bool apply_canvas_(int,int,bool);int set_canvas_(int,int);bool restore_legacy_canvas_();
 bool prepare_canvas_for_app_(const std::string&);
 std::string get_app_canvas(const std::string&){return {};}
 std::string listing_recommended_canvas_(const std::string&){return "640x400";}
};
''' + methods + r'''
int main(){
 PaxLoader p;Display display;p.display_=&display;p.geometry_.panel_w=1024;p.geometry_.panel_h=600;
 assert(p.allocate_frame_buffers_(1024,600));assert(live.size()==3);
 assert(p.apply_canvas_(800,480,false));
 assert(p.set_canvas_(640,400)==0);assert(lock_depth==0);
 assert(p.frame_alloc_bytes_==512000&&live.size()==3);
 assert(p.max_canvas_w_==1024&&p.max_canvas_h_==600);
 assert(display.rows==600&&display.calls==3); // bounded full-panel clear from smaller work buffer
 for(int failed=1;failed<=3;++failed){
  auto*old=p.framebuffer_;auto*r=p.rotated_framebuffer_;auto*q=p.ppa_framebuffer_;
  old[0]=0x1234;fail_at=allocations+failed;
  assert(p.set_canvas_(1024,600)==-1);
  assert(p.framebuffer_==old&&p.rotated_framebuffer_==r&&p.ppa_framebuffer_==q);
  assert(old[0]==0x1234&&p.geometry_.canvas_w==640&&p.frame_alloc_bytes_==512000);
  assert(live.size()==3&&lock_depth==0);fail_at=0;
 }
 fail_at=allocations+1;assert(!p.prepare_canvas_for_app_("redalert.pax"));
 assert(p.geometry_.canvas_w==640&&live.size()==3);fail_at=0;
 grant=false;assert(!p.restore_legacy_canvas_());assert(p.set_canvas_(800,480)==-1);grant=true;
 assert(p.restore_legacy_canvas_());assert(p.frame_alloc_bytes_==768000);
 assert(p.set_canvas_(1024,600)==0);assert(p.frame_alloc_bytes_==1228800);
 auto*old=p.framebuffer_;fail_at=allocations+2;
 assert(p.set_canvas_(640,400)==0); // failed shrink retains larger safe buffers
 assert(p.framebuffer_==old&&p.frame_alloc_bytes_==1228800&&live.size()==3);fail_at=0;
 assert(p.prepare_canvas_for_app_("redalert.pax"));assert(p.offered_canvas_w_==640);
 assert(p.set_canvas_(640,400)==0);assert(p.frame_alloc_bytes_==512000);
 assert(p.set_canvas_(1026,600)==-1);assert(p.set_canvas_(639,400)==-1);
 p.free_frame_buffers_();assert(live.empty());
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                 '-I'+str(root/'components/pax_loader'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS canvas resize: shrink/grow, failed allocations, rollback, restored limits, locking and bounded panel clearing')
