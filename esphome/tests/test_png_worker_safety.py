"""Compile the real PNG helper with a concurrent decoder stub."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]

class PNGWorkerSafety(unittest.TestCase):
 def test_parallel_decoders_serialize_and_failed_decode_releases_output(self):
  with tempfile.TemporaryDirectory() as td:
   folder=Path(td);(folder/'esp_heap_caps.h').write_text('''#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
inline void *heap_caps_aligned_calloc(int,size_t count,size_t size,int){return calloc(count,size);}
inline void heap_caps_free(void *p){free(p);}
''')
   (folder/'esp_log.h').write_text('#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n')
   (folder/'PNGdec.h').write_text('''#pragma once
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#define PNG_SUCCESS 0
#define PNG_RGB565_LITTLE_ENDIAN 1
#define PNG_RGB565_BIG_ENDIAN 2
struct PNGFILE{};struct PNGDRAW{};
inline std::atomic<int> active=0,maximum=0;
class PNG {bool fail=false;public:
 int open(const char *path,void *(*)(const char *,int32_t *),void (*)(void *),int32_t (*)(PNGFILE *,uint8_t *,int32_t),int32_t (*)(PNGFILE *,int32_t),void (*)(PNGDRAW *)){
  int now=++active;if(now>maximum)maximum=now;
  fail=strcmp(path,"bad")==0;std::this_thread::sleep_for(std::chrono::milliseconds(2));return 0;
 }
 int getWidth(){return 2;}int getHeight(){return 2;}int getBpp(){return 16;}int getPixelType(){return 2;}
 int decode(void *,int){std::this_thread::sleep_for(std::chrono::milliseconds(2));return fail ? -1 : 0;}
 void close(){--active;}
 void getLineAsRGB565(PNGDRAW *,uint16_t *,int,uint32_t){}
};
''')
   (folder/'test.cpp').write_text('''#include <cassert>
#include <cstdlib>
#include <thread>
#include "PNGdec.h"
#include "pngAux.h"
int main(){
 std::thread workers[6];
 for(int i=0;i<6;i++)workers[i]=std::thread([i](){for(int n=0;n<5;n++){
  pngObject out{};bool ok=i%2 ? loadPngFromFile("ok",&out,true,true) : loadPngFromFileRawBackground("ok",&out,true,true,0x123456);
  assert(ok && out.w==2 && out.h==2 && out.data);free(out.data);
 }});
 for(auto &worker:workers)worker.join();assert(maximum==1 && active==0);
 pngObject bad{};assert(!loadPngFromFileRaw("bad",&bad,true,true));assert(!bad.data && !bad.w && !bad.h && active==0);
 assert(!loadPngFromFile("bad",&bad,true,true));assert(!bad.data && !bad.w && !bad.h && active==0);
 assert(!loadPngFromFile(nullptr,&bad,true,true));
}
''')
   (folder/'pngAux.cpp').write_bytes((ROOT/'components/pax_loader/pngAux.cpp').read_bytes())
   exe=folder/'test'
   subprocess.run(['c++','-std=c++17','-pthread','-I',str(folder),'-I',str(ROOT/'components/pax_loader'),str(folder/'test.cpp'),str(folder/'pngAux.cpp'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
