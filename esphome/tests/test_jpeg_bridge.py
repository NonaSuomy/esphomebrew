"""Check the actual JPEG bridge's padded buffers with a fake DMA driver."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class JpegBridgeTest(unittest.TestCase):
    def test_padding_capacity_reuse_failure_and_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path / 'driver').mkdir()
            (path / 'esp_heap_caps.h').write_text('#pragma once\n#include <cstdlib>\ninline void heap_caps_free(void *p){free(p);}\n')
            (path / 'driver/jpeg_decode.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
using esp_err_t=int;
constexpr int ESP_OK=0;
constexpr int JPEG_DOWN_SAMPLING_GRAY=0,JPEG_DOWN_SAMPLING_YUV444=1,JPEG_DOWN_SAMPLING_YUV422=2,JPEG_DOWN_SAMPLING_YUV420=3;
constexpr int JPEG_DECODE_OUT_FORMAT_RGB565=1,JPEG_DEC_RGB_ELEMENT_ORDER_BGR=1,JPEG_YUV_RGB_CONV_STD_BT601=0;
using jpeg_dec_buffer_alloc_direction_t=int;
constexpr int JPEG_DEC_ALLOC_INPUT_BUFFER=0,JPEG_DEC_ALLOC_OUTPUT_BUFFER=1;
using jpeg_decoder_handle_t=void*;
struct jpeg_decode_picture_info_t {uint32_t width,height;int sample_method;};
struct jpeg_decode_engine_cfg_t {int timeout_ms;};
struct jpeg_decode_memory_alloc_cfg_t {int buffer_direction;};
struct jpeg_decode_cfg_t {int output_format,rgb_order,conv_std;};
static int calls=0,created=0,deleted=0;static bool fail=false,engine_fail=false;
inline int jpeg_decoder_get_info(const uint8_t *p,uint32_t,jpeg_decode_picture_info_t *i){if(p[0]==255)return -1;i->width=p[1];i->height=p[2];i->sample_method=p[0];return 0;}
inline int jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t*,void **p){if(engine_fail)return -1;*p=(void*)1;++created;return 0;}
inline int jpeg_del_decoder_engine(void*){++deleted;return 0;}
inline void *jpeg_alloc_decoder_mem(size_t n,const jpeg_decode_memory_alloc_cfg_t*,size_t *actual){*actual=n;return fail?nullptr:malloc(n);}
inline int jpeg_decoder_process(void*,const jpeg_decode_cfg_t*,const uint8_t *p,uint32_t,uint8_t *out,uint32_t capacity,uint32_t *written){
 int bx=p[0]==1?8:16,by=p[0]==3?16:8;
 int w=(p[1]+bx-1)&~(bx-1),h=(p[2]+by-1)&~(by-1);
 *written=w*h*2;if(*written>capacity)return -1;
 for(int y=0;y<h;++y)for(int x=0;x<w;++x){uint16_t v=y*100+x;memcpy(out+(y*w+x)*2,&v,2);}++calls;return 0;
}
''')
            (path / 'test.cpp').write_text(r'''
#include <cassert>
#include "pax_jpeg.h"
int main(){
 PaxJpegAccelerator jpeg;
 uint16_t out[40*24+1];
 for(int sample:{1,2,3}){
  uint8_t packet[4]={(uint8_t)sample,33,17,0};
  for(auto &v:out)v=0xbeef;
  assert(jpeg.decode(packet,4,out,40*17*2,33,17,80)==0);
  for(int y=0;y<17;++y){for(int x=0;x<33;++x)assert(out[y*40+x]==y*100+x);for(int x=33;x<40;++x)assert(out[y*40+x]==0xbeef);}
  assert(out[40*17]==0xbeef);
  assert(jpeg.decode(packet,4,out,40*17*2-1,33,17,80)<0);
  assert(jpeg.decode(packet,4,out,sizeof(out),34,17,80)<0);
 }
 assert(created==1&&calls==3);
 jpeg.reset();assert(deleted==1);
 uint8_t bad[4]={255,33,17,0};assert(jpeg.decode(bad,4,out,sizeof(out),33,17,80)<0);
 fail=true;bad[0]=2;assert(jpeg.decode(bad,4,out,sizeof(out),33,17,80)<0);jpeg.reset();
 fail=false;engine_fail=true;assert(jpeg.decode(bad,4,out,sizeof(out),33,17,80)<0);jpeg.reset();
 assert(jpeg.decode(nullptr,4,out,sizeof(out),33,17,80)<0);
 assert(jpeg.decode(bad,4,out,sizeof(out),4097,17,80)<0);
}
'''.replace('#include <cassert>','#include <cassert>\n#include <initializer_list>'))
            subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(path),'-I'+str(ROOT/'components/pax_loader'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
if __name__=='__main__':unittest.main()
