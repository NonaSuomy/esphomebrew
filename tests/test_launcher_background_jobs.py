"""Private image publication and complete Settings directory snapshots."""
from pathlib import Path
import re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'apps/launcher_ui/main.c').read_text()

def function(name):
 m=re.search(r'static [^;\n]+\b'+name+r'\([^;]*?\)\s*\{',SOURCE);a=m.start();i=SOURCE.index('{',a)+1;depth=1
 while depth:
  if SOURCE[i]=='{':depth+=1
  elif SOURCE[i]=='}':depth-=1
  i+=1
 return SOURCE[a:i]

def run(code):
 with tempfile.TemporaryDirectory() as td:
  path=Path(td);(path/'test.c').write_text(code)
  sdk=ROOT.parent/'pax-esphome/components/pax_loader'
  if not sdk.exists():sdk=ROOT/'esphome/components/pax_loader'
  subprocess.run(['cc','-std=gnu11','-I'+str(sdk),'-I'+str(ROOT/'apps/launcher_ui'),str(path/'test.c'),'-o',str(path/'test')],check=True)
  subprocess.run([str(path/'test')],check=True)

PREFIX=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "timing.h"
#define PATH_BYTES 256
#define LIST_BYTES 8192
static app_services_t services,*g_svc=&services;
static bool g_discard_io,g_io_cancel,cancel_result;
static uint64_t timing_now(void){static uint64_t now;return ++now;}
static int io_run(int (*fn)(void *),void *arg){int result=fn(arg);if(cancel_result){g_discard_io=true;return -2;}return result;}
static void copy_text(char *out,size_t n,const char *in){strncpy(out,in,n-1);out[n-1]=0;}
static void work_status(const char *s){(void)s;}
'''

class BackgroundJobs(unittest.TestCase):
 def test_wallpaper_keeps_live_pixels_on_cancel_and_releases_after_ui_swap(self):
  code=PREFIX+r'''
#define LV_IMAGE_HEADER_MAGIC 1
#define LV_COLOR_FORMAT_RGB565 2
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_ALIGN_BOTTOM_RIGHT 1
typedef struct {struct {int magic,cf,w,h,stride;} header;uint32_t data_size;const uint8_t *data;} lv_image_dsc_t;
typedef int lv_obj_t;
typedef struct {uint16_t *pixels;uint16_t width,height;} ImageResult;
typedef struct {char path[PATH_BYTES];ImageResult image;} BackgroundJob;
static PaxTiming g_image_time;
static int display,widget;static void *g_display=&display;static lv_obj_t *g_background_widget=&widget;
static uint16_t *g_background_pixels,*previous,*decoded;static uint16_t g_background_width,g_background_height;
static lv_image_dsc_t g_background_descriptor;
static int g_canvas_w=1024,g_canvas_h=600,g_background_count=1,g_background_index;
static char g_background_paths[1][PATH_BYTES]={{"/sd/backgrounds/test.png"}};
static int freed,cache_drops;static bool on_ui;
static void release(void *ptr){if(ptr==previous){assert(on_ui && cache_drops==2 && g_background_pixels!=previous);}freed++;free(ptr);}
static uint16_t *decode(const char *path,uint16_t *w,uint16_t *h){assert(!strcmp(path,"/sd/backgrounds/test.png"));*w=*h=2;decoded=calloc(4,2);return decoded;}
static int call_ui(int (*fn)(void *),void *arg,uint32_t timeout){(void)timeout;on_ui=true;int result=fn(arg);on_ui=false;return result;}
static void lv_image_cache_drop(const void *src){assert(on_ui && src==&g_background_descriptor);cache_drops++;}
static void lv_image_header_cache_drop(const void *src){assert(on_ui && src==&g_background_descriptor);cache_drops++;}
static void lv_image_set_src(int *o,const void *src){assert(on_ui && o==&widget && ((const lv_image_dsc_t *)src)->data==(const uint8_t *)decoded);}
static void lv_image_set_scale_x(int *o,uint32_t x){(void)o;assert(x);}
static void lv_image_set_scale_y(int *o,uint32_t y){(void)o;assert(y);}
static void lv_obj_center(int *o){(void)o;}
static void lv_obj_clear_flag(int *o,int f){(void)o;(void)f;}
static void lv_obj_add_flag(int *o,int f){(void)o;(void)f;}
static int *lv_display_get_screen_active(void *d){assert(d==&display);return &widget;}
static void lv_obj_invalidate(int *o){assert(on_ui && o==&widget);}
'''+ '\n'.join(function(n) for n in ('image_release','image_descriptor','background_job','publish_background_on_host','load_background_image'))+r'''
int main(void){
 services.mem_free=release;services.png_load_rgb565=decode;services.ui_call_sync=call_ui;
 previous=g_background_pixels=calloc(4,2);g_background_width=g_background_height=2;
 cancel_result=true;load_background_image();assert(g_background_pixels==previous && freed==1 && cache_drops==0);
 cancel_result=g_discard_io=false;load_background_image();assert(g_background_pixels==decoded && freed==2 && cache_drops==2);
 uint16_t *live=g_background_pixels;load_background_image();assert(live==g_background_pixels && freed==2); /* cached */
 previous=live;cache_drops=0;g_background_count=0;load_background_image();assert(!g_background_pixels && freed==3 && cache_drops==2);
}
'''
  run(code)

 def test_storage_entry_count_is_not_mistaken_for_buffer_size_and_cancel_is_private(self):
  a=SOURCE.index('typedef struct {\n  pax_storage_status_t status[PAX_STORAGE_COUNT];');b=SOURCE.index('static int storage_scan_job(',a)
  code=PREFIX+SOURCE[a:b]+r'''
