"""Bounded discovery validates replies and closes its socket on every outcome."""
import unittest
from test_launcher_background_jobs import PREFIX,run
HEADER=r'''
#include <stdio.h>
#define lv_snprintf snprintf
#include "supply_discovery.h"
'''
class SupplyDiscovery(unittest.TestCase):
 def test_nonce_length_port_and_sender_validation(self):
  run(PREFIX+HEADER+r'''
int main(void){
 unsigned char p[14]={'P','A','X','H','E','R','E','1',0x78,0x56,0x34,0x12,0x40,0x1f};char out[256];
 assert(pax_supply_reply(p,14,0x12345678,0xc0a80134,out,sizeof(out)) && !strcmp(out,"http://192.168.1.52:8000/"));
 assert(!pax_supply_reply(p,13,0x12345678,0xc0a80134,out,sizeof(out)));
 assert(!pax_supply_reply(p,14,0x12345679,0xc0a80134,out,sizeof(out)));
 assert(!pax_supply_reply(p,14,0x12345678,0,out,sizeof(out)));
 assert(!pax_supply_reply(p,14,0x12345678,0xe0000001,out,sizeof(out)));
 assert(!pax_supply_reply(p,14,0x12345678,0xc0a80134,out,5));
 p[0]='X';assert(!pax_supply_reply(p,14,0x12345678,0xc0a80134,out,sizeof(out)));p[0]='P';
 p[12]=p[13]=0;assert(!pax_supply_reply(p,14,0x12345678,0xc0a80134,out,sizeof(out)));
}
''')
 def test_timeout_cancel_missing_api_and_success(self):
  run(PREFIX+r'''
#include <stdio.h>
#define lv_snprintf snprintf
static int64_t now=123456000;static int sends,closes,reads,mode,opens;static unsigned char request[12];
static int64_t clock_us(void){return now;}static void delay(int ms){now+=(int64_t)ms*1000;}
static int open_udp(uint16_t port,int broadcast){assert(!port && broadcast);opens++;return mode==3 ? -1 : 7;}
static int send_udp(int fd,const void *packet,int length,uint32_t ip,uint16_t port){assert(fd==7 && length==12 && port==18732 && (ip==0xc0a801ff || ip==0xffffffff));memcpy(request,packet,12);sends++;return length;}
static int recv_udp(int fd,void *packet,int capacity,uint32_t *ip,uint16_t *port){assert(fd==7 && capacity>=14);reads++;if(mode!=1 || reads<2)return 0;unsigned char *out=packet;memcpy(out,"PAXHERE1",8);memcpy(out+8,request+8,4);out[12]=0x40;out[13]=0x1f;*ip=0xc0a80134;*port=18732;return 14;}
static void close_udp(int fd){assert(fd==7);closes++;}
static int address(uint32_t *ip,uint32_t *mask){*ip=0xc0a80163;*mask=0xffffff00;return 1;}
#include "supply_discovery.h"
int main(void){
 PaxSupplyResult result={0};services.get_time_us=clock_us;services.delay_ms=delay;
 assert(pax_supply_discovery_job(&result)==-1 && !opens);
 services.net_udp_open=open_udp;services.net_udp_send=send_udp;services.net_udp_recv=recv_udp;services.net_udp_close=close_udp;services.net_ipv4=address;
 int64_t start=now;assert(pax_supply_discovery_job(&result)==-1 && now-start<=1200000 && closes==1 && sends==6);
 mode=1;reads=0;assert(pax_supply_discovery_job(&result)==0 && closes==2 && !strcmp(result.url,"http://192.168.1.52:8000/"));
 g_io_cancel=true;assert(pax_supply_discovery_job(&result)==-1 && closes==3);g_io_cancel=false;
 mode=3;assert(pax_supply_discovery_job(&result)==-1 && closes==3);
}
''')
if __name__=='__main__':unittest.main()
