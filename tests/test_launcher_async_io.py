"""Exercise the actual bounded I/O mailbox, cancellation and task lifetime."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AsyncIOTests(unittest.TestCase):
    def test_mailbox_keeps_ui_alive_and_joins_before_unload(self):
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <unistd.h>
static int g_open_index=-1;
static pthread_t worker;
static void (*entry)(void *);
static bool deleted,fail_create;
static int pulses,cancel_at,active;
static void delay(int ms){usleep(ms*1000);}
static void *start(void *unused){(void)unused;entry(NULL);return NULL;}
static int create(void (*fn)(void *),const char *name,int stack,void *arg,int priority,void **out,int core){
 (void)name;(void)stack;(void)arg;(void)priority;(void)core;
 if(fail_create)return -1;
 entry=fn;assert(pthread_create(&worker,NULL,start,NULL)==0);*out=&worker;return 0;
}
static void destroy(void *handle){assert(handle==&worker && !active);pthread_cancel(worker);pthread_join(worker,NULL);deleted=true;}
static struct {int (*task_create)(void (*)(void *),const char *,int,void *,int,void **,int);void (*task_delete)(void *);void (*delay_ms)(int);} svc={create,destroy,delay},*g_svc=&svc;
#include "async_io.h"
static void io_ui_pulse(void){pulses++;if(cancel_at && pulses==cancel_at)g_open_index=-3;}
static int slow_job(void *arg){
 active=1;
 for(int i=0;i<40;i++){if(__atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE))break;delay(1);}
 active=0;return *(int *)arg;
}
static int stale_completed_job(void *arg){(void)arg;__atomic_add_fetch(&g_navigation_generation,1,__ATOMIC_ACQ_REL);return 7;}
int main(void){
 fail_create=true;assert(!io_start());io_finish();assert(!deleted);
 fail_create=false;assert(io_start());int result=42;
 assert(io_run(slow_job,&result)==42 && pulses>10 && !active);
 pulses=0;cancel_at=5;assert(io_run(slow_job,&result)==-2);assert(g_discard_io && !active);
 assert(io_run(slow_job,&result)==-2); /* stale work cannot submit again */
 g_discard_io=false;cancel_at=0;g_open_index=-1;
 assert(io_run(stale_completed_job,NULL)==-2); /* even if completion races the UI pulse */
 g_discard_io=false;assert(io_run(slow_job,&result)==42);
 io_finish();assert(deleted && !g_io_task && g_io_state==3);
 io_finish(); /* idempotent cleanup */
}
'''
        with tempfile.TemporaryDirectory() as td:
            src=Path(td)/'mailbox.c';src.write_text(code)
            exe=Path(td)/'mailbox'
            subprocess.run(['cc','-std=gnu11','-Wall','-pthread','-I',str(ROOT/'apps/launcher_ui'),str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)

if __name__=='__main__':unittest.main()
