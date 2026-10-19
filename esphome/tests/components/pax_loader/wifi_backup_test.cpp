#include "pax_wifi_backup.h"
#include <cassert>
#include <string>
#include <vector>
extern "C" int __real_rename(const char *,const char *);
extern "C" int __wrap_rename(const char *src,const char *dst){
 if(access(dst,F_OK)==0){errno=EEXIST;return -1;}return __real_rename(src,dst);
}
int main(int argc,char **argv){
 assert(argc==2);std::string path=std::string(argv[1])+"/wifi.bin";
 pax_wifi_request_t original{};original.magic=PAX_WIFI_SETTINGS_MAGIC;original.replace_password=1;
 strcpy(original.ssid,"Network with spaces");strcpy(original.password,"password123");
 pax_wifi_request_t loaded{};assert(!pax_wifi_backup::load(path.c_str(),loaded));
 assert(pax_wifi_backup::save(path.c_str(),original));assert(pax_wifi_backup::load(path.c_str(),loaded));
 assert(!strcmp(original.ssid,loaded.ssid)&&!strcmp(original.password,loaded.password));
 FILE *file=fopen(path.c_str(),"rb");std::vector<unsigned char> bytes(pax_wifi_backup::SIZE);assert(fread(bytes.data(),1,bytes.size(),file)==bytes.size());fclose(file);
 // Corruption anywhere, truncation, and trailing bytes must not supply credentials.
 for(size_t i=0;i<bytes.size();i++){
  auto damaged=bytes;damaged[i]^=1;file=fopen(path.c_str(),"wb");fwrite(damaged.data(),1,damaged.size(),file);fclose(file);
  strcpy(loaded.ssid,"untouched");assert(!pax_wifi_backup::load(path.c_str(),loaded));assert(!strcmp(loaded.ssid,"untouched"));
 }
 for(size_t size=0;size<bytes.size();size++){
  file=fopen(path.c_str(),"wb");fwrite(bytes.data(),1,size,file);fclose(file);assert(!pax_wifi_backup::load(path.c_str(),loaded));
 }
 file=fopen(path.c_str(),"wb");fwrite(bytes.data(),1,bytes.size(),file);fputc(0,file);fclose(file);assert(!pax_wifi_backup::load(path.c_str(),loaded));
 original.password[0]=0;assert(pax_wifi_backup::save(path.c_str(),original));assert(pax_wifi_backup::load(path.c_str(),loaded));assert(!loaded.password[0]);
 strcpy(original.password,"short");assert(!pax_wifi_backup::save(path.c_str(),original));assert(pax_wifi_backup::load(path.c_str(),loaded));assert(!loaded.password[0]);
 assert(!pax_wifi_backup::save((path+"/unwritable").c_str(),loaded));
 return 0;
}
