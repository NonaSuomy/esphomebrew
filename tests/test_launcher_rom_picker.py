from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class RomPickerTests(unittest.TestCase):
    def test_picker_filters_and_launches_with_emulator_selection(self):
        source=(ROOT/'apps/launcher_ui/main.c').read_text()
        routines=source[source.index('static void rom_picker_add_folder'):source.index('static int build_rom_picker_overlay')]
        harness=r"""
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "rom_profiles.h"
#define MAX_ROM_CHOICES 128
#define ROM_PICKER_VISIBLE_ROWS 10
#define PATH_BYTES 256
#define NAME_BYTES 64
static app_services_t services;
static const app_services_t *g_svc=&services;
static const pax_rom_profile_t *g_rom_picker_profile;
static int g_rom_picker_count,g_rom_picker_selected,g_rom_picker_first,g_rom_picker_rows_shown;
static bool g_rom_picker_rom_selected;
static char g_rom_picker_paths[128][256],g_rom_picker_names[128][64],g_listing[4096],g_install_status[256];
static char configured[256],sidecar_path[256],cache_name[64],filter[128];
static int extracted,writes,fail_extract;
static void copy_text(char *out,size_t n,const char *text){snprintf(out,n,"%s",text);}
static void append_text(char *out,size_t n,const char *text){snprintf(out+strlen(out),n-strlen(out),"%s",text);}
static bool extract_ends_with_ci(const char *name,const char *ext){size_t a=strlen(name),b=strlen(ext);return a>=b&&!strcasecmp(name+a-b,ext);}
static int list(const char *folder,char *out,int n){
 const char entries[]="demo.NES\0game.GBC\0set.ZIP\0wrong.txt\0directory.nes/\0a_very_long_game_name_which_exceeds_the_display_label_but_must_still_be_selectable.GBC\0";
 if(strstr(folder,"/nesplus/"))return -1;
 assert(n>(int)sizeof(entries));memcpy(out,entries,sizeof(entries));return 6;
}
static int zip(const char *path,const char *extensions,const char *folder,const char *stem,char *out,size_t n){
 (void)path;assert(!strcmp(folder,"/sd/roms/pax/.zipcache"));extracted++;
 copy_text(cache_name,sizeof(cache_name),stem);copy_text(filter,sizeof(filter),extensions);
 snprintf(out,n,"/sd/roms/pax/.zipcache/%s.gbc",stem);return fail_extract?-1:0;
}
static void set(const char *path){copy_text(configured,sizeof(configured),path);}
static void *open_file(const char *path,const char *mode){assert(!strcmp(mode,"wb"));copy_text(sidecar_path,sizeof(sidecar_path),path);return &services;}
static size_t write_file(const void *ptr,size_t size,size_t count,void *f){(void)ptr;(void)f;writes++;return size*count;}
static int close_file(void *f){(void)f;return 0;}
static int log_text(const char *text,...){(void)text;return 0;}
"""+routines+r"""
int main(){
 services.file_list_dir=list;services.file_zip_extract_first=zip;services.settings_rom_path_set=set;
 services.file_open=open_file;services.file_write=write_file;services.file_close=close_file;services.log_printf=log_text;
 assert(pax_rom_profile_for_app("/sd/roms/pax/nes.pax"));
 assert(!strcmp(pax_rom_profile_for_app("https://host/GB-debug.PAX")->id,"gb"));
 assert(!pax_rom_profile_for_app("nes.pap"));assert(!pax_rom_profile_for_app("nes-unknown.pax"));
 assert(!pax_rom_profile_for_app("doom.pax"));assert(!pax_rom_profile_for_app(NULL));
 for(size_t i=0;i<sizeof(pax_rom_profiles)/sizeof(pax_rom_profiles[0]);i++){
  char name[64];snprintf(name,sizeof(name),"%s.pax",pax_rom_profiles[i].id);
  assert(pax_rom_profile_for_app(name)==&pax_rom_profiles[i]);
  assert(!pax_rom_profile_accepts(&pax_rom_profiles[i],"invalid.txt"));
  assert(pax_rom_profile_accepts(&pax_rom_profiles[i],"set.ZIP"));
 }
 rom_picker_load_games("nes.pax");assert(g_rom_picker_count==4);
 assert(!strcmp(g_rom_picker_paths[0],"/sd/roms/nes/demo.NES"));
 rom_picker_add_folder("/sd/roms/nes/","SD  ");assert(g_rom_picker_count==4);
 assert(rom_picker_set_selected_game());assert(!strcmp(configured,"/sd/roms/nes/demo.NES"));assert(!strcmp(sidecar_path,"/sd/roms/pax/nes.rom"));
 rom_picker_load_games("gb-debug.pax");assert(g_rom_picker_count==6);
 assert(strstr(g_rom_picker_paths[2],"still_be_selectable.GBC"));
 assert(!strcmp(g_rom_picker_paths[0],"/sd/roms/gb/game.GBC"));g_rom_picker_selected=1;
 assert(rom_picker_set_selected_game());assert(extracted==1);assert(!strcmp(filter,".gb|.gbc|"));assert(!strncmp(cache_name,"gb_",3));assert(!strcmp(sidecar_path,"/sd/roms/pax/gb.rom"));
 fail_extract=1;configured[0]=0;assert(!rom_picker_set_selected_game());assert(!configured[0]);fail_extract=0;
 rom_picker_load_games("neogeo.pax");assert(g_rom_picker_count==2);assert(rom_picker_set_selected_game());
 assert(extracted==2);assert(!strcmp(configured,"/sd/roms/neogeo/set.ZIP"));assert(!strcmp(sidecar_path,"/sd/roms/pax/neogeo.rom"));
 rom_picker_load_games("nesplus.pax");assert(g_rom_picker_count==4);assert(strstr(g_rom_picker_paths[0],"/roms/nes/"));
 g_rom_picker_count=0;assert(!rom_picker_set_selected_game());
 return 0;
}
"""
        sdk=ROOT.parent/'pax-esphome/components/pax_loader'
        if not sdk.exists():sdk=ROOT/'esphome/components/pax_loader'
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.c').write_text(harness)
            subprocess.run(['cc','-std=gnu11','-Werror','-include','strings.h','-I'+str(sdk),'-I'+str(ROOT/'apps/launcher_ui'),str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

    def test_profiles_match_active_emulator_formats(self):
        profile=(ROOT/'apps/launcher_ui/rom_profiles.h').read_text()
        for app,title,extensions in re.findall(r'\{"([^"]+)", "([^"]+)", "([^"]+)"\}',profile):
            source=next((ROOT/'apps'/app).glob('pax_main.*')).read_text()
            match=re.search(r'emu_(?:rom_path|load_rom)\("'+app+r'",\s*"([^"]+)"',source)
            self.assertIsNotNone(match,app)
            self.assertEqual(match.group(1),extensions,app)
