"""Navigate the real launcher's Tooltips control without a physical board."""
from pathlib import Path
import re,struct,sys,time
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pax-tools/desktop'))
from host import Host,main
source=(Path(__file__).resolve().parents[1]/'apps/launcher_ui/main.c').read_text()
order=[int(x) for x in re.search(r'static const uint8_t order\[SETTINGS_FOCUS_COUNT\] = \{([^}]+)\}',source).group(1).replace('\n','').split(',') if x.strip()]
keys=[155]*order.index(37)+[13]
events=[(key,down) for key in keys for down in (1,0)]
original=Host.dispatch
ready=None;next_key=0;stop=False
def dispatch(self,name,*args):
 global ready,next_key,stop
 if name=='file_open' and self.text(args[0])=='/dev/pax-wifi' and ready is None:ready=time.monotonic()+.8
 if name=='input_mouse_read':
  for address in args:self.put(address,struct.pack('<i',0))
  return 0
 if name=='input_keyboard_read':
  if stop:stop=False;return 0
  now=time.monotonic()
  if ready is None or now<ready or now<next_key or not events:return 0
  key,down=events.pop(0);self.put(args[0],struct.pack('<ii',key,down));stop=True;next_key=now+.07;return 1
 return original(self,name,*args)
Host.dispatch=dispatch
if __name__=='__main__':main()
