#pragma once
#include "pax_wifi_settings.h"
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace pax_wifi_backup {
// Fixed, versioned bytes; no compiler struct padding is persisted.
constexpr size_t SIZE = 110;
static inline uint32_t checksum(const unsigned char *data, size_t size) {
 uint32_t crc = 0xffffffffU;
 for(size_t i=0;i<size;i++){crc^=data[i];for(int bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320U & (0U-(crc&1)));}
 return ~crc;
}
static inline bool load(const char *path, pax_wifi_request_t &request) {
 unsigned char bytes[SIZE]{};FILE *file=std::fopen(path,"rb");if(!file)return false;
 bool ok=std::fread(bytes,1,SIZE,file)==SIZE && std::fgetc(file)==EOF && !std::ferror(file);
 if(std::fclose(file)!=0)ok=false;
 uint32_t crc=checksum(bytes,SIZE-4);
 ok=ok && std::memcmp(bytes,"PAXWIFI1",8)==0;
 for(unsigned i=0;i<4;i++)ok=ok && bytes[SIZE-4+i]==static_cast<unsigned char>(crc>>(8*i));
 pax_wifi_request_t candidate{};candidate.magic=PAX_WIFI_SETTINGS_MAGIC;candidate.replace_password=1;
 std::memcpy(candidate.ssid,bytes+8,33);std::memcpy(candidate.password,bytes+41,65);
 ok=ok && pax_wifi_request_valid(&candidate);
 if(ok)request=candidate;
 pax_wifi_wipe(&candidate,sizeof(candidate));pax_wifi_wipe(bytes,sizeof(bytes));return ok;
}
static inline bool save(const char *path, const pax_wifi_request_t &request) {
 if(request.replace_password!=1 || !pax_wifi_request_valid(&request))return false;
 char temporary[256],previous[256];
 if(std::snprintf(temporary,sizeof(temporary),"%s.tmp",path)>=static_cast<int>(sizeof(temporary)) ||
    std::snprintf(previous,sizeof(previous),"%s.prev",path)>=static_cast<int>(sizeof(previous)))return false;
 unsigned char bytes[SIZE]{};std::memcpy(bytes,"PAXWIFI1",8);
 std::memcpy(bytes+8,request.ssid,std::strlen(request.ssid));std::memcpy(bytes+41,request.password,std::strlen(request.password));
 uint32_t crc=checksum(bytes,SIZE-4);for(unsigned i=0;i<4;i++)bytes[SIZE-4+i]=crc>>(8*i);
 FILE *file=std::fopen(temporary,"wb");bool ok=false;
 if(file){ok=std::fwrite(bytes,1,SIZE,file)==SIZE;if(std::fflush(file)!=0)ok=false;if(ok && ::fsync(fileno(file))!=0)ok=false;if(std::fclose(file)!=0)ok=false;}
 // FatFs cannot rename over an existing file. Retain a recovery copy until
 // the new file is in place; startup also accepts .prev after a power loss.
 if(ok){
  bool moved=false;
  if(std::remove(previous)!=0 && errno!=ENOENT)ok=false;
  if(ok){moved=std::rename(path,previous)==0;if(!moved && errno!=ENOENT)ok=false;}
  if(ok && std::rename(temporary,path)!=0){ok=false;if(moved)std::rename(previous,path);}
  if(ok)std::remove(previous);
 }
 if(!ok)std::remove(temporary);
 pax_wifi_wipe(bytes,sizeof(bytes));return ok;
}
}
