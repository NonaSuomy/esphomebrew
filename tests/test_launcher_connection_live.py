"""DHCP lease updates stay live without overwriting staged network edits."""
import unittest
from test_launcher_background_jobs import PREFIX,function,run

class LiveConnection(unittest.TestCase):
 def test_new_lease_and_mode_refresh_without_overwriting_drafts(self):
  run(PREFIX+r'''
static pax_network_config_t g_network_live,g_network_config,next;
static bool g_network_config_dirty;
static int g_network_edit_field=-1,error;
static int get_config(pax_network_config_t *out){*out=next;return error;}
'''+function('refresh_network_live')+r'''
int main(void){
 services.network_config_get=get_config;
 next.dhcp_enabled=1;next.connected=1;next.ipv4=0xc0a80128;next.netmask=0xffffff00;next.gateway=0xc0a80101;next.dns=0xc0a80101;
 refresh_network_live(true);assert(!memcmp(&g_network_config,&next,sizeof(next)) && !g_network_config_dirty);
 next.ipv4++;refresh_network_live(false);assert(g_network_live.ipv4==next.ipv4 && g_network_config.ipv4==next.ipv4);
 g_network_edit_field=0;next.ipv4++;refresh_network_live(false);assert(g_network_live.ipv4==next.ipv4 && g_network_config.ipv4==next.ipv4-1);
 g_network_edit_field=-1;g_network_config_dirty=true;g_network_config.ipv4=123;g_network_config.dhcp_enabled=0;
 next.ipv4++;refresh_network_live(false);assert(g_network_live.dhcp_enabled && g_network_config.ipv4==123 && !g_network_config.dhcp_enabled);
 g_network_config_dirty=false;refresh_network_live(false);assert(g_network_config.dhcp_enabled && g_network_config.ipv4==next.ipv4);
 error=-1;next.ipv4=0;refresh_network_live(false);assert(g_network_live.ipv4 && g_network_config.ipv4);
 error=0;next.connected=0;refresh_network_live(false);assert(!g_network_live.connected && !g_network_config.ipv4);
}
''')
 def test_idle_settings_polls_once_per_second_without_storage_scan(self):
  run(PREFIX+r'''
#include "pax_wifi_settings.h"
static pax_network_config_t g_network_live,g_network_config;
static pax_wifi_status_t g_wifi_status;static bool g_wifi_available;
static bool g_settings_mode;static uint32_t g_connection_poll_ms;
static uint64_t now;static int network_reads,wifi_reads,c6_reads,redraws;
static int64_t clock_us(void){return now*1000;}
static void refresh_network_live(bool load){assert(!load);network_reads++;g_network_live.ipv4=network_reads==3 ? 2 : 1;}
static void refresh_wifi_settings(bool load){assert(!load);wifi_reads++;}
static void refresh_c6_status(void){c6_reads++;}
static int update_selection_on_host(void *p){(void)p;redraws++;return 0;}
static int ui(int (*fn)(void *),void *p,uint32_t timeout){assert(timeout==1000);return fn(p);}
'''+function('poll_settings_connection')+r'''
int main(void){
 services.get_time_us=clock_us;services.ui_call_sync=ui;
 now=1000;poll_settings_connection();assert(!network_reads);
 g_settings_mode=true;poll_settings_connection();assert(network_reads==1 && wifi_reads==1 && c6_reads==1 && redraws==1);
 now=1999;poll_settings_connection();assert(network_reads==1);
 now=2000;poll_settings_connection();assert(network_reads==2 && redraws==1);
 g_discard_io=true;now=3000;poll_settings_connection();assert(network_reads==3 && redraws==1);
}
''')
if __name__=='__main__':unittest.main()
