#!/usr/bin/env python3
"""Check the PC Engine shared-menu actions and adjustable options."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/"pax_main.c").read_text()
part=s[s.index("static void pce_options_load("):s.index("/* Globals declared",s.index("static void pce_menu_action("))]
prefix=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PAX_APP_SIDE 1
#include "psram_app.h"
static unsigned pce_video_mode=1,pce_zoom=1;
static bool pce_audio_enabled=true;
static unsigned pce_state_slot; static uint32_t pce_rom_id=123;
static int last_action; static unsigned last_slot;
static bool pce_state_save(uint32_t rom,unsigned slot){assert(rom==123);last_action=5;last_slot=slot;return true;}
static bool pce_state_load(uint32_t rom,unsigned slot){assert(rom==123);last_action=6;last_slot=slot;return true;}
static bool pce_state_delete(uint32_t rom,unsigned slot){assert(rom==123);last_action=7;last_slot=slot;return true;}
static char stored[64]; static size_t length;
static void *open_file(const char *path,const char *mode){(void)path;if(*mode=='w')length=0;return stored;}
static size_t read_file(void *p,size_t s,size_t n,void *f){(void)f;assert(s==1);if(n>length)n=length;memcpy(p,stored,n);return n;}
static size_t write_file(const void *p,size_t s,size_t n,void *f){(void)f;assert(s==1 && n<sizeof(stored));memcpy(stored,p,n);length=n;return n;}
static int close_file(void *f){(void)f;return 0;}
static pax_game_menu_page_t seen; static int closed;
static int set_page(const pax_game_menu_page_t *p){if(p)seen=*p;else closed++;return 0;}
static app_services_t svc={.game_menu_set_page=set_page,.file_open=open_file,.file_read=read_file,.file_write=write_file,.file_close=close_file};
static app_services_t *_pax_svc=&svc;
"""
suffix=r"""
int main(void){
 pce_menu_action(NULL,0,0);assert(seen.count==8 && strstr(seen.items[0].label,"BALANCED"));
 pce_menu_action(NULL,1,1);assert(pce_video_mode==2 && strstr(seen.items[0].label,"AUDIO PRIORITY"));
 pce_menu_action(NULL,1,-1);assert(pce_video_mode==1);
 pce_menu_action(NULL,2,0);assert(!pce_audio_enabled && strstr(seen.items[1].label,"OFF"));
 pce_menu_action(NULL,3,-1);assert(pce_zoom==0 && strstr(seen.items[2].label,"1.5X"));
 pce_video_mode=0;pce_audio_enabled=true;pce_zoom=2;pce_options_load();
 assert(pce_video_mode==1 && !pce_audio_enabled && pce_zoom==0);
 pce_menu_action(NULL,4,1);assert(pce_state_slot==1);
 pce_menu_action(NULL,5,0);assert(last_action==5 && last_slot==1 && strstr(seen.status,"SAVED"));
 pce_menu_action(NULL,6,0);assert(last_action==6 && last_slot==1);
 pce_menu_action(NULL,7,0);assert(last_action==7 && last_slot==1);
 pce_menu_action(NULL,8,0);assert(closed==1);return 0;
}
"""
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/"test.c";b=Path(d)/"test";c.write_text(prefix+part+suffix)
 subprocess.run(["cc","-std=gnu11","-Wall","-Wextra","-Werror","-I.cache/loader_sdk",str(c),"-o",str(b)],check=True)
 subprocess.run([str(b)],check=True)
print("PC Engine shared-menu options passed")
