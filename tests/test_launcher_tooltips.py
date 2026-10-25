"""Saved preference behavior and real LVGL hover/hold safety when LVGL is built."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_launcher_background_jobs import function,PREFIX,run
ROOT=Path(__file__).resolve().parents[1]
LIBS=list((ROOT.parent/'pax-esphome/.esphome/build/pax-desktop/.pioenvs/pax-desktop').glob('*/liblvgl.a'))
class Tooltips(unittest.TestCase):
 def test_preference_defaults_on_roundtrips_off_and_reports_failed_save(self):
  run(PREFIX+r'''
#define TOOLTIPS_PREF_FILE "/sd/roms/pax/.launcher_ui_tooltips"
static bool g_tooltips_enabled;static char stored[12];static int mode,closed;
static bool read_small_file(const char *p,char *out,size_t n){assert(!strcmp(p,TOOLTIPS_PREF_FILE));if(!stored[0])return false;strncpy(out,stored,n);return true;}
static void *open_file(const char *p,const char *m){assert(!strcmp(p,TOOLTIPS_PREF_FILE)&&!strcmp(m,"w"));return mode==1 ? NULL : stored;}
static size_t write_file(const void *p,size_t a,size_t b,void *f){assert(f==stored&&a==1&&b==2);memcpy(stored,p,2);stored[2]=0;return mode==2 ? 1 : 2;}
static int close_file(void *f){assert(f==stored);closed++;return mode==3 ? -1 : 0;}
'''+function('load_tooltips_preference')+function('save_tooltips_preference')+r'''
int main(void){
 services.file_open=open_file;services.file_write=write_file;services.file_close=close_file;
 load_tooltips_preference();assert(g_tooltips_enabled);
 g_tooltips_enabled=false;assert(save_tooltips_preference());g_tooltips_enabled=true;load_tooltips_preference();assert(!g_tooltips_enabled);
 g_tooltips_enabled=true;assert(save_tooltips_preference());g_tooltips_enabled=false;load_tooltips_preference();assert(g_tooltips_enabled);
 mode=1;assert(!save_tooltips_preference());mode=2;assert(!save_tooltips_preference());mode=3;assert(!save_tooltips_preference());assert(closed==4);
}
''')
 @unittest.skipUnless(len(LIBS)==1,'compile the desktop LVGL profile for hover/hold regression coverage')
 def test_real_lvgl_help_does_not_activate_held_buttons(self):
  build=ROOT.parent/'pax-esphome/.esphome/build/pax-desktop'
  with tempfile.TemporaryDirectory() as td:
   out=Path(td)/'tooltips'
   subprocess.run(['cc','-std=gnu11','-DLV_CONF_INCLUDE_SIMPLE','-I'+str(ROOT/'apps/launcher_ui'),'-I'+str(build/'src'),'-I'+str(build/'.piolibdeps/pax-desktop/lvgl'),str(ROOT/'tests/cpp/launcher_tooltips_test.c'),str(LIBS[0]),'-lm','-o',str(out)],check=True)
   subprocess.run([str(out)],check=True)
if __name__=='__main__':unittest.main()
