#!/usr/bin/env python3
"""Exercise the real touch polling code with bounced and intentional taps."""
from pathlib import Path
import subprocess, tempfile
src = (Path(__file__).resolve().parents[1] / 'pax_surface.c').read_text()
part = src[src.index('#define TOUCH_SLOP'):src.index('// ── Input ──')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
typedef int nsfb_t;
#define FB_W 1024
#define FB_H 600
#define NSFB_KEY_MOUSE_1 1
#define fb_toolbar_size 1
#define fb_furniture_size 2
#define nsoption_int(x) ((x)==1 ? 30 : 20)
static int64_t fake_time;
static int fake_down, fake_x=100, fake_y=100, taps, scrolls;
static int touch_read(int *x,int *y) {*x=fake_x;*y=fake_y;return fake_down;}
static void log_printf(const char *fmt,...) {(void)fmt;}
static struct {int (*touch_read)(int*,int*); void (*log_printf)(const char*,...);} services={touch_read,log_printf};
static void *unused;
static __typeof__(services) *pax_svc=&services;
static int64_t pax_time_us(void) {return fake_time;}
static void push_move(bool a,int b,int c) {(void)a;(void)b;(void)c;}
static void push_tap(int k) {(void)k;++taps;}
static void push_key(bool d,int k) {(void)d;(void)k;}
static void push_scroll(int x,int y) {(void)x;(void)y;++scrolls;}
static void show_cursor(nsfb_t *n,bool v) {(void)n;(void)v;}
'''
suffix = r'''
static void sample(int64_t t,int down,int x,int y) {fake_time=t;fake_down=down;fake_x=x;fake_y=y;poll_touch(unused);}
int main(void) {
 sample(1000000,1,100,100); sample(1020000,0,0,0);
 sample(1030000,1,100,100); sample(1040000,0,0,0);
 sample(1055000,1,100,100); sample(1065000,0,0,0);
 assert(taps==0); sample(1155000,0,0,0); assert(taps==1);
 sample(1400000,1,100,100); sample(1420000,0,0,0); sample(1520000,0,0,0); assert(taps==2);
 sample(1800000,1,100,100); sample(1840000,1,100,80); sample(1860000,0,0,0);
 sample(1960000,0,0,0); assert(taps==2 && scrolls>0);
 sample(2200000,1,100,100); sample(2910000,1,100,100); assert(taps==3);
 sample(2920000,0,0,0); sample(3020000,0,0,0); assert(taps==3);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c, binary = Path(d)/'test.c', Path(d)/'test'
    c.write_text(prefix+part+suffix)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('Touch bounce, separate taps, scrolling and hold-to-click passed')
