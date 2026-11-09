"""Controller profile validation, IO transaction and persistence regression."""
from pathlib import Path
import subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
class ControllerRemap(unittest.TestCase):
 def test_profile_and_virtual_file_transactions(self):
  with tempfile.TemporaryDirectory() as td:
   t=Path(td);p=t/'esphome/core/preferences.h';p.parent.mkdir(parents=True)
   p.write_text('''#pragma once
#include <cstring>
namespace esphome {
inline bool persist=true,has_saved=false;inline unsigned char stored[64];
struct ESPPreferenceObject {template<class T> bool load(T *p){if(!has_saved)return false;memcpy(p,stored,sizeof(T));return true;} template<class T> bool save(const T *p){if(!persist)return false;memcpy(stored,p,sizeof(T));has_saved=true;return true;}};
struct Prefs {template<class T> ESPPreferenceObject make_preference(unsigned,bool){return {};} bool sync(){return persist;}};
inline Prefs prefs;inline Prefs *global_preferences=&prefs;
}
''')
   code='''#include <cassert>
#include <cstdint>
#include <cstdio>
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#include "pax_controller_stream.h"
static uint32_t raw(){return (1U<<6)|(1U<<12);}
int main(){
 pax_controller_map_t m;pax_controller_defaults(&m);assert(pax_controller_valid(&m));
 assert(pax_controller_apply(&m,0xffff)==0xffff);
 m.source[6]=7;m.source[7]=6;assert(pax_controller_valid(&m));
 assert(pax_controller_apply(&m,1U<<6)==(1U<<7));
 assert(pax_controller_apply(&m,1U<<12)==(1U<<12));
 m.source[0]=255;assert(pax_controller_valid(&m));assert(!(pax_controller_apply(&m,1)&1));
 m.source[0]=50;assert(!pax_controller_valid(&m));m.source[0]=7;assert(!pax_controller_valid(&m));
 pax_controller::setup(raw);
 FILE *f=pax_controller::open("rb");pax_controller_status_t s{};assert(fread(&s,1,sizeof(s),f)==sizeof(s));assert(fread(&s,1,1,f)==0);fclose(f);assert(s.raw==raw());
 pax_controller_defaults(&m);m.source[6]=7;m.source[7]=6;
 f=pax_controller::open("wb");assert(fwrite(&m,1,4,f)==4);assert(fwrite((char*)&m+4,1,sizeof(m)-4,f)==sizeof(m)-4);assert(fclose(f)==0);
 assert(pax_controller::apply(1U<<6)==(1U<<6)); /* queued, not applied from app task */
 pax_controller::loop();assert(pax_controller::apply(1U<<6)==(1U<<7));
 pax_controller::setup(raw);assert(pax_controller::apply(1U<<6)==(1U<<7)); /* reload */
 esphome::persist=false;pax_controller_defaults(&m);f=pax_controller::open("wb");assert(fwrite(&m,1,sizeof(m),f)==sizeof(m));assert(fclose(f)==0);pax_controller::loop();
 assert(pax_controller::failed && !pax_controller::pending);assert(pax_controller::apply(1U<<6)==(1U<<7));
 f=pax_controller::open("wb");assert(fwrite(&m,1,4,f)==4);assert(fclose(f)!=0);assert(!pax_controller::pending);
 m.source[0]=60;f=pax_controller::open("wb");assert(fwrite(&m,1,sizeof(m),f)==sizeof(m));assert(fclose(f)!=0);
}
'''
   (t/'test.cpp').write_text(code)
   subprocess.run(['g++','-std=c++17','-I'+str(t),'-I'+str(ROOT/'components/pax_loader'),str(t/'test.cpp'),'-o',str(t/'test')],check=True)
   subprocess.run([str(t/'test')],check=True)
if __name__=='__main__':unittest.main()