static bool g_settings_scan_busy,g_settings_scan_pending;
static pax_storage_status_t g_storage_status[PAX_STORAGE_COUNT];
static int g_storage_app_count[PAX_STORAGE_COUNT];static bool g_storage_status_inferred[PAX_STORAGE_COUNT],g_storage_path_accessible[PAX_STORAGE_COUNT];
static pax_network_config_t g_network_live,g_network_config;
static PaxTiming g_scan_time;
static bool ends_with(const char *s,const char *suffix){size_t n=strlen(s),m=strlen(suffix);return n>=m && !strcmp(s+n-m,suffix);}
static void refresh_wifi_settings(bool load){(void)load;}
static void refresh_network_live(bool load){(void)load;}
static int update_selection_on_host(void *arg){(void)arg;return 0;}
static int call_ui(int (*fn)(void *),void *arg,uint32_t timeout){(void)timeout;return fn(arg);}
static int status(int which,pax_storage_status_t *out){out->available=out->mounted=which==0;return 0;}
static int listing(const char *path,char *out,int capacity){assert(capacity==8192);if(!strcmp(path,"/sd/roms/pax/")){static const char names[]="a-long-application-name.pax\0second.pax\0folder/\0";memcpy(out,names,sizeof(names));return 3;}return -1;}
'''+function('storage_scan_job')+function('refresh_settings_snapshot')+r'''
int main(void){
 services.storage_status_get=status;services.file_list_dir=listing;services.ui_call_sync=call_ui;
 StorageSnapshot result={0};assert(storage_scan_job(&result)==0 && result.count[0]==2 && result.accessible[0]);
 g_storage_app_count[0]=99;cancel_result=true;refresh_settings_snapshot(false);assert(g_storage_app_count[0]==99 && !g_settings_scan_busy && g_settings_scan_pending);
 cancel_result=g_discard_io=false;g_settings_scan_pending=false;refresh_settings_snapshot(false);assert(g_storage_app_count[0]==2 && g_storage_status[0].mounted && !g_settings_scan_busy);
}
'''
  run(code)
 def test_cover_pixels_are_converted_and_scaled_on_worker_then_published_or_discarded(self):
  code=PREFIX+r'''
#define LV_IMAGE_HEADER_MAGIC 1
#define LV_COLOR_FORMAT_RGB565 2
#define COL_BG 0x123456
#define DETAIL_COVER_TEMP_SD "/sd/cover.png"
#define DETAIL_COVER_TEMP_USB "/usb0/cover.png"
typedef struct {struct {int magic,cf,w,h,stride;} header;uint32_t data_size;const uint8_t *data;} lv_image_dsc_t;
typedef struct {uint16_t *pixels;uint16_t width,height;} ImageResult;
typedef struct {const char *json,*path;int length;ImageResult image;} CoverJob;
static uint16_t *g_detail_cover_pixels;static uint16_t g_detail_cover_width,g_detail_cover_height;
static lv_image_dsc_t g_detail_cover_descriptor;static PaxTiming g_image_time;
static int allocations,frees,removed;
static int ui_px(int value){return value;}
static bool has_prefix(const char *s,const char *prefix){return !strncmp(s,prefix,strlen(prefix));}
static bool decode_base64_png_to_file(const char *json,int n,const char *path){(void)json;(void)n;assert(!strcmp(path,DETAIL_COVER_TEMP_USB));return true;}
static uint16_t *decode(const char *path,uint16_t *w,uint16_t *h,uint32_t background){assert(!strcmp(path,DETAIL_COVER_TEMP_USB) && background==0x563412);*w=128;*h=64;uint16_t *p=malloc(128*64*2);for(int i=0;i<128*64;i++)p[i]=0x0703;allocations++;return p;}
static void release(void *p){frees++;free(p);}
static int remove_file(const char *p){assert(!strcmp(p,DETAIL_COVER_TEMP_USB));removed++;return 0;}
'''+'\n'.join(function(n) for n in ('image_release','image_descriptor','release_detail_cover','restore_lvgl_rgb565','cover_job','load_detail_cover'))+r'''
int main(void){
 services.png_load_rgb565_blended=decode;services.mem_free=release;services.file_remove=remove_file;
 assert(load_detail_cover("json",4,"/usb0/app.json"));assert(g_detail_cover_width==28 && g_detail_cover_height==14 && g_detail_cover_pixels[0]==0x001f);
 assert(g_detail_cover_descriptor.data_size==28*14*2 && removed==1 && allocations==1);
 cancel_result=true;assert(!load_detail_cover("json",4,"/usb0/app.json"));assert(!g_detail_cover_pixels && frees==2 && allocations==2);
 PaxTiming timing={0};timing_record(&timing,1000);timing_record(&timing,17000);timing_record(&timing,(uint64_t)UINT32_MAX+1);
 assert(timing.samples==3 && timing.maximum==UINT32_MAX && timing.over_16ms==2 && timing.total==(uint64_t)UINT32_MAX+18000);
}
'''
  run(code)
if __name__=='__main__':unittest.main()
