"""Exercise launcher mouse inactivity with repeated HID reports and held clicks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
class LauncherCursor(unittest.TestCase):
    def test_real_mouse_callback_idle_and_wake(self):
        source = (ROOT / 'apps/launcher_ui/main.c').read_text()
        callback = source[source.index('static void mouse_read_cb('):source.index('static void create_mouse_cursor(')]
        code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#define MOUSE_CURSOR_IDLE_MS 3000U
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_INDEV_STATE_PRESSED 1
#define LV_INDEV_STATE_RELEASED 0
typedef int lv_indev_t;
typedef struct {struct {int x,y;} point;int state;bool continue_reading;} lv_indev_data_t;
static int object,*g_mouse_cursor=&object;
static bool hidden=true,g_mouse_cursor_seen;
static int g_mouse_x,g_mouse_y,g_mouse_buttons,g_canvas_w=1024,g_canvas_h=600;
static int g_mouse_wheel_pending;
static uint32_t now,g_mouse_last_activity_ms;
static int dx,dy,buttons,wheel;
static uint32_t lv_tick_get(void){return now;}
static void lv_obj_clear_flag(int *o,int f){assert(o==&object && f==1);hidden=false;}
static void lv_obj_add_flag(int *o,int f){assert(o==&object && f==1);hidden=true;}
static int read_mouse(int *x,int *y,int *b){*x=dx;*y=dy;*b=buttons;return 1;}
static int scroll(int *w){*w=wheel;return wheel!=0;}
static struct {int (*input_mouse_read)(int *,int *,int *);int (*input_mouse_scroll_read)(int *);} svc={read_mouse,scroll},*g_svc=&svc;
''' + callback + r'''
int main(void){
 lv_indev_data_t data={0};
 // A device reporting its stationary state must not reveal an unused cursor.
 now=100;mouse_read_cb(0,&data);assert(hidden && !g_mouse_cursor_seen);
 dx=10;mouse_read_cb(0,&data);assert(!hidden && g_mouse_x==10);
 dx=0;now=3099;mouse_read_cb(0,&data);assert(!hidden);
 now=3100;mouse_read_cb(0,&data);assert(hidden && data.state==0);
 buttons=1;now=3500;mouse_read_cb(0,&data);assert(!hidden && data.state==1);
 now=10000;mouse_read_cb(0,&data);assert(!hidden && data.state==1);
 buttons=0;now=10001;mouse_read_cb(0,&data);assert(!hidden && data.state==0);
 now=13001;mouse_read_cb(0,&data);assert(hidden);
 wheel=1;now=14000;mouse_read_cb(0,&data);assert(!hidden && g_mouse_wheel_pending==1);
 wheel=0;g_mouse_last_activity_ms=0xfffffff0U;now=0x20;mouse_read_cb(0,&data);assert(!hidden);
 now=0x1000;mouse_read_cb(0,&data);assert(hidden);
 dy=-5000;mouse_read_cb(0,&data);assert(!hidden && g_mouse_y==0);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src=Path(folder)/'cursor.c';src.write_text(code);exe=Path(folder)/'cursor'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
