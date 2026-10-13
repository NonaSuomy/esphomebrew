"""Exercise the shared runtime's actual ZIP selection and stale-ROM fallback."""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ZipSelectionTest(unittest.TestCase):
    def test_zip_selection_reaches_extractor(self):
        source = r"""
#include <assert.h>
#include "runtime.h"
const app_services_t *_pax_svc;
static char *configured;
static const char *sidecar;
static int calls;
static char *get(void){return configured;}
static void *open_file(const char *p,const char *m){(void)p;(void)m;return sidecar?(void*)1:NULL;}
static size_t read_file(void *p,size_t s,size_t n,void *f){(void)f;size_t len=strlen(sidecar);if(len>s*n)len=s*n;memcpy(p,sidecar,len);return len/s;}
static int close_file(void *f){(void)f;return 0;}
static int extract(const char *p,const char *e,const char *d,const char *s,char *out,size_t n){
 assert(!strcmp(p,"/sd/roms/nes/game.ZIP"));assert(!strcmp(e,".nes|"));assert(!strcmp(d,"/sd/roms/pax/.zipcache"));assert(!strcmp(s,"nes"));calls++;snprintf(out,n,"/sd/roms/pax/.zipcache/nes.nes");return 0;}
int main(void){
 app_services_t svc={0};svc.settings_rom_path_get=get;svc.file_open=open_file;svc.file_read=read_file;svc.file_close=close_file;svc.file_zip_extract_first=extract;_pax_svc=&svc;char out[512];
 configured="/sd/roms/nes/game.ZIP";assert(emu_rom_path("nes",".nes|",out,sizeof out)==0);assert(calls==1);
 configured="";sidecar="/sd/roms/nes/game.ZIP\n";assert(emu_rom_path("nes",".nes|",out,sizeof out)==0);assert(calls==2);
 configured="/sd/roms/gb/game.gb";assert(emu_rom_path("nes",".nes|",out,sizeof out)==0);assert(calls==3);
 configured="/sd/roms/gb/game.gb";sidecar=NULL;assert(emu_rom_path("nes",".nes|",out,sizeof out)==-1);assert(calls==3);
 configured="/sd/roms/nes/game.nes";assert(emu_rom_path("nes",".nes|",out,sizeof out)==0);assert(!strcmp(out,configured));return 0;}
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            (directory / "test.c").write_text(source)
            subprocess.run(
                [
                    "cc",
                    "-std=gnu11",
                    "-O2",
                    "-I" + str(ROOT / "apps/pax_emulator_common"),
                    "-I" + str(ROOT.parent / "pax-esphome/components/pax_loader"),
                    str(directory / "test.c"),
                    "-o",
                    str(directory / "test"),
                ],
                check=True,
            )
            subprocess.run([str(directory / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
