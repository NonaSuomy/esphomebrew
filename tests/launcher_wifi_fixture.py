"""Bounded scan endpoint for real RV32 picker tests; no radio or credentials."""
from pathlib import Path
import io
import re
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pax-tools/desktop'))
from host import Host,main
original=Host.dispatch
started=None
navigate='--fixture-navigation' in sys.argv
if navigate:sys.argv.remove('--fixture-navigation')
nav_ready=None
source=(Path(__file__).resolve().parents[1]/'apps/launcher_ui/main.c').read_text()
order=[int(value) for value in re.search(r'static const uint8_t order\[SETTINGS_FOCUS_COUNT\] = \{([^}]+)\}',source).group(1).replace('\n','').split(',') if value.strip()]
scan_steps=(order.index(36)-order.index(18))%len(order)
nav_events=[(key,down) for key in [155]*scan_steps+[13] for down in (1,0)]+[None]+[(key,down) for key in (155,13) for down in (1,0)]
nav_next=0.0
nav_stop=False
class Request(io.BytesIO):
 def close(self):
  global started
  if not self.closed:
   assert self.getvalue()==struct.pack('<I',0x50575331)
   started=time.monotonic();print('Wi-Fi fixture: scan accepted',file=sys.stderr)
  super().close()
def dispatch(self,name,*args):
 global nav_ready,nav_next,nav_stop
 if navigate and name=='input_keyboard_read':
  if nav_stop:nav_stop=False;return 0
  now=time.monotonic()
  if nav_ready is None or now<nav_ready or now<nav_next or not nav_events:return 0
  if nav_events[0] is None:
   if started is None or now-started<1.5:return 0
   nav_events.pop(0)
  key,down=nav_events.pop(0);self.put(args[0],struct.pack('<ii',key,down))
  nav_stop=True;nav_next=now+.06;return 1
 if name=='input_mouse_read':
  for pointer in args:self.put(pointer,struct.pack('<i',0))
  return 0
 if name=='file_open':
  path=self.text(args[0]);mode=self.text(args[1])
  if path=='/dev/pax-wifi-scan':
   if mode=='wb':return self.handle(Request())
   state=0 if started is None else 1 if time.monotonic()-started<.5 else 2
   aps=[('Lab Current',-25,1,1),('Lab Other',-45,6,1),('Lab Open',-70,11,0)] if state==2 else []
   data=struct.pack('<IiII80s',0x50575331,state,len(aps),state,b'Choose a network, or enter manually')
   data+=b''.join(struct.pack('<33s3xiII',ssid.encode(),rssi,channel,secured) for ssid,rssi,channel,secured in aps)
   data+=bytes((24-len(aps))*48)
   return self.handle(io.BytesIO(data))
  if path=='/dev/pax-wifi':
   if nav_ready is None:nav_ready=time.monotonic()+1
   assert mode=='rb','Picker must not connect before Apply'
   return self.handle(io.BytesIO(struct.pack('<IiI33s3xII48s',0x50574631,0,1,b'Lab Current',1,2412,b'WPA2-PSK')))
 return original(self,name,*args)
Host.dispatch=dispatch
if __name__=='__main__':main()
