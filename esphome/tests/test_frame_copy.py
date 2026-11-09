"""Clipped span copies agree with the old pixel loop for independent input."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class FrameCopy(unittest.TestCase):
 def test_clip_stride_canaries_extreme_coordinates_and_full_width_overlap(self):
  code=r'''
#include <cassert>
#include <climits>
#include <cstdint>
#include <vector>
#include "pax_frame_copy.h"
int main(){
 constexpr int cw=9,ch=7;constexpr uint16_t guard=0xdead;
 for(int w=1;w<=13;w++)for(int h=1;h<=11;h++)for(int x=-14;x<=14;x++)for(int y=-12;y<=12;y++){
  std::vector<uint16_t> source(w*h),result(cw*ch+2,guard),expected=result;
  for(int i=0;i<w*h;i++)source[i]=i+1;
  for(int row=0;row<h;row++)for(int col=0;col<w;col++)if(x+col>=0 && x+col<cw && y+row>=0 && y+row<ch)expected[1+(y+row)*cw+x+col]=source[row*w+col];
  esphome::pax_frame::copy_rect(result.data()+1,cw,ch,x,y,w,h,source.data());assert(result==expected);
 }
 uint16_t frame[16],source[16];for(int i=0;i<16;i++)frame[i]=source[i]=i;
 esphome::pax_frame::copy_rect(frame,4,4,0,1,4,3,frame);for(int i=4;i<16;i++)assert(frame[i]==source[i-4]);
 auto unchanged=std::vector<uint16_t>(frame,frame+16);
 esphome::pax_frame::copy_rect(frame,4,4,INT_MAX,INT_MIN,INT_MAX,INT_MAX,source);
 esphome::pax_frame::copy_rect(frame,4,4,INT_MIN,INT_MIN,INT_MAX,INT_MAX,source);
 esphome::pax_frame::copy_rect(frame,4,4,0,0,-1,4,source);
 esphome::pax_frame::copy_rect(frame,4,4,0,0,4,4,nullptr);
 assert(std::vector<uint16_t>(frame,frame+16)==unchanged);
}
'''
  with tempfile.TemporaryDirectory() as td:
   folder=Path(td);src=folder/'test.cpp';src.write_text(code);exe=folder/'test'
   subprocess.run(['c++','-std=c++17','-O2','-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(ROOT/'components/pax_loader'),str(src),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=10)
if __name__=='__main__':unittest.main()
