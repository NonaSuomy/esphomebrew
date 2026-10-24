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
#include "list_sort.h"
#define SETTINGS_FOCUS_COUNT SETTINGS_COUNT_PLACEHOLDER
#define LIST_PREF_FILE "view-preference"
static bool g_hide_pax_extension,g_list_file_details;
static int g_sort_mode,g_size_unit;
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
 pax_list_label(text,sizeof(text),name,true);assert(strcmp(text,"scummvm-sky")==0);assert(strcmp(name,"scummvm-sky.pax")==0);
 pax_list_label(text,sizeof(text),name,false);assert(strcmp(text,name)==0);
 pax_list_label(text,sizeof(text),"Demo.PAX",true);assert(strcmp(text,"Demo")==0);
 pax_list_label(text,sizeof(text),"pax-example.zip",true);assert(strcmp(text,"pax-example.zip")==0);
 pax_list_details(text,sizeof(text),&stat);assert(strcmp(text,"5368709120 bytes | 2000-02-29 18:30 UTC")==0);
 text[0]=0;pax_info_date(text,sizeof(text),4107542400LL);assert(strcmp(text,"2100-03-01 00:00 UTC")==0);
 text[0]=0;pax_info_date(text,sizeof(text),0);assert(strcmp(text,"Unavailable")==0);
 text[0]=0;pax_info_date(text,sizeof(text),-1);assert(strcmp(text,"Unavailable")==0);
 pax_list_details(text,sizeof(text),NULL);assert(strstr(text,"File details unavailable"));
 char tiny[4];pax_list_details(tiny,sizeof(tiny),&stat);assert(tiny[3]==0);
 app_services_t svc={0};svc.file_open=open_file;svc.file_read=read_file;svc.file_write=write_file;svc.file_close=close_file;g_svc=&svc;
 load_list_preferences();assert(!g_hide_pax_extension && !g_list_file_details);
 g_hide_pax_extension=g_list_file_details=true;assert(save_list_preferences());g_hide_pax_extension=g_list_file_details=false;
 load_list_preferences();assert(g_hide_pax_extension && g_list_file_details);
 g_list_file_details=false;assert(save_list_preferences());load_list_preferences();assert(g_hide_pax_extension && !g_list_file_details);
 strcpy(stored,"11\n");load_list_preferences();assert(g_sort_mode==PAX_SORT_SOURCE && g_list_file_details);
 g_sort_mode=PAX_SORT_NEWEST;assert(save_list_preferences());g_sort_mode=0;load_list_preferences();assert(g_sort_mode==PAX_SORT_NEWEST);
 strcpy(stored,"119\n");load_list_preferences();assert(g_sort_mode==PAX_SORT_SOURCE);
 g_size_unit=PAX_SIZE_MB;assert(save_list_preferences());g_size_unit=0;load_list_preferences();assert(g_size_unit==PAX_SIZE_MB);
 strcpy(stored,"113\n");load_list_preferences();assert(g_size_unit==PAX_SIZE_BYTES && g_sort_mode==PAX_SORT_NEWEST);
 strcpy(stored,"1139\n");load_list_preferences();assert(g_size_unit==PAX_SIZE_BYTES);
 text[0]=0;pax_info_size(text,sizeof(text),UINT64_MAX,PAX_SIZE_GB);assert(strcmp(text,"18446744073.70 GB")==0);
 text[0]=0;pax_info_size(text,sizeof(text),1500,PAX_SIZE_KB);assert(strcmp(text,"1.50 KB")==0);
 text[0]=0;pax_info_size(text,sizeof(text),999,PAX_SIZE_AUTO);assert(strcmp(text,"999 bytes")==0);
 text[0]=0;pax_info_size(text,sizeof(text),1000,PAX_SIZE_AUTO);assert(strcmp(text,"1.00 KB")==0);
 text[0]=0;pax_info_size(text,sizeof(text),0,PAX_SIZE_MB);assert(strcmp(text,"0.00 MB")==0);
 bool seen[SETTINGS_FOCUS_COUNT]={0};int focus=18;for(int i=0;i<SETTINGS_FOCUS_COUNT;i++){assert(focus>=0 && focus<SETTINGS_FOCUS_COUNT && !seen[focus]);seen[focus]=true;int next=settings_focus_step(focus,1);assert(settings_focus_step(next,-1)==focus);focus=next;}assert(focus==18 && seen[22] && seen[23] && seen[24] && seen[25] && seen[26] && seen[27] && seen[28] && seen[29]);
 return 0;
}
'''
        count=re.search(r'#define SETTINGS_FOCUS_COUNT (\d+)',source).group(1)
        program=program.replace('SETTINGS_COUNT_PLACEHOLDER',count)
        sdk=ROOT.parent/'pax-esphome/components/pax_loader'
        if not sdk.exists():sdk=ROOT/'esphome/components/pax_loader'
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.c').write_text(program)
            subprocess.run(['cc','-std=gnu11','-I'+str(sdk),'-I'+str(ROOT/'apps/launcher_ui'),str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
