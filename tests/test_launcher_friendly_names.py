from pathlib import Path
import importlib.util
import io
import json
import re
import subprocess
import tempfile
import unittest
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[1]

def function(source, name):
    match = re.search(r'static [^\n;]+\b'+name+r'\([^;]*?\)\s*\{', source)
    opening = source.index('{', match.start()); end = opening+1; level = 1
    while level:
        if source[end] == '{': level += 1
        elif source[end] == '}': level -= 1
        end += 1
    return source[match.start():end]

class FriendlyNamesTests(unittest.TestCase):
    def test_live_directory_and_store_publish_safe_titles_without_renaming_assets(self):
        spec=importlib.util.spec_from_file_location('serve_dev_titles',ROOT/'tools/serve_dev.py')
        mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
        with tempfile.TemporaryDirectory() as td:
            folder=Path(td)
            fixtures={'anotherworld':'Another World', 'special':'Café: "A&B" / + 100%',
                      'blank':'  ', 'bad':None, 'missing':None, 'control':'bad\nname'}
            for name,title in fixtures.items():
                (folder/f'{name}.pax').write_bytes(b'PAX')
                if title is not None: (folder/f'{name}.json').write_text(json.dumps({'title':title}))
            (folder/'bad.json').write_text('{')
            handler=object.__new__(mod.PaxRequestHandler);handler.path='/'
            handler.send_response=lambda *args:None;handler.send_header=lambda *args:None;handler.end_headers=lambda:None
            page=handler.list_directory(td).read().decode()
            for name in ('anotherworld','special'):
                self.assertIn(f'href="{name}.pax" data-pax-size="3"',page)
                self.assertIn(f'data-pax-title-url="{quote(fixtures[name],safe="")}"',page)
            for name in ('blank','bad','missing','control'):
                tag=re.search(r'<a href="'+name+r'\.pax"[^>]*>',page).group()
                self.assertNotIn('data-pax-title-url',tag)
        spec=importlib.util.spec_from_file_location('catalog_titles',ROOT/'tools/make_catalog.py')
        catalog=importlib.util.module_from_spec(spec);spec.loader.exec_module(catalog)
        app={'name':'anotherworld','title':fixtures['special'],'version':'1','file':'anotherworld.pax','size':3,'sha256':'a'*64}
        page=catalog.render('owner/repo',[app])
        self.assertIn('href="anotherworld-1.pax"',page)
        self.assertIn(f'data-pax-title-url="{quote(fixtures["special"],safe="")}"',page)

    def test_launcher_loads_titles_falls_back_and_searches_both_title_and_filename(self):
        source=(ROOT/'apps/launcher_ui/main.c').read_text()
        code=r"""
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef struct {uint64_t size;int64_t mtime;} pax_file_stat_t;
#include "network_file_info.h"
#define MAX_APPS 4
#define NAME_BYTES 64
#define PATH_BYTES 256
#define REMOTE_BYTES 204800
#define SOURCE_ARCHIVE 3
#define VISIBLE_ROWS 5
static char g_names[MAX_APPS][NAME_BYTES],g_titles[MAX_APPS][NAME_BYTES],g_paths[MAX_APPS][PATH_BYTES],g_remote[REMOTE_BYTES];
static int g_app_count=3,g_source=0,g_view_count,g_selected,g_first_visible,g_visible_indices[MAX_APPS];
static bool g_favorites_mode=false;
static char g_search_text[128];
static bool path_is_favorite(const char *p){(void)p;return true;}
static int find_bytes(const char *text,int length,const char *needle){(void)length;const char *p=strstr(text,needle);return p?(int)(p-text):-1;}
static int reads=0;
static int read_json_file(const char *p){reads++;assert(strstr(p,".json"));if(strstr(p,"missing"))return -1;strcpy(g_remote,strstr(p,"blank")?"{\"title\":\"  \"}":"{\"title\":\"Another World\"}");return (int)strlen(g_remote);}
static int http_fetch(const char *p,char *body,int cap){(void)body;(void)cap;return read_json_file(p);}
""" + '\n'.join(function(source,n) for n in ('copy_text','ends_with','has_prefix','replace_package_suffix','json_string_field','display_name_for','contains_case_insensitive','load_list_title','apply_filter')) + r"""
int main(void){
 strcpy(g_names[0],"anotherworld.pax");strcpy(g_paths[0],"/sd/roms/pax/anotherworld.pax");
 strcpy(g_names[1],"missing.pax");strcpy(g_paths[1],"http://host/missing.pax");
 strcpy(g_names[2],"blank.pax");strcpy(g_paths[2],"/usb0/roms/pax/blank.pax");
 for(int i=0;i<3;i++)load_list_title(i);
 assert(!strcmp(display_name_for(0),"Another World"));
 assert(!strcmp(display_name_for(1),"missing.pax"));assert(!strcmp(display_name_for(2),"blank.pax"));
 assert(!strcmp(g_names[0],"anotherworld.pax"));assert(!strcmp(g_paths[0],"/sd/roms/pax/anotherworld.pax"));
 int before=reads;load_list_title(0);assert(reads==before);
 strcpy(g_search_text,"Another World");apply_filter();assert(g_view_count==1 && g_visible_indices[0]==0);
 strcpy(g_search_text,"anotherworld.pax");apply_filter();assert(g_view_count==1 && g_visible_indices[0]==0);
 char title[64];const char *tag="<a data-pax-title-url=\"Caf%C3%A9%3A%20%22A%26B%22%20%2F%20%2B%20100%25\">";
 assert(pax_network_title(tag,strlen(tag),title,sizeof(title)));assert(!strcmp(title,"Café: \"A&B\" / + 100%"));
 const char *bad[]={"<a data-pax-title-url=''>","<a data-pax-title-url='%00'>","<a data-pax-title-url='%ZZ'>","<a data-pax-title-url='%20%20'>","<a data-pax-title-url='%0A'>","<a data-pax-title-url='abc"};
 for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++){assert(!pax_network_title(bad[i],strlen(bad[i]),title,sizeof(title)));assert(!title[0]);}
 assert(!pax_network_title(tag,strlen(tag),title,4));assert(!title[0]);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.c').write_text(code)
            subprocess.run(['cc','-std=gnu11','-I',str(ROOT/'apps/launcher_ui'),str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
