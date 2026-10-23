"""Exercise the launcher's real list/toolbar controller routing without LVGL."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

def function(source,name):
    start=source.index('static int '+name+'(') if name=='toolbar_focus_step' else source.index('static void '+name+'(')
    opening=source.index('{',start);end=opening+1;level=1
    while level:
        if source[end]=='{':level+=1
        elif source[end]=='}':level-=1
        end+=1
    return source[start:end]

class EmptyNavigationTests(unittest.TestCase):
    def test_empty_storage_network_favorites_and_toolbar_return(self):
        source=(ROOT/'apps/launcher_ui/main.c').read_text()
        poll=source.index('static bool poll_controller(void)')
        start=source.index('  } else if (g_toolbar_focus >= 0) {',poll)
        end=source.index('  if (g_detail_index < 0',start)
        routing=source[start:end].replace('  } else if','  if',1)
        defines='\n'.join(re.findall(r'^#define (?:SOURCE_\w+|VISIBLE_ROWS|ARCHIVE_LEVEL_SEARCH)\s+[^\n]+',source,re.M))
        program=r'''
#include <stdbool.h>
#include <assert.h>
#define PAX_APP_SIDE 1
#include "psram_app.h"
'''+defines+r'''
static int g_toolbar_focus=-1,g_source,g_selected,g_first_visible,g_view_count,g_open_index;
static int g_archive_level;
static uint32_t g_navigation_generation;
static bool g_catalog_stale;
static int g_detail_index=-1,g_io_state;
typedef struct {int source;} lv_event_t;
#define LV_EVENT_CLICKED 1
static int lv_event_get_code(lv_event_t *e){(void)e;return LV_EVENT_CLICKED;}
static void *lv_event_get_user_data(lv_event_t *e){return (void *)(intptr_t)e->source;}
static struct {int (*log_printf)(const char *,...);} *g_svc;

static bool g_favorites_mode,g_settings_mode,g_archive_search_pending;
static char g_archive_identifier[128],g_archive_zip_url[512],g_search_text[128];
static bool g_pad_last[PAX_INPUT_MAX];
static void mark_ui_action(void){}
static void play_ui_sound(int sound,int level){(void)sound;(void)level;}
#define PAX_UI_SOUND_CHIME 0
'''+function(source,'toolbar_focus_step')+'\n'+function(source,'select_source')+'\n'+function(source,'source_button_event')+r'''
static void press(int key){
 bool up=key==PAX_INPUT_UP,down=key==PAX_INPUT_DOWN,left=key==PAX_INPUT_LEFT,right=key==PAX_INPUT_RIGHT;
 bool a=key==PAX_INPUT_A,b=key==PAX_INPUT_B,l=key==PAX_INPUT_L,r=key==PAX_INPUT_R,changed=false;
 bool up_pressed=up&&!g_pad_last[PAX_INPUT_UP],down_pressed=down&&!g_pad_last[PAX_INPUT_DOWN];
 bool left_pressed=left&&!g_pad_last[PAX_INPUT_LEFT],right_pressed=right&&!g_pad_last[PAX_INPUT_RIGHT];
 bool page_up_pressed=l&&!g_pad_last[PAX_INPUT_L],page_down_pressed=r&&!g_pad_last[PAX_INPUT_R];
'''+routing+r'''
 (void)changed;
}
int main(void){
 g_source=SOURCE_STORAGE;g_view_count=0;
 press(PAX_INPUT_UP);assert(g_toolbar_focus==SOURCE_STORAGE);
 press(PAX_INPUT_RIGHT);assert(g_toolbar_focus==SOURCE_NETWORK);
 press(PAX_INPUT_A);assert(g_source==SOURCE_NETWORK && g_open_index==-3 && g_toolbar_focus==-1);
 press(PAX_INPUT_UP);assert(g_toolbar_focus==SOURCE_NETWORK);
 press(PAX_INPUT_DOWN);assert(g_toolbar_focus==7);
 press(PAX_INPUT_DOWN);assert(g_toolbar_focus==-1);
 press(PAX_INPUT_UP);assert(g_toolbar_focus==SOURCE_NETWORK);
 press(PAX_INPUT_B);assert(g_toolbar_focus==-1);
 g_favorites_mode=true;press(PAX_INPUT_UP);assert(g_toolbar_focus==4);
 g_favorites_mode=false;g_toolbar_focus=-1;g_pad_last[PAX_INPUT_UP]=true;
 press(PAX_INPUT_UP);assert(g_toolbar_focus==-1);g_pad_last[PAX_INPUT_UP]=false;
 g_view_count=2;g_selected=1;press(PAX_INPUT_UP);assert(g_selected==0 && g_toolbar_focus==-1);
 press(PAX_INPUT_UP);assert(g_toolbar_focus==SOURCE_NETWORK);
 /* The active source also exits Settings and cancels an in-flight read. */
 lv_event_t event={.source=g_source};g_settings_mode=true;g_open_index=-1;
 source_button_event(&event);assert(!g_settings_mode && g_open_index==-3 && g_catalog_stale);
 g_io_state=1;g_open_index=-1;uint32_t generation=g_navigation_generation;
 source_button_event(&event);assert(g_open_index==-3 && g_navigation_generation==generation+1);
 return 0;
}
'''
        sdk=ROOT.parent/'pax-esphome/components/pax_loader'
        if not sdk.exists():sdk=ROOT/'esphome/components/pax_loader'
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.c').write_text(program)
            subprocess.run(['cc','-std=gnu11','-I'+str(sdk),str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
