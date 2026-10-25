#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "pax_asset_pack.h"
static unsigned le16(const unsigned char *p) {return unsigned(p[0])|(unsigned(p[1])<<8);}
static unsigned le32(const unsigned char *p) {return le16(p)|(le16(p+2)<<16);}
int main(int argc,char **argv) {
 assert(argc==2); FILE *file=fopen(argv[1],"rb");assert(file);
 assert(!fseek(file,0,SEEK_END));const auto length=ftell(file);assert(length>12);rewind(file);
 std::vector<unsigned char> bytes(length);assert(fread(bytes.data(),1,length,file)==size_t(length));fclose(file);
 const unsigned count=le32(bytes.data()+8);size_t offset=12;
 for(unsigned i=0;i<count;++i) {
  const unsigned namesize=le16(bytes.data()+offset),size=le32(bytes.data()+offset+2);offset+=6;
  char name[256]{};assert(namesize<sizeof(name));memcpy(name,bytes.data()+offset,namesize);offset+=namesize;
  const auto *payload=bytes.data()+offset;offset+=size;
  file=fopen(argv[1],"rb");assert(file);PaxIndexedAsset asset;
  assert(pax_read_asset_pack(file,name,malloc,free,asset));fclose(file);
  assert(asset.width==le16(payload) && asset.height==le16(payload+2) && asset.colors==le16(payload+4));
  assert(!memcmp(asset.palette,payload+6,asset.colors*4));
  assert(!memcmp(asset.pixels,payload+6+asset.colors*4,size-6-asset.colors*4));free(asset.pixels);
 }
 printf("Verified all %u external sprite payloads and palettes match the original pack\n",count);
}
