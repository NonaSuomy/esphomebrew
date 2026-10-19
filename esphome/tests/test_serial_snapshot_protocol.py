"""Exercise the production serial snapshot handler with host task/heap stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class SerialSnapshotProtocol(unittest.TestCase):
    def test_sessions_bounds_failure_and_release(self):
        source = (ROOT / 'components/pax_web/pax_web.cpp').read_text()
        handler = source[source.index('std::vector<uint8_t> PaxWeb::snapshot_command_'):source.index('// Executed on the main loop via Improv')]
        fixture = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>
static int tasks=0;static bool reject=false;
static uint32_t millis(){return 100;}
static void heap_caps_free(void *p){free(p);}
constexpr int pdPASS=1, MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
static int xTaskCreatePinnedToCoreWithCaps(void (*)(void *),const char *,int,void *,int,void *,int,int){tasks++;return reject?0:pdPASS;}
struct PaxWeb {
 std::atomic<int> snapshot_state_{0};uint16_t *snapshot_pixels_=nullptr;
 uint16_t snapshot_width_=0,snapshot_height_=0;uint32_t snapshot_id_=0,snapshot_access_=0;
 uint8_t snapshot_sha_[32]{};
 static void snapshot_task_(void *){}
 std::vector<uint8_t> snapshot_command_(std::span<const uint8_t>);
};
''' + handler + r'''
int main(){
 PaxWeb web;
 auto call=[&](uint8_t op,uint32_t id,uint32_t offset=0){
  std::vector<uint8_t> p{op};for(unsigned i=0;i<4;i++)p.push_back(id>>(8*i));
  if(op==3)for(unsigned i=0;i<4;i++)p.push_back(offset>>(8*i));
  return web.snapshot_command_(p);
 };
 assert(web.snapshot_command_({})[1]==1);
 assert(call(1,42)[1]==0 && tasks==1 && web.snapshot_state_==1);
 assert(call(1,42)[1]==0 && tasks==1); // Retransmit must not restart the worker.
 assert(call(1,43)[1]==1 && tasks==1); // Another session cannot replace an active capture.
 assert(call(2,42)[1]==2 && call(4,42)[1]==2);
 web.snapshot_width_=128;web.snapshot_height_=1;
 web.snapshot_pixels_=static_cast<uint16_t *>(malloc(256));memset(web.snapshot_pixels_,7,256);
 web.snapshot_state_=2;
 assert(call(2,43)[1]==1);
 auto info=call(2,42);assert(info.size()==47 && info[7]==128 && info[9]==1);
 auto chunk=call(3,42);assert(chunk.size()==251 && chunk[11]==7);
 auto tail=call(3,42,240);assert(tail.size()==27 && tail[7]==240);
 assert(call(3,42,256)[1]==1 && call(3,42,0xffffffff)[1]==1);
 assert(call(4,43)[1]==1 && web.snapshot_pixels_!=nullptr);
 assert(call(4,42)[1]==0 && web.snapshot_pixels_==nullptr && web.snapshot_state_==0);
 reject=true;assert(call(1,44)[1]==1 && web.snapshot_state_==3);
 assert(call(4,44)[1]==0 && web.snapshot_state_==0);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'snapshot.cpp'; cpp.write_text(fixture)
            executable = Path(folder) / 'snapshot'
            subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

if __name__ == '__main__':
    unittest.main()
