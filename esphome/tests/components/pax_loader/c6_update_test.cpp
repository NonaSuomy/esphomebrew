#include "pax_c6_image.h"
#include "pax_c6_update.h"
#include <cassert>
#include <vector>
using esphome::pax_loader::data::Sha256;
static std::vector<uint8_t> image() {
  std::vector<uint8_t> b(24+8+256,0);
  b[0]=0xe9;b[1]=1;b[12]=13;b[23]=1;b[28]=0;b[29]=1;
  b[32]=0x32;b[33]=0x54;b[34]=0xcd;b[35]=0xab;
  memcpy(b.data()+48,"2.12.3",7);memcpy(b.data()+80,"network_adapter",16);
  uint8_t checksum=0xef;for(size_t i=32;i<b.size();i++)checksum^=b[i];
  b.resize((b.size()+16)&~size_t(15),0);b.back()=checksum;
  Sha256 hash;hash.update(b.data(),b.size());std::string hex=hash.hex();
  for(size_t i=0;i<64;i+=2)b.push_back(static_cast<uint8_t>(strtoul(hex.substr(i,2).c_str(),nullptr,16)));
  return b;
}
static FILE *file(const std::vector<uint8_t> &bytes) {FILE *f=tmpfile();assert(f);assert(fwrite(bytes.data(),1,bytes.size(),f)==bytes.size());rewind(f);return f;}
static bool valid(const std::vector<uint8_t> &bytes) {FILE *f=file(bytes);uint32_t size=0;char version[32];bool ok=pax_c6::validate_image(f,size,version,sizeof(version));fclose(f);return ok;}
struct Fake {
  int fail,step=0,begins=0,ends=0,activations=0;std::vector<uint8_t> bytes;
  int next(){return ++step==fail ? -9 : 0;}
  int begin(){begins++;return next();}
  int write(uint8_t *b,size_t n){assert(n>0 && n<=1500);int e=next();if(!e)bytes.insert(bytes.end(),b,b+n);return e;}
  int end(){ends++;return next();}
  int activate(){activations++;return next();}
  void progress(uint32_t,uint32_t){}
};
static void rehash(std::vector<uint8_t> &b) {
 Sha256 hash;hash.update(b.data(),b.size()-32);std::string hex=hash.hex();
 for(size_t i=0;i<32;i++)b[b.size()-32+i]=static_cast<uint8_t>(strtoul(hex.substr(i*2,2).c_str(),nullptr,16));
}
int main(){
 static_assert(sizeof(pax_c6_request_t)==520);static_assert(sizeof(pax_c6_status_t)==176);
 auto b=image();assert(valid(b));
 Sha256 full_hash;full_hash.update(b.data(),b.size());std::string approved=full_hash.hex();
 FILE *approved_file=file(b);assert(pax_c6::matches_approved_image(approved_file,approved.c_str()));
 assert(ftell(approved_file)==0);assert(!pax_c6::matches_approved_image(approved_file,""));fclose(approved_file);
 // A different, internally valid ESP image still must not match this board.
 auto other_transport=b;other_transport[84]='z';other_transport[303]^=b[84]^other_transport[84];rehash(other_transport);
 FILE *wrong_transport=file(other_transport);assert(!pax_c6::matches_approved_image(wrong_transport,approved.c_str()));fclose(wrong_transport);

 FILE *progress_file=file(b);uint32_t image_size=0,last_done=0,calls=0;char version[32];
 assert(pax_c6::validate_image(progress_file,image_size,version,sizeof(version),[&](uint32_t done,uint32_t total){assert(total==b.size() && done>=last_done && done<=total);last_done=done;calls++;}));
 assert(calls>=2 && last_done>0);fclose(progress_file);
 for(size_t i=0;i<b.size();i++){auto damaged=b;damaged[i]^=1;assert(!valid(damaged));}
 for(size_t n=0;n<b.size();n++){auto truncated=b;truncated.resize(n);assert(!valid(truncated));}
 auto wrong_chip=b;wrong_chip[12]=18;rehash(wrong_chip);assert(!valid(wrong_chip));
 auto wrong_project=b;wrong_project[80]='x';wrong_project[303]^=b[80]^wrong_project[80];rehash(wrong_project);assert(!valid(wrong_project));
 auto overflow=b;memset(overflow.data()+28,255,4);rehash(overflow);assert(!valid(overflow));
 auto extra=b;extra.push_back(0);assert(!valid(extra));
 std::vector<uint8_t> data(4501,42);
 for(int fail=0;fail<=7;fail++){
  FILE *f=file(data);Fake t{fail};uint32_t sent=0;int e=pax_c6::transfer_image(f,data.size(),t,sent);fclose(f);
  assert((e==0)==(fail==0));
  if(!fail){assert(t.bytes==data && sent==data.size() && t.activations==1 && t.ends==1);}
  else if(fail==1){assert(t.ends==0 && t.activations==0);}
  else if(fail<7){assert(t.ends==1 && t.activations==0);}
 }
 FILE *f=file({1,2});Fake t{0};uint32_t sent;assert(pax_c6::transfer_image(f,3000,t,sent)!=0);assert(t.ends==1 && !t.activations);fclose(f);
 puts("C6 validation/corruption/truncation and OTA fault tests passed");
}
