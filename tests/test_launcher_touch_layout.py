"""Real LVGL layout: large targets and separated stacked rows on small panels."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_launcher_catalog_pages import function
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT.parent/'pax-esphome/.esphome/build/pax-desktop'
LIBS=list((BUILD/'.pioenvs/pax-desktop').glob('*/liblvgl.a'))
@unittest.skipUnless(len(LIBS)==1,'compile desktop LVGL for layout checks')
class TouchLayout(unittest.TestCase):
 def test_narrow_rows_stack_without_reducing_fonts_or_targets(self):
  source=(ROOT/'apps/launcher_ui/main.c').read_text()
  code=r'''
#include "lvgl.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
void lv_mem_init(void){}
void lv_mem_deinit(void){}
void *lv_malloc_core(size_t n){return malloc(n);}
void lv_free_core(void *p){free(p);}
void *lv_realloc_core(void *p,size_t n){return realloc(p,n);}
lv_result_t lv_mem_test_core(void){return LV_RESULT_OK;}
void lv_mem_monitor_core(lv_mem_monitor_t *p){memset(p,0,sizeof(*p));}
#include "touch_layout.h"
#define COL_ACCENT 0x00dddd
static lv_obj_t *g_volume_slider,*g_brightness_slider,*g_settings_back_button;
static int ui_px(int n){return n*2;}
'''+function(source,'settings_touch_layout')+r'''
int main(void){
 lv_init();lv_display_t *display=lv_display_create(800,480);
 for(int width=700;width<=920;width+=220){
  lv_obj_t *panel=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(panel);lv_obj_set_size(panel,width,350);
  g_settings_back_button=lv_button_create(panel);lv_obj_remove_style_all(g_settings_back_button);
  lv_obj_set_pos(g_settings_back_button,width-100,0);lv_obj_set_size(g_settings_back_button,96,34);
  lv_obj_t *labels[3],*buttons[3];
  int control_x=width*46/100,control_w=width-control_x-16;
  for(int i=0;i<3;i++){
   labels[i]=lv_label_create(panel);lv_label_set_text(labels[i],"WI-FI NAME");lv_obj_set_pos(labels[i],16,48+i*52+4);lv_obj_set_width(labels[i],control_x-24);
   buttons[i]=lv_button_create(panel);lv_obj_remove_style_all(buttons[i]);lv_obj_add_flag(buttons[i],LV_OBJ_FLAG_CLICKABLE);
   lv_obj_set_pos(buttons[i],control_x,48+i*52+4);lv_obj_set_size(buttons[i],control_w,36);
  }
  settings_touch_layout(panel,width,control_x,control_w,48,52);
  assert(lv_obj_get_height(g_settings_back_button)>=48);
  for(int i=0;i<3;i++){
   assert(lv_obj_get_height(buttons[i])>=48 && lv_obj_get_width(buttons[i])>=48);
   if(width<760){assert(lv_obj_get_x(buttons[i])==16);assert(lv_obj_get_y(buttons[i])>=lv_obj_get_y(labels[i])+30);}
   else assert(lv_obj_get_x(buttons[i])==control_x);
   if(i<2)assert(lv_obj_get_y(buttons[i+1])>=lv_obj_get_y(buttons[i])+lv_obj_get_height(buttons[i])+12);
  }
  lv_obj_delete(panel);
 }
 assert(touch_keyboard_height(480,200)==224);assert(touch_keyboard_height(600,250)==250);
 lv_display_delete(display);
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'test.c').write_text(code)
   subprocess.run(['cc','-std=gnu11','-DLV_CONF_INCLUDE_SIMPLE','-I'+str(ROOT/'apps/launcher_ui'),'-I'+str(BUILD/'src'),'-I'+str(BUILD/'.piolibdeps/pax-desktop/lvgl'),str(p/'test.c'),str(LIBS[0]),'-lm','-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
