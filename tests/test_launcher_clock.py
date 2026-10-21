"""Check the actual shared clock handlers and compact date formatting."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
def function(source,name):
 start=source.index('static void '+name+'(');opening=source.index('{',start);end=opening+1;depth=1
 while depth:
  if source[end]=='{':depth+=1
  elif source[end]=='}':depth-=1
  end+=1
 return source[start:end]
class ClockTest(unittest.TestCase):
 def test_tap_toggles_text_without_popup(self):
  source=(ROOT/'apps/launcher_ui/main.c').read_text()
  program=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LV_PART_MAIN 0
#define LV_EVENT_CLICKED 1
static int lv_font_montserrat_14,lv_font_montserrat_20;
typedef struct {int code;} lv_event_t;
static int lv_event_get_code(lv_event_t *event){return event->code;}
static char shown[64];static const int *font;
static void lv_obj_set_style_text_font(void *object,const int *value,int part){(void)object;(void)part;font=value;}
static void lv_label_set_text(void *object,const char *value){(void)object;strcpy(shown,value);}
static void *g_status_clock=(void*)1;
static bool g_show_date;
static struct {char local_time[16];} g_device_status={"10:00 AM"};
static const char *date="Wednesday\n21 October 2026";static int result;
static int get_date(char *out,uint32_t capacity){assert(capacity>strlen(date));strcpy(out,date);return result;}
static struct {int (*device_date_get)(char*,uint32_t);} services={get_date},*g_svc=&services;
'''+function(source,'append_text')+'\n'+function(source,'update_clock_label')+'\n'+function(source,'clock_tap_event')+r'''
int main(void){
 lv_event_t tap={LV_EVENT_CLICKED},other={0};
 update_clock_label();assert(!strcmp(shown,"10:00 AM") && font==&lv_font_montserrat_20);
 clock_tap_event(&tap);assert(g_show_date && !strcmp(shown,"21 Oct 2026") && font==&lv_font_montserrat_14);
 date="Thursday\n01 January 2027";update_clock_label();assert(!strcmp(shown,"01 Jan 2027"));
 clock_tap_event(&other);assert(g_show_date);
 clock_tap_event(&tap);assert(!g_show_date && !strcmp(shown,"10:00 AM"));
 strcpy(g_device_status.local_time,"23:59");update_clock_label();assert(!strcmp(shown,"23:59"));
 clock_tap_event(&tap);result=-2;update_clock_label();assert(!strcmp(shown,"Syncing..."));
 services.device_date_get=NULL;update_clock_label();assert(!strcmp(shown,"No date"));
 clock_tap_event(&tap);assert(!strcmp(shown,"23:59"));
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as temp:
   p=Path(temp);(p/'test.c').write_text(program)
   subprocess.run(['cc','-std=c11',str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
