#include "pax_firmware_transfer.h"
#include <cassert>
#include <cstring>
#include <vector>
struct Source {
 int left=10000;bool fail=false;
 int read(uint8_t *out,size_t wanted){if(fail)return -1;int n=std::min(size_t(left),wanted);memset(out,42,n);left-=n;return n;}
};
struct Target {
 bool begin_ok=true,write_ok=true,end_ok=true,activate_ok=true;
 bool started=false,aborted=false,ended=false,activated=false;unsigned written=0;
 bool begin(uint32_t){started=true;return begin_ok;}
 bool write(uint8_t *,size_t n){written+=n;return write_ok;}
 void abort(){aborted=true;}
 bool end(){ended=true;return end_ok;}
 bool activate(){activated=true;return activate_ok;}
 void progress(uint32_t,uint32_t){}
};
struct Digest {
 bool begin_ok=true,update_ok=true,hash_ok=true;
 bool begin(){return begin_ok;}bool update(uint8_t *,size_t){return update_ok;}
 bool matches(const char *expected){return hash_ok && !strcmp(expected,"correct");}
};
int main(){
 for(int fault=0;fault<10;fault++){
  Source source;Target target;Digest digest;
  if(fault==1)target.begin_ok=false;
  if(fault==2)source.fail=true;
  if(fault==3)source.left=8000;
  if(fault==4)target.write_ok=false;
  if(fault==5)digest.hash_ok=false;
  if(fault==6)digest.begin_ok=false;
  if(fault==7)digest.update_ok=false;
  if(fault==8)target.end_ok=false;
  if(fault==9)target.activate_ok=false;
  bool ok=pax_firmware::transfer(source,target,digest,10000,"correct");
  assert(ok==(fault==0));
  assert(target.activated==(fault==0 || fault==9));
  if(fault>=2 && fault<=7){assert(target.aborted);assert(!target.ended);}
  if(fault==0){assert(target.written==10000);assert(!target.aborted);assert(target.ended);}
 }
 Source s;Target t;Digest d;
 assert(!pax_firmware::transfer(s,t,d,0,"correct"));assert(!t.started);
}
