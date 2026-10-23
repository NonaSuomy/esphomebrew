"""Exercise held navigation timing through the launcher's actual list routing."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class InputRepeat(unittest.TestCase):
    def test_held_direction_scrolls_without_catchup_and_stops_on_release(self):
        source = (ROOT/'apps/launcher_ui/main.c').read_text()
        start = source.index('  } else if (g_view_count > 0) {', source.index('static bool poll_controller(void)'))
        end = source.index('\n  } else if (up_pressed) {', start)
        routing = source[start:end].replace('  } else if', '  if', 1) + '\n  }\n'
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "input_repeat.h"
#define VISIBLE_ROWS 5
static int g_view_count=100,g_selected,g_first_visible,g_toolbar_focus=-1,g_source;
static bool g_favorites_mode;
static PaxInputRepeat keys[4];
static void sample(bool up,bool right,bool down,bool left,uint32_t now){
 bool up_pressed=pax_input_repeat_step(&keys[0],up,now);
 bool right_pressed=pax_input_repeat_step(&keys[1],right,now);
 bool down_pressed=pax_input_repeat_step(&keys[2],down,now);
 bool left_pressed=pax_input_repeat_step(&keys[3],left,now);
 bool page_up_pressed=false,page_down_pressed=false,changed=false;
'''+routing+r'''
 (void)changed;
}
int main(void){
 sample(false,false,true,false,0);assert(g_selected==1);
 sample(false,false,true,false,399);assert(g_selected==1);
 sample(false,false,true,false,400);assert(g_selected==2);
 sample(false,false,true,false,479);assert(g_selected==2);
 sample(false,false,true,false,480);assert(g_selected==3);
 sample(false,false,false,false,500);sample(false,false,false,false,900);assert(g_selected==3);
 sample(false,false,true,false,1000);assert(g_selected==4); /* next tap is immediate */
 sample(false,false,true,false,4000);assert(g_selected==5); /* no burst after a slow operation */
 sample(false,false,true,false,4001);assert(g_selected==5);
 sample(false,false,false,true,4002);assert(g_selected==4); /* switching direction is immediate */
 sample(false,false,false,false,4010);
 g_selected=g_first_visible=0;memset(keys,0,sizeof(keys));
 for(unsigned time=0;time<=1200;time+=20)sample(false,false,true,false,time);
 assert(g_selected==12 && g_first_visible==8); /* continuous hold scrolls beyond one screen */
 sample(false,false,false,false,1220);
 for(unsigned time=1240;time<2000;time+=20)sample(false,false,false,false,time);
 assert(g_selected==12);
 /* Millisecond counter rollover keeps the same initial and repeat delays. */
 memset(keys,0,sizeof(keys));g_selected=10;
 uint32_t begin=UINT32_MAX-200;
 sample(true,false,false,false,begin);assert(g_selected==9);
 sample(true,false,false,false,begin+399U);assert(g_selected==9);
 sample(true,false,false,false,begin+400U);assert(g_selected==8);
 sample(true,false,false,false,begin+480U);assert(g_selected==7);
}
'''
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.c').write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-I'+str(ROOT/'apps/launcher_ui'),str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

if __name__=='__main__':unittest.main()
