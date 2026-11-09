"""Exercise same-core and cross-core DMA handoff interleavings."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class DisplayHandoff(unittest.TestCase):
 def test_safe_buffer_release(self):
  code=r'''
#include <cassert>
#include "frame_buffer_handoff.h"
using esphome::mipi_dsi::FrameBufferHandoff;
int main(){
 for(bool same_core:{false,true}){
  FrameBufferHandoff h;
  for(int i=0;i<100;i++){
   auto back=h.back_index();h.begin(back);
   assert(!h.frame_complete() && h.pending()); // ISR before submission
   h.submitted(same_core);
   if(!same_core){assert(!h.frame_complete() && h.pending());h.submitted(false);}
   assert(h.frame_complete() && !h.pending() && h.back_index()!=back);
   assert(!h.frame_complete());
  }
  h.begin(h.back_index());h.submitted(same_core);h.cancel();assert(!h.frame_complete() && !h.pending());
 }
}
'''
  code=code.replace('#include <cassert>','#include <cassert>\n#include <initializer_list>')
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'test.cpp').write_text(code)
   subprocess.run(['c++','-std=c++17','-I',str(ROOT/'dependencies/esphome/config/components/mipi_dsi'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
