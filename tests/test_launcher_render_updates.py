"""Verify real launcher update helpers skip unchanged work and publish atomically."""
from pathlib import Path
import re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]

def function(source,name):
    start=re.search(r'static (?:void|bool) '+name+r'\([^;]*?\)\s*\{',source).start()
    opening=source.index('{',start);end=opening+1;level=1
    while level:
        if source[end]=='{':level+=1
        elif source[end]=='}':level-=1
        end+=1
    return source[start:end]

def run(code):
    with tempfile.TemporaryDirectory() as folder:
        src=Path(folder)/'test.c';src.write_text(code);exe=Path(folder)/'test'
        subprocess.run(['cc','-std=gnu11','-Wall',str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

class RenderUpdateTests(unittest.TestCase):
    def test_unchanged_rows_labels_and_buttons_do_no_work(self):
        source=(ROOT/'apps/launcher_ui/main.c').read_text()
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define lv_snprintf snprintf
#define VISIBLE_ROWS 5
#define LIST_LABEL_BYTES 256
#define LV_PART_MAIN 0
#define LV_STATE_PRESSED 1
#define LV_STATE_FOCUSED 2
#define LV_OPA_60 60
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_ALIGN_RIGHT_MID 1
#define LV_ALIGN_LEFT_MID 2
#define LV_COORD_MAX 32767
#define LV_TEXT_FLAG_NONE 0
#define COL_SELECTED 1
#define COL_PANEL 2
#define COL_ACCENT 3
#define COL_MUTED 4
typedef int lv_font_t;
static const lv_font_t lv_font_montserrat_20=20,lv_font_montserrat_12=12;
typedef struct {int x,y;} lv_point_t;
typedef struct {char text[256];int width,hidden;} lv_obj_t;
static int writes,geometry,styles;
static lv_obj_t button,name,info;
static lv_obj_t *g_buttons[VISIBLE_ROWS]={&button},*g_labels[VISIBLE_ROWS]={&name},*g_list_detail_labels[VISIBLE_ROWS]={&info};
static bool g_list_file_details=true,g_list_stats_valid[2]={true,true};
static int g_size_unit;static float g_ui_scale=2;
static struct {int size;} g_list_stats[2]={{10},{20}};
static void copy_text(char *out,size_t n,const char *in){snprintf(out,n,"%s",in);}
static const char *lv_label_get_text(lv_obj_t *o){return o->text;}
static void lv_label_set_text(lv_obj_t *o,const char *s){writes++;copy_text(o->text,sizeof(o->text),s);}
static int lv_obj_get_content_width(lv_obj_t *o){return o->width;}
static int ui_px(int x){return x;}
static void pax_list_details_units(char *out,size_t n,const void *p,int unit){snprintf(out,n,"%d/%d",p?*(const int *)p:0,unit);}
static void lv_obj_set_style_text_font(lv_obj_t *o,const lv_font_t *f,int p){(void)o;(void)f;(void)p;geometry++;}
static void lv_text_get_size(lv_point_t *size,const char *t,const lv_font_t *f,int a,int b,int c,int d){(void)f;(void)a;(void)b;(void)c;(void)d;size->x=strlen(t);}
static void lv_obj_set_width(lv_obj_t *o,int w){o->width=w;geometry++;}
static void lv_obj_clear_flag(lv_obj_t *o,int f){(void)f;o->hidden=0;geometry++;}
static void lv_obj_add_flag(lv_obj_t *o,int f){(void)f;o->hidden=1;geometry++;}
static void lv_obj_align(lv_obj_t *o,int a,int x,int y){(void)o;(void)a;(void)x;(void)y;geometry++;}
static int lv_color_hex(int c){return c;}
static void style(lv_obj_t *o,int value,int part){(void)o;(void)value;(void)part;styles++;}
#define lv_obj_set_style_radius style
#define lv_obj_set_style_bg_color style
#define lv_obj_set_style_border_color style
#define lv_obj_set_style_border_width style
#define lv_obj_set_style_bg_opa style
''' + source[source.index('static unsigned g_ui_builds,'):source.index('static void style_button(')] + function(source,'style_button') + source[source.index('typedef struct {\n  bool valid, details;'):source.index('static void append_wifi_summary(')] + r'''
int main(void){
 button.width=500;update_list_row_text(0,0,"App");assert(g_row_updates==1 && writes==2);
 int before=writes,layout=geometry;update_list_row_text(0,0,"App");assert(writes==before && geometry==layout);
 update_list_row_text(0,1,"App");assert(g_row_updates==2 && writes==before+1); /* metadata changed */
 before=writes;button.width=300;update_list_row_text(0,1,"App");assert(g_row_updates==3 && writes==before);
 g_list_file_details=false;update_list_row_text(0,1,"App");assert(info.hidden);
 update_list_row_text(0,1,"Renamed");assert(!strcmp(name.text,"Renamed"));
 style_button(&button,false);before=styles;style_button(&button,false);assert(styles==before);
 style_button(&button,true);assert(styles>before);before=styles;style_button(&button,true);assert(styles==before);
 g_button_appearance_count=0;style_button(&button,true);assert(styles>before); /* new display, reused address */
}
'''
        run(code)

    def test_metadata_batches_preserve_selected_record_and_allow_immediate_input(self):
        source=(ROOT/'apps/launcher_ui/main.c').read_text()
        code=r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#define VISIBLE_ROWS 5
static bool g_metadata_pending,g_catalog_loading,g_settings_mode;
static int g_detail_index=-1,g_open_index=-1,g_selected=1,g_first_visible,g_view_count=2,g_visible_indices[2]={0,1};
static unsigned g_metadata_flushes;
static uint32_t g_metadata_last_flush;static int64_t now;static int redraws;
static int64_t get_time(void){return now*1000;}
static int update_selection_on_host(void *p){(void)p;return 0;}
static int ui_call(int (*fn)(void *),void *arg,int timeout){assert(fn==update_selection_on_host && !arg && timeout==1000);redraws++;return 0;}
static struct {int64_t (*get_time_us)(void);int (*ui_call_sync)(int (*)(void *),void *,int);} services={get_time,ui_call},*g_svc=&services;
static int selected_record_index(void){return g_visible_indices[g_selected];}
static void apply_filter(void){g_visible_indices[0]=1;g_visible_indices[1]=0;g_selected=1;}
''' + function(source,'flush_metadata_updates') + r'''
int main(void){
 g_metadata_pending=true;now=100;assert(!flush_metadata_updates(false));assert(g_visible_indices[1]==1 && !redraws);
 now=150;assert(flush_metadata_updates(false));assert(selected_record_index()==1 && redraws==1 && !g_metadata_pending);
 g_metadata_pending=true;now=151;assert(flush_metadata_updates(true) && redraws==2); /* user input is immediate */
 g_metadata_pending=true;g_open_index=-3;now=500;assert(!flush_metadata_updates(true) && redraws==2);
 g_open_index=-1;g_settings_mode=true;assert(!flush_metadata_updates(true));g_settings_mode=false;
 g_catalog_loading=true;assert(!flush_metadata_updates(true));g_catalog_loading=false;
 g_detail_index=0;assert(!flush_metadata_updates(true));g_detail_index=-1;
 g_metadata_last_flush=0xfffffff0U;now=0x90;assert(flush_metadata_updates(false)); /* clock wrap */
}
'''
        run(code)
