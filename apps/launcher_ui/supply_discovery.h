#pragma once
/* ABI-1 UDP discovery; all waits run on the cancellable catalog I/O worker. */
#define PAX_SUPPLY_PORT 18732
#define PAX_SUPPLY_AUTO_FILE "/sd/roms/pax/.launcher_ui_network_mode.txt"
typedef struct {char url[256];} PaxSupplyResult;
static bool pax_supply_reply(const unsigned char *packet,int length,uint32_t nonce,
                             uint32_t ip,char *url,size_t capacity)
{
  if(length!=14 || memcmp(packet,"PAXHERE1",8) || !ip || ip==0xffffffffU || (ip>>28)>=14)return false;
  uint32_t echoed=(uint32_t)packet[8]|((uint32_t)packet[9]<<8)|((uint32_t)packet[10]<<16)|((uint32_t)packet[11]<<24);
  unsigned port=(unsigned)packet[12]|((unsigned)packet[13]<<8);
  if(echoed!=nonce || !port)return false;
  int n=lv_snprintf(url,capacity,"http://%u.%u.%u.%u:%u/",(unsigned)(ip>>24),
    (unsigned)((ip>>16)&255),(unsigned)((ip>>8)&255),(unsigned)(ip&255),port);
  return n>0 && (size_t)n<capacity;
}
static int pax_supply_discovery_job(void *argument)
{
  PaxSupplyResult *result=argument;
  if(!g_svc->net_udp_open || !g_svc->net_udp_send || !g_svc->net_udp_recv || !g_svc->net_udp_close)return -1;
  int socket=g_svc->net_udp_open(0,1);if(socket<0)return -1;
  static uint32_t serial;uint32_t nonce=(uint32_t)g_svc->get_time_us() ^ ++serial;
  unsigned char query[12]={'P','A','X','F','I','N','D','1'};
  for(unsigned i=0;i<4;i++)query[8+i]=(unsigned char)(nonce>>(i*8));
  uint32_t ip=0,mask=0,broadcast=0xffffffffU;
  if(g_svc->net_ipv4 && g_svc->net_ipv4(&ip,&mask)>0 && ip && mask)broadcast=ip|~mask;
  uint32_t started=(uint32_t)(g_svc->get_time_us()/1000U),last_send=0;int answer=-1;
  for(unsigned loops=0;loops<120;loops++){
    if(__atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE))break;
    uint32_t elapsed=(uint32_t)((uint32_t)(g_svc->get_time_us()/1000U)-started);
    if(elapsed>=1200U)break;
    if(loops==0 || elapsed-last_send>=400U){
      (void)g_svc->net_udp_send(socket,query,sizeof(query),broadcast,PAX_SUPPLY_PORT);
      if(broadcast!=0xffffffffU)(void)g_svc->net_udp_send(socket,query,sizeof(query),0xffffffffU,PAX_SUPPLY_PORT);
      last_send=elapsed;
    }
    unsigned char packet[64];uint32_t peer=0;uint16_t port=0;
    int length=g_svc->net_udp_recv(socket,packet,sizeof(packet),&peer,&port);
    if(port==PAX_SUPPLY_PORT && pax_supply_reply(packet,length,nonce,peer,result->url,sizeof(result->url))){answer=0;break;}
    g_svc->delay_ms(10);
  }
  g_svc->net_udp_close(socket);return answer;
}
