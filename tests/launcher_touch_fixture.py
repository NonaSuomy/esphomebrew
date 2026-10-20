"""Run the desktop PAX host as a touch panel with no USB mouse attached.

The desktop host normally mirrors one pointer into touch and mouse services.
Disable that mirror here so a touch cannot also click at an old mouse position.
"""
from pathlib import Path
import struct
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pax-tools/desktop'))
from host import Host,main
original_dispatch=Host.dispatch

def dispatch(self,name,*args):
    if name=='input_mouse_read':
        for pointer in args:self.put(pointer,struct.pack('<i',0))
        return 0
    return original_dispatch(self,name,*args)

Host.dispatch=dispatch
if __name__=='__main__':main()
