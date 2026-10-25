"""Actual loader DNS recovery and idempotent DHCP configuration behavior."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'components/pax_loader/pax_loader.cpp').read_text()
def function(signature):
 a=SOURCE.index(signature);i=SOURCE.index('{',a)+1;n=1
 while n:
  if SOURCE[i]=='{':n+=1
  elif SOURCE[i]=='}':n-=1
  i+=1
 return SOURCE[a:i]
def run(code):
 with tempfile.TemporaryDirectory() as td:
  p=Path(td);(p/'test.cpp').write_text(code)
  subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'components/pax_loader'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
DNS_STUB=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#define CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_IP_IS_ANY(ip) ((ip).addr==0)
using esp_err_t=int;constexpr int ESP_OK=0;
struct esp_netif_t{};static esp_netif_t device;static bool missing;
struct IP {uint32_t addr;};struct esp_netif_ip_info_t {IP ip,netmask,gw;};
struct esp_netif_dns_info_t {IP ip;};
enum esp_netif_dns_type_t {ESP_NETIF_DNS_MAIN,ESP_NETIF_DNS_BACKUP,ESP_NETIF_DNS_FALLBACK};
static uint32_t global_dns[3],local_dns[3],live_ip=123;static int writes;
static esp_netif_t *esp_netif_get_default_netif(){return missing?nullptr:&device;}
static int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *out){assert(n==&device);out->ip.addr=live_ip;return 0;}
static int esp_netif_get_dns_info(esp_netif_t *n,esp_netif_dns_type_t slot,esp_netif_dns_info_t *out){out->ip.addr=(n?local_dns:global_dns)[slot];return 0;}
static int esp_netif_set_dns_info(esp_netif_t *n,esp_netif_dns_type_t slot,const esp_netif_dns_info_t *value){assert(!n);writes++;global_dns[slot]=value->ip.addr;return 0;}
'''
class NetworkDNS(unittest.TestCase):
 def test_recovers_only_empty_global_resolver_from_connected_interfaces_own_servers(self):
  run(DNS_STUB+function('static bool restore_active_dns_if_missing_()')+r'''
int main(){
 local_dns[0]=123;local_dns[1]=456;assert(restore_active_dns_if_missing_() && writes==2 && global_dns[0]==123 && global_dns[1]==456);
 assert(!restore_active_dns_if_missing_() && writes==2); /* never override working DNS */
 std::memset(global_dns,0,sizeof(global_dns));global_dns[2]=789;assert(!restore_active_dns_if_missing_() && writes==2); /* valid fallback is sufficient */
 std::memset(global_dns,0,sizeof(global_dns));live_ip=0;assert(!restore_active_dns_if_missing_() && writes==2);
 live_ip=123;missing=true;assert(!restore_active_dns_if_missing_() && writes==2);
 missing=false;std::memset(local_dns,0,sizeof(local_dns));assert(!restore_active_dns_if_missing_() && writes==2);
}
''')
 def test_reapply_dhcp_keeps_lease_dns_and_still_saves_preferences(self):
  # Compile the full production setter with deterministic network/preference APIs.
  code=r'''
#include <cassert>
#include <cstdint>
#include <arpa/inet.h>
#define PAX_APP_SIDE 1
#include "psram_app.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define IPADDR_TYPE_V4 0
using esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED=1,ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED=2;
struct esp_netif_t{};static esp_netif_t device;
struct IP {uint32_t addr;};struct esp_netif_ip_info_t {IP ip,netmask,gw;};
struct esp_netif_dns_info_t {struct {int type;union {IP ip4;} u_addr;} ip;};
constexpr int ESP_NETIF_DNS_MAIN=0;
enum esp_netif_dhcp_status_t {ESP_NETIF_DHCP_INIT,ESP_NETIF_DHCP_STARTED};
static esp_netif_dhcp_status_t status=ESP_NETIF_DHCP_STARTED;
static int stops,starts,ip_writes,dns_writes,saves,syncs;static bool save_ok=true;
static esp_netif_t *esp_netif_get_default_netif(){return &device;}
static int esp_netif_dhcpc_get_status(esp_netif_t *,esp_netif_dhcp_status_t *out){*out=status;return 0;}
static int esp_netif_dhcpc_stop(esp_netif_t *){stops++;status=ESP_NETIF_DHCP_INIT;return 0;}
static int esp_netif_dhcpc_start(esp_netif_t *){starts++;status=ESP_NETIF_DHCP_STARTED;return 0;}
static int esp_netif_set_ip_info(esp_netif_t *,const esp_netif_ip_info_t *){ip_writes++;return 0;}
static int esp_netif_set_dns_info(esp_netif_t *,int,const esp_netif_dns_info_t *){dns_writes++;return 0;}
static const char *esp_err_to_name(int){return "error";}
static bool restore_active_dns_if_missing_(){return false;}
struct SavedNetworkConfig {uint32_t magic,dhcp_enabled,ipv4,netmask,gateway,dns;};
constexpr int NETWORK_PREFERENCE_MAGIC=1,NETWORK_PREFERENCE_KEY=2;
namespace esphome {
struct Preference {bool save(const SavedNetworkConfig *value){saves++;assert(value->magic==1);return save_ok;}};
struct Preferences {template<class T> Preference make_preference(int,bool){return {};}bool sync(){syncs++;return true;}};
static Preferences prefs,*global_preferences=&prefs;
}
class PaxLoader {public:static PaxLoader *active_;static int svc_network_config_set(const pax_network_config_t *);};
static PaxLoader loader;PaxLoader *PaxLoader::active_=&loader;
'''+function('int PaxLoader::svc_network_config_set(')+r'''
int main(){
 pax_network_config_t config{};config.dhcp_enabled=1;
 assert(PaxLoader::svc_network_config_set(&config)==0 && stops==0 && starts==0 && ip_writes==0 && dns_writes==0 && saves==1 && syncs==1);
 assert(PaxLoader::svc_network_config_set(&config)==0 && stops==0 && starts==0 && saves==2); /* startup restore is idempotent too */
 save_ok=false;assert(PaxLoader::svc_network_config_set(&config)==-3 && stops==0 && starts==0);save_ok=true;
 config.dhcp_enabled=0;config.ipv4=0x0a000002;config.netmask=0xffffff00;config.dns=0x0a000001;
 assert(PaxLoader::svc_network_config_set(&config)==0 && stops==1 && starts==0 && ip_writes==1 && dns_writes==1);
 config.dhcp_enabled=1;assert(PaxLoader::svc_network_config_set(&config)==0 && stops==1 && starts==1 && ip_writes==2);
 config.dhcp_enabled=0;config.netmask=0xff00ff00;assert(PaxLoader::svc_network_config_set(&config)==-1 && stops==1 && ip_writes==2);
}
'''
  run(code)
