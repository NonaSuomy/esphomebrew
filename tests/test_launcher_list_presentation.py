from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

def function(source,name):
    start=re.search(r'static (?:bool|void|int) '+name+r'\(',source).start()
    opening=source.index('{',start);end=opening+1;level=1
    while level:
        if source[end]=='{':level+=1
        elif source[end]=='}':level-=1
        end+=1
    return source[start:end]

class ListPresentationTests(unittest.TestCase):
    def test_names_dates_size_saved_preferences_and_focus(self):
        source=(ROOT/'apps/launcher_ui/main.c').read_text()
        program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "list_presentation.h"
#define SETTINGS_FOCUS_COUNT 25
#define LIST_PREF_FILE "view-preference"
static bool g_hide_pax_extension,g_list_file_details;
static const app_services_t *g_svc;
static char stored[12];static int offset;static bool missing=true;
static void *open_file(const char *path,const char *mode){assert(strcmp(path,LIST_PREF_FILE)==0);offset=0;if(mode[0]=='r' && missing)return NULL;if(mode[0]=='w'){memset(stored,0,sizeof(stored));missing=false;}return stored;}
static size_t read_file(void *out,size_t size,size_t count,void *f){(void)f;size_t n=strlen(stored)-offset;if(n>size*count)n=size*count;memcpy(out,stored+offset,n);offset+=n;return n/size;}
static size_t write_file(const void *in,size_t size,size_t count,void *f){(void)f;assert(size*count<sizeof(stored));memcpy(stored,in,size*count);return count;}
static int close_file(void *f){(void)f;return 0;}
'''+ '\n'.join(function(source,name) for name in ['read_small_file','load_list_preferences','save_list_preferences','settings_focus_step'])+r'''
int main(void){
 char text[256];const char name[]="scummvm-sky.pax";
 pax_file_stat_t stat={.size=5368709120ULL,.mtime=951849000};
 pax_list_label(text,sizeof(text),name,true,false,&stat);assert(strcmp(text,"scummvm-sky")==0);assert(strcmp(name,"scummvm-sky.pax")==0);
 pax_list_label(text,sizeof(text),name,false,false,&stat);assert(strcmp(text,name)==0);
 pax_list_label(text,sizeof(text),"Demo.PAX",true,false,NULL);assert(strcmp(text,"Demo")==0);
 pax_list_label(text,sizeof(text),"pax-example.zip",true,false,NULL);assert(strcmp(text,"pax-example.zip")==0);
 pax_list_label(text,sizeof(text),name,true,true,&stat);assert(strcmp(text,"scummvm-sky\n5368709120 bytes | 2000-02-29 18:30 UTC")==0);
 text[0]=0;pax_info_date(text,sizeof(text),4107542400LL);assert(strcmp(text,"2100-03-01 00:00 UTC")==0);
 text[0]=0;pax_info_date(text,sizeof(text),0);assert(strcmp(text,"Unavailable")==0);
 text[0]=0;pax_info_date(text,sizeof(text),-1);assert(strcmp(text,"Unavailable")==0);
 pax_list_label(text,sizeof(text),name,true,true,NULL);assert(strstr(text,"File details unavailable"));
 char tiny[4];pax_list_label(tiny,sizeof(tiny),name,true,true,&stat);assert(tiny[3]==0);
 app_services_t svc={0};svc.file_open=open_file;svc.file_read=read_file;svc.file_write=write_file;svc.file_close=close_file;g_svc=&svc;
 load_list_preferences();assert(!g_hide_pax_extension && !g_list_file_details);
 g_hide_pax_extension=g_list_file_details=true;assert(save_list_preferences());g_hide_pax_extension=g_list_file_details=false;
 load_list_preferences();assert(g_hide_pax_extension && g_list_file_details);
 g_list_file_details=false;assert(save_list_preferences());load_list_preferences();assert(g_hide_pax_extension && !g_list_file_details);
 bool seen[25]={0};int focus=18;for(int i=0;i<25;i++){assert(focus>=0 && focus<25 && !seen[focus]);seen[focus]=true;int next=settings_focus_step(focus,1);assert(settings_focus_step(next,-1)==focus);focus=next;}assert(focus==18 && seen[22] && seen[23] && seen[24]);
 return 0;
}
'''
        sdk=ROOT.parent/'pax-esphome/components/pax_loader'
        if not sdk.exists():sdk=ROOT/'esphome/components/pax_loader'
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.c').write_text(program)
            subprocess.run(['cc','-std=gnu11','-I'+str(sdk),'-I'+str(ROOT/'apps/launcher_ui'),str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
