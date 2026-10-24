#pragma once
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <unistd.h>
namespace pax_supply {
static constexpr uint16_t DISCOVERY_PORT=18732;
static inline int64_t now_us(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return static_cast<int64_t>(t.tv_sec)*1000000+t.tv_nsec/1000;}
static inline bool safe_http(const std::string &url){
 size_t prefix=url.rfind("http://",0)==0 ? 7 : url.rfind("https://",0)==0 ? 8 : 0;
 if(!prefix || url.size()<=prefix || url.size()>240 || url[prefix]=='/')return false;
 for(unsigned char c:url)if(c<=32 || c>=127 || c=='@' || c=='?' || c=='#' || c=='\\')return false;
 return true;
}
static inline std::string read_setting(const std::string &path){
 FILE *file=std::fopen(path.c_str(),"rb");if(!file)return {};
 char text[256]={};size_t n=std::fread(text,1,sizeof(text)-1,file);
 bool oversized=std::fgetc(file)!=EOF;std::fclose(file);if(oversized)return {};
 while(n && (text[n-1]=='\r' || text[n-1]=='\n' || text[n-1]==' '))n--;
 return std::string(text,n);
}
static inline std::string decode(const unsigned char *data,int length,uint32_t nonce,uint32_t ip){
 if(length!=14 || std::memcmp(data,"PAXHERE1",8) || !ip || (ip>>28)>=14)return {};
 uint32_t echoed=static_cast<uint32_t>(data[8])|(static_cast<uint32_t>(data[9])<<8)|
   (static_cast<uint32_t>(data[10])<<16)|(static_cast<uint32_t>(data[11])<<24);
 unsigned port=static_cast<unsigned>(data[12])|(static_cast<unsigned>(data[13])<<8);
 if(echoed!=nonce || !port)return {};
 char url[64];std::snprintf(url,sizeof(url),"http://%u.%u.%u.%u:%u/",static_cast<unsigned>(ip>>24),
  static_cast<unsigned>((ip>>16)&255),static_cast<unsigned>((ip>>8)&255),static_cast<unsigned>(ip&255),port);
 return url;
}
/* Called only by the background HTTP bootstrap task, never the UI loop.
 * Parameters allow loopback integration tests without occupying the LAN port. */
static inline std::string discover(uint16_t port=DISCOVERY_PORT,uint32_t address=0xffffffffU){
 int socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(socket<0)return {};
 int yes=1;::setsockopt(socket,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes));
 sockaddr_in bind_address{};bind_address.sin_family=AF_INET;
 if(::bind(socket,reinterpret_cast<sockaddr *>(&bind_address),sizeof(bind_address))!=0){::close(socket);return {};}
 sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(port);target.sin_addr.s_addr=htonl(address);
 int64_t started=now_us(),last_send=started-400000;
 uint32_t nonce=static_cast<uint32_t>(started)^static_cast<uint32_t>(socket);
 unsigned char query[12]={'P','A','X','F','I','N','D','1'};
 for(unsigned i=0;i<4;i++)query[8+i]=static_cast<unsigned char>(nonce>>(i*8));
 std::string result;
 while(now_us()-started<1200000){
  int64_t now=now_us();
  if(now-last_send>=400000){::sendto(socket,query,sizeof(query),0,reinterpret_cast<sockaddr *>(&target),sizeof(target));last_send=now;}
  fd_set read;FD_ZERO(&read);FD_SET(socket,&read);timeval timeout{0,100000};
  if(::select(socket+1,&read,nullptr,nullptr,&timeout)<=0)continue;
  unsigned char packet[64];sockaddr_in peer{};socklen_t size=sizeof(peer);
  int n=::recvfrom(socket,packet,sizeof(packet),0,reinterpret_cast<sockaddr *>(&peer),&size);
  if(ntohs(peer.sin_port)!=port)continue;
  result=decode(packet,n,nonce,ntohl(peer.sin_addr.s_addr));if(!result.empty())break;
 }
 ::close(socket);return result;
}
static inline std::string launcher_url(const std::string &base){
 if(!safe_http(base))return {};
 return base+(base.back()=='/' ? "" : "/")+"launcher_ui.pax";
}
static inline std::string resolve(const std::string &fallback,const std::string &mode,const std::string &cached,
                           uint16_t port=DISCOVERY_PORT,uint32_t address=0xffffffffU){
 if(mode!="manual"){
  auto found=discover(port,address);if(!found.empty())return launcher_url(found);
 }
 auto saved=launcher_url(cached);if(!saved.empty())return saved;
 return safe_http(fallback) ? fallback : std::string{};
}
}
