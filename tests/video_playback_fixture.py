"""Run the actual RV32 video PAX with an 800x480 panel and bounded app heap.

Uses the normal desktop host; this checks functionality, not P4 frame rate.
Set PAX_VIDEO_HEAP_MB (default 20) to model the heap remaining after firmware,
scanout and the loaded video app. Output includes peak allocated guest bytes.
"""
from pathlib import Path
import os,struct,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pax-tools/desktop'))
from host import Host,GuestMemory,main
from unicorn import UC_HOOK_MEM_INVALID
from unicorn import riscv_const as R
initialize=GuestMemory.__init__
def memory_init(self,uc):
 initialize(self,uc)
 def invalid(u,access,address,size,value,user):
  regs={name:hex(u.reg_read(getattr(R,"UC_RISCV_REG_"+name))) for name in ["PC","RA","SP","A0","A1","A2","A3","A4","A5"]}
  print("VIDEO_INVALID_MEMORY",hex(address),size,regs,file=sys.stderr)
  return False
 uc.hook_add(UC_HOOK_MEM_INVALID,invalid)
GuestMemory.__init__=memory_init
allocation=GuestMemory.alloc;dispatch=Host.dispatch;report=Host.report
peak=failures=0
budget=int(os.environ.get('PAX_VIDEO_HEAP_MB','20'))*1024*1024

def alloc(self,size):
 global peak,failures
 if sum(self.blocks.values())+size>budget:failures+=1;return 0
 pointer=allocation(self,size);used=sum(self.blocks.values())
 if used>budget:self.free(pointer);failures+=1;return 0
 peak=max(peak,used);return pointer

def call(self,name,*args):
 if name=='display_get_size':
  self.put(args[0],struct.pack('<i',800));self.put(args[1],struct.pack('<i',480));return
 return dispatch(self,name,*args)

def result(self,mode):
 print(f'VIDEO_GUEST_HEAP peak={peak} budget={budget} failed_allocations={failures}',file=sys.stderr)
 return report(self,mode)
GuestMemory.alloc=alloc;Host.dispatch=call;Host.report=result
if __name__=='__main__':main()
