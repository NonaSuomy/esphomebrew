"""Actual launcher catalog code: long directories, growth, cancellation and OOM."""
from pathlib import Path
import os, re, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]

def function(source,name):
    start=re.search(r'static [^\n;]+\b'+name+r'\([^;]*?\)\s*\{',source).start()
    pos=source.index('{',start)+1; depth=1
    while depth:
        if source[pos]=='{':depth+=1
        elif source[pos]=='}':depth-=1
        pos+=1
    return source[start:pos]

class CatalogPages(unittest.TestCase):
 def test_complete_catalog_long_names_and_failure_cleanup(self):
  source=(ROOT/'apps/launcher_ui/main.c').read_text()
  code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define MAX_APPS 2048
#define NAME_BYTES 64
#define PATH_BYTES 256
#define LIST_BYTES 8192
#define SOURCE_STORAGE 0
#include "pax_directory_page.h"
typedef struct { uint64_t size; int64_t mtime; int is_dir; } pax_file_stat_t;
#include "catalog_memory.h"
static CatalogMemory memory;
static bool fail_allocate,g_discard_io,g_catalog_limited;
static int live,allocations,pages,cancel_page,entries=700,legacy,malformed;
static void *allocate(size_t n){if(fail_allocate)return NULL;void *p=malloc(n);if(p){live++;allocations++;}return p;}
static void release(void *p){assert(live>0);live--;free(p);}
static char (*g_names)[NAME_BYTES],(*g_paths)[PATH_BYTES];
static char g_listing[LIST_BYTES];static int g_app_count,g_item_source[MAX_APPS];
static bool catalog_reserve(int n){
 if(!catalog_memory_reserve(&memory,n,allocate,release)){g_catalog_limited=true;return false;}
 g_names=memory.names;g_paths=memory.paths;return true;
}
static int list_directory_async(const char *request,char *buffer,int size){
 if(legacy){pages++;if(!strncmp(request,PAX_DIRECTORY_PREFIX,strlen(PAX_DIRECTORY_PREFIX)))return -1;
  strcpy(buffer,"legacy.pax");return 1;}
 assert(strncmp(request,PAX_DIRECTORY_PREFIX,strlen(PAX_DIRECTORY_PREFIX))==0);
 unsigned cursor=0;assert(sscanf(request+strlen(PAX_DIRECTORY_PREFIX),"%u:",&cursor)==1);
 struct pax_directory_page header={.magic=PAX_DIRECTORY_MAGIC,.next_offset=cursor};
 char *out=buffer+sizeof(header);pages++;
 for(int i=0;i<PAX_DIRECTORY_PAGE_ENTRIES && cursor<(unsigned)entries;i++,cursor++){
  char name[64];snprintf(name,sizeof(name),"long-package-name-abcdefghijklmnopqrstuvwxyz-%04u.pax",cursor);
  size_t n=strlen(name)+1;assert(header.bytes+n<(unsigned)size-sizeof(header));
  memcpy(out+header.bytes,name,n);header.bytes+=n;header.count++;header.next_offset++;
 }
 header.more=cursor<(unsigned)entries;if(malformed)header.bytes=8192;memcpy(buffer,&header,sizeof(header));
 if(cancel_page && pages==cancel_page)g_discard_io=true;
 return header.count;
}
static struct {void *file_list_dir;} services={(void *)1},*g_svc=&services;
'''+ '\n'.join(function(source,n) for n in ['copy_text','append_text','append_uint','append_exact','append_text_checked','ends_with','already_listed','add_folder_entries']) + r'''
int main(void){
 add_folder_entries("/sd/roms/pax/");assert(g_app_count==700 && pages==22);
 assert(allocations==4 && live==1 && memory.capacity==1024);
 for(int i=0;i<700;i++){char expected[256];snprintf(expected,sizeof(expected),"/sd/roms/pax/long-package-name-abcdefghijklmnopqrstuvwxyz-%04d.pax",i);assert(!strcmp(g_paths[i],expected));}
 add_folder_entries("/usb0/roms/pax/");assert(g_app_count==700); /* duplicate names retain SD preference */
 void *old=memory.block;fail_allocate=true;assert(!catalog_memory_reserve(&memory,1500,allocate,release));
 assert(memory.block==old && !strcmp(g_names[699],"long-package-name-abcdefghijklmnopqrstuvwxyz-0699.pax"));
 fail_allocate=false;assert(!catalog_memory_reserve(&memory,2049,allocate,release));
 catalog_memory_release(&memory,release);assert(live==0 && memory.capacity==0);
 g_app_count=0;pages=0;cancel_page=2;add_folder_entries("/sd/roms/pax/");
 assert(g_app_count==32 && g_discard_io && pages==2);
 catalog_memory_release(&memory,release);assert(live==0);
 g_discard_io=false;g_app_count=0;pages=0;cancel_page=0;fail_allocate=true;
 add_folder_entries("/sd/roms/pax/");assert(g_app_count==0 && !memory.block && live==0 && g_catalog_limited);
 fail_allocate=false;g_app_count=0;pages=0;entries=2100;
 add_folder_entries("/sd/roms/pax/");assert(g_app_count==2048 && g_catalog_limited && pages==64);
 catalog_memory_release(&memory,release);assert(live==0);
 g_app_count=0;pages=0;legacy=1;add_folder_entries("/sd/roms/pax/");
 assert(g_app_count==1 && pages==2 && !strcmp(g_names[0],"legacy.pax"));
 catalog_memory_release(&memory,release);assert(live==0);
 g_app_count=0;legacy=0;malformed=1;add_folder_entries("/sd/roms/pax/");
 assert(g_app_count==0 && live==0);
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'test.c').write_text(code)
   subprocess.run(['cc','-std=gnu11','-fsanitize=address,undefined','-I',str(ROOT/'apps/launcher_ui'),'-I',str(ROOT.parent/'pax-esphome/components/pax_loader'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
if __name__=='__main__':unittest.main()
