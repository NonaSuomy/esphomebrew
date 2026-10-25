#include <cstdio>
#include <cstdlib>
#include <vector>
#include <stdexcept>
#include <cstdarg>
#include "Arduino.h"
#include "bank.h"
#include "resource.h"
#include "pax_data.h"
PaxSerial Serial;
void debug(unsigned short, const char *, ...) {}
void error(const char *message, ...) {throw std::runtime_error(message);}
static unsigned be16(const unsigned char *p) {return (unsigned(p[0])<<8)|p[1];}
static unsigned be32(const unsigned char *p) {return (be16(p)<<16)|be16(p+2);}
int main() {
 if(!pax_aw_load_memlist()) return 2;
 unsigned count=0, compressed=0;
 for(unsigned pos=0;pos<gb_memlist_bin_size;pos+=20) {
  const auto *p=gb_memlist_bin+pos; if(p[0]==255)break;
  MemEntry entry{};entry.bankId=p[7];entry.bankOffset=be32(p+8);entry.packedSize=be16(p+14);entry.size=be16(p+18);
  if(!entry.size)continue;
  std::vector<unsigned char> bytes(entry.size);Bank bank(".");
  if(!bank.readFlash(&entry,bytes.data())){fprintf(stderr,"failed resource %u\n",pos/20);return 3;}
  ++count; if(entry.packedSize!=entry.size)++compressed;
 }
 printf("Loaded %u SD resources; %u compressed-resource CRC checks passed\n",count,compressed);
}
