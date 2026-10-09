"""Check rendered volume indicators and buffer bounds using the firmware renderer."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class VolumeIconTests(unittest.TestCase):
    def test_levels_rotation_and_buffer_bounds(self):
        header=ROOT.parent/'pax-esphome/components/pax_loader'
        if not header.exists():header=ROOT/'esphome/components/pax_loader'
        source=r'''
#include "pax_volume_icon.h"
#include <cassert>
#include <cstdio>
using namespace esphome::pax_loader::volume_icon;
int main(int argc,char **argv){
 uint16_t guarded[SIZE*SIZE+2];guarded[0]=0x1234;guarded[SIZE*SIZE+1]=0x5678;
 for(int level=-1;level<=101;level++){
  render(guarded+1,level);assert(guarded[0]==0x1234 && guarded[SIZE*SIZE+1]==0x5678);
  auto visible=[&](int x,int y){return guarded[1+(SIZE-1-y)*SIZE+SIZE-1-x];};
  // Rectangular driver, widening cone, and straight mouth face the indicator bars.
  assert(visible(8,29)==BODY && visible(8,13)==BACKGROUND);
  assert(visible(17,13)==BACKGROUND && visible(29,13)==BODY && visible(30,45)==BODY);
  int expected=level<=0?0:level>=100?5:(level+19)/20;
  for(int bar=0;bar<5;bar++)assert(visible(35+bar*4,29)==(bar<expected?ACTIVE:INACTIVE));
 }
 if(argc>1){
  for(int level: {0,20,60,100}){
   render(guarded+1,level);char path[1024];snprintf(path,sizeof(path),"%s/%d.ppm",argv[1],level);
   FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",SIZE,SIZE);
   for(int y=0;y<SIZE;y++)for(int x=0;x<SIZE;x++){
    uint16_t c=guarded[1+(SIZE-1-y)*SIZE+SIZE-1-x];
    unsigned char rgb[3]={(unsigned char)(((c>>11)&31)*255/31),(unsigned char)(((c>>5)&63)*255/63),(unsigned char)((c&31)*255/31)};
    fwrite(rgb,1,3,f);
   }fclose(f);
  }
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++17','-I'+str(header),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

if __name__=='__main__':unittest.main()
