"""Exercise real launcher progress arithmetic and UI bridge timeout recovery."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class C6SettingsTests(unittest.TestCase):
    def test_progress_and_timeout_recovery(self):
        source = (ROOT / 'apps/launcher_ui/main.c').read_text()
        start = source.index('static uint32_t c6_progress_percent(')
        end = source.index('\n}\n', start) + 3
        progress = source[start:end]
        start = source.index('static bool consume_c6_action(')
        end = source.index('\n}\n',start)+3
        consume = source[start:end]
        start = source.index('      const int pump_result=')
        end = source.index('      /* C6 jobs are asynchronous.', start)
        pump = source[start:end]
        sdk = ROOT.parent / 'pax-esphome/components/pax_loader'
        if not sdk.exists():
            sdk = ROOT / 'esphome/components/pax_loader'
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pax_c6_update.h"
''' + progress + r'''
static int g_open_index,requests;static uint32_t command;
static void request_c6_update(uint32_t c){assert(g_open_index==-1);requests++;command=c;}
''' + consume + r'''
static int next,delays,logs,values[4];
static int pump_ui_on_host(void *arg){(void)arg;return 0;}
static int call(int (*callback)(void *),void *arg,uint32_t timeout){(void)callback;(void)arg;assert(timeout==3000);return values[next++];}
static void delay(int ms){assert(ms==20);delays++;}
static int log_message(const char *format,...){(void)format;logs++;return 0;}
static struct {int (*ui_call_sync)(int (*)(void *),void *,uint32_t);void (*delay_ms)(int);int (*log_printf)(const char *,...);} services={call,delay,log_message},*svc=&services;
static int run(int count){int frames=0;next=delays=logs=0;while(next<count){
''' + pump + r'''
frames++;}return frames;}
int main(void){
 g_open_index=-71;assert(consume_c6_action() && requests==1 && command==PAX_C6_PREPARE);
 g_open_index=-72;assert(consume_c6_action() && requests==2 && command==PAX_C6_INSTALL);
 g_open_index=-9;assert(!consume_c6_action() && g_open_index==-9 && requests==2);
 pax_c6_status_t state={0};assert(c6_progress_percent(&state)==0);
 state.done=50;state.total=100;assert(c6_progress_percent(&state)==50);
 state.done=UINT32_MAX;state.total=UINT32_MAX;assert(c6_progress_percent(&state)==100);
 state.total=1;assert(c6_progress_percent(&state)==100);
 state.done=0;assert(c6_progress_percent(&state)==0);
 values[0]=-3;values[1]=-2;values[2]=0;assert(run(3)==1 && delays==2 && logs==0 && next==3);
 values[0]=-1;assert(run(1)==0 && delays==0 && logs==1);
 return 0;}
'''
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / 'test.c').write_text(code)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(sdk), str(folder/'test.c'), '-o', str(folder/'test')], check=True)
            subprocess.run([str(folder/'test')], check=True)

if __name__ == '__main__':
    unittest.main()
