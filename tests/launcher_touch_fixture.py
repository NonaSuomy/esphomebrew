"""Run the desktop PAX host as a touch panel with no USB mouse attached.

The desktop host normally mirrors one pointer into touch and mouse services.
Disable that mirror here so a touch cannot also click at an old mouse position.
"""
from pathlib import Path
import struct
import os
import time
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pax-tools/desktop'))
from host import Host,main
original_dispatch=Host.dispatch

def dispatch(self,name,*args):
    if name in ('png_load_rgb565','png_load_rgb565_blended') and os.environ.get('PAX_FIXTURE_PNG'):
        # Deterministic large pixels exercise the real guest conversion and
        # cancellation path; codec correctness is checked separately.
        if name=='png_load_rgb565_blended' and os.environ.get('PAX_FIXTURE_CANCEL_IMAGE'):
            at=time.monotonic()-self.start
            self.events.appendleft(dict(at=at+.1,type='touch',x=40,y=30,down=False))
            self.events.appendleft(dict(at=at,type='touch',x=40,y=30,down=True))
        width=height=2048 if os.environ.get("PAX_FIXTURE_CANCEL_IMAGE") else 512
        pixels=self.mem.alloc(width*height*2)
        self.put(pixels,b'\x00\xf8'*(width*height))
        self.put(args[1],struct.pack('<H',width));self.put(args[2],struct.pack('<H',height))
        return pixels
    if name=='input_mouse_read':
        for pointer in args:self.put(pointer,struct.pack('<i',0))
        return 0
    return original_dispatch(self,name,*args)

Host.dispatch=dispatch
if __name__=='__main__':main()
