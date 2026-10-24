"""Keep the discovery integration test's UDP packets on loopback."""
from pathlib import Path
import sys,os,struct
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pax-tools/desktop'))
from host import Host,main
original=Host.dispatch
def dispatch(self,name,*args):
 if name=='net_udp_send':
  assert args[4]==18732
  args=(*args[:3],0x7f000001,int(os.environ['PAX_TEST_DISCOVERY_PORT']))
 if name=='net_udp_recv':
  result=original(self,name,*args)
  if result>0 and args[4]:self.put(args[4],struct.pack('<H',18732))
  return result
 if name=='input_mouse_read':
  for ptr in args:self.put(ptr,struct.pack('<i',0))
  return 0
 return original(self,name,*args)
Host.dispatch=dispatch
if __name__=='__main__':main()
