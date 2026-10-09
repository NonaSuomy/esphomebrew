from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class ThemeTests(unittest.TestCase):
    def test_roundtrip_validation_and_live_overlay_palette(self):
        sdk=ROOT.parent/'pax-esphome/components/pax_loader'
        if not sdk.exists():sdk=ROOT/'esphome/components/pax_loader'
        program=r'''
#include "pax_theme_profile.h"
#include "pax_overlay_theme.h"
#include "pax_volume_icon.h"
#include <cassert>
using namespace esphome::pax_loader;
int main(){char json[768];pax_theme_profile_t result={};
 for(int i=0;i<PAX_THEME_PRESETS;i++){
  assert(pax_theme_serialize(&pax_theme_presets[i],json,sizeof(json)));
  assert(pax_theme_parse(json,&result));assert(memcmp(&result,&pax_theme_presets[i],sizeof(result))==0);
  overlay_theme::apply(result);assert(overlay_theme::ACCENT==overlay_theme::rgb565(result.colors[PAX_THEME_ACCENT]));
  uint16_t pixels[58*58];volume_icon::render(pixels,60);
  assert(pixels[(57-29)*58+57-35]==overlay_theme::ACCENT);
  assert(pixels[(57-29)*58+57-8]==overlay_theme::TEXT);
 }
 assert(strcmp(pax_theme_presets[4].name,"Cyberpunk")==0);
 assert(pax_theme_presets[4].colors[PAX_THEME_ACCENT]==0xFF2BD6);
 pax_theme_profile_t before=result;
 assert(!pax_theme_parse("{\"name\":\"incomplete\"}",&result));assert(memcmp(&before,&result,sizeof(result))==0);
 assert(pax_theme_serialize(&pax_theme_presets[0],json,sizeof(json)));
 char *color=strstr(json,"#071525");assert(color);color[1]='G';assert(!pax_theme_parse(json,&result));
 assert(!pax_theme_serialize(&before,json,10));
 return 0;}
'''
        with tempfile.TemporaryDirectory() as td:
            p=Path(td);(p/'test.cpp').write_text(program)
            subprocess.run(['c++','-std=c++17','-I'+str(sdk),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
