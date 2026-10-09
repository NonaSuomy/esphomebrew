"""Check both NES ROM-load failure and successful core ownership transfer."""
from pathlib import Path
import subprocess, sys, tempfile, unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_pax as bp

class NesOwnershipTests(unittest.TestCase):
    def test_rom_path_is_freed_once(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            (temp / 'runtime.h').write_text('#include "psram_app.h"\n#include <string.h>\nstatic int emu_rom_path(const char *id,const char *ext,char *out,size_t len) {(void)id;(void)ext;(void)len;strcpy(out,"/demo.nes");return 0;}\n')
            fixture = temp / 'ownership.c'
            fixture.write_text(r'''
#include "psram_app.h"
#include <assert.h>
#include <string.h>
extern int app_entry(const app_services_t *);
extern const app_services_t *_pax_svc;
volatile int nofrendo_quit_flag, pax_exit_requested;
char *pax_nes_rom_data;
static char path[512];
static int frees, fail_load;
static void *allocate(size_t n) {assert(n<=sizeof(path));return path;}
static void release(void *p) {assert(p==path);assert(++frees==1);}
static void *open_file(const char *a,const char *b) {(void)a;(void)b;return 0;}
static void clear(uint16_t c) {(void)c;}
static void flush(void) {}
static void audio(int rate) {(void)rate;}
static int log_message(const char *fmt,...) {(void)fmt;return 0;}
static int task(void (*fn)(void *),const char *name,uint32_t depth,void *arg,int priority,void *handle,int core) {(void)fn;(void)name;(void)depth;(void)arg;(void)priority;(void)core;*(void**)handle=0;return 0;}
int pax_nes_load_rom(const char *p) {(void)p;return fail_load?-1:0;}
void pax_nes_cleanup(void) {}
void pax_nes_options_load_for_current_rom(const app_services_t *s) {(void)s;}
int nofrendo_main(int argc,char **argv) {assert(argc==1);_pax_svc->mem_free(argv[0]);return 0;}
int main(void) {
 app_services_t svc={0};svc.abi_version=PAX_ABI_VERSION;svc.mem_alloc=allocate;svc.mem_free=release;svc.file_open=open_file;svc.log_printf=log_message;svc.audio_init=audio;svc.task_create=task;svc.display_clear=clear;svc.display_flush=flush;
 fail_load=0;assert(app_entry(&svc)==0);assert(frees==1);
 frees=0;fail_load=1;assert(app_entry(&svc)==-1);assert(frees==1);
 return 0;
}
''')
            binary = temp / 'ownership'
            subprocess.run(['gcc', '-std=gnu11', '-Wno-unused-function', '-I'+str(temp), '-I'+str(bp.get_loader_sdk()), str(root/'apps/nes/pax_main.c'), str(fixture), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True)
