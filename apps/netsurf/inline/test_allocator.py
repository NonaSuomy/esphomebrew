#!/usr/bin/env python3
"""Compile the browser allocator itself against a mock PAX service table."""
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[1]/'pax_syscalls.c').read_text()
part=source[source.index('typedef struct block {'):source.index('// newlib\'s own locks')]
prefix=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define PAX_NETSURF_INLINE_MEDIA 1
#define PAX_MEM_CAP_INTERNAL 1
#define PAX_MEM_CAP_DMA 2
#define PAX_MEM_CAP_SPIRAM 4
struct _reent;
static unsigned allocated, released;
static void *alloc(size_t n) { void *p=malloc(n); if(p) ++allocated; return p; }
static void *caps(size_t n,int flags) { (void)flags; return alloc(n); }
static void release(void *p) { if(p) ++released; free(p); }
static void log_text(const char *s,...) { (void)s; }
static struct services { void *(*mem_caps_alloc)(size_t,int); void *(*mem_alloc)(size_t); void (*mem_free)(void*); void (*log_printf)(const char*,...); } svc={caps,alloc,release,log_text}, *pax_svc=&svc;
static void *s_heap_lock;
static void lock_take(void *p) { (void)p; }
static void lock_give(void *p) { (void)p; }
'''
suffix=r'''
int main(void) {
 for (size_t a=16;a<=4096;a*=2) {
  unsigned char *p=pax_alloc_aligned(123,a); assert(p && ((uintptr_t)p%a)==0);
  memset(p,0xa5,123); unsigned char *q=block_realloc(p,456); assert(q);
  for (int i=0;i<123;i++) { assert(q[i]==0xa5); }
  block_free(q);
 }
 assert(!block_alloc_aligned(SIZE_MAX,128)); assert(!block_alloc_aligned(10,24));
 void *p=NULL; assert(__wrap_posix_memalign(&p,128,1024)==0 && ((uintptr_t)p%128)==0);
 assert(__wrap_posix_memalign(&p,3,10)==EINVAL); block_free(p);
 assert(block_alloc(200)); assert(block_alloc_aligned(50,64)); pax_free_all_memory();
 assert(allocated==released); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='pax-ns-alloc-') as d:
 p=Path(d)/'test.c'; p.write_text(prefix+part+suffix); exe=Path(d)/'test'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('Browser aligned allocation, realloc and cleanup tests passed')
