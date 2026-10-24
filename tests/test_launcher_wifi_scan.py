"""Scan selection stages an SSID without connecting or reusing another password."""
import unittest
from test_launcher_background_jobs import function,run,PREFIX

class WifiSelection(unittest.TestCase):
 def test_current_secured_new_secured_open_and_invalid_selection(self):
  code=PREFIX+r'''
#include "pax_wifi_settings.h"
#include "pax_wifi_scan.h"
static pax_wifi_scan_status_t g_wifi_scan;
static pax_wifi_status_t g_wifi_status;
static bool g_wifi_available=true,g_wifi_needs_password,g_wifi_password_changed;
static char g_wifi_ssid[33],g_wifi_password[65],g_settings_message[160];
static void secure_clear(void *ptr,size_t n){pax_wifi_wipe(ptr,n);}
'''+function('select_wifi_network')+r'''
int main(void){
 g_wifi_scan.magic=PAX_WIFI_SCAN_MAGIC;g_wifi_scan.state=2;
 pax_wifi_scan_add(&g_wifi_scan,"Current",-20,1,1);
 pax_wifi_scan_add(&g_wifi_scan,"Other",-40,6,1);
 pax_wifi_scan_add(&g_wifi_scan,"Open",-60,11,0);
 strcpy(g_wifi_status.ssid,"Current");strcpy(g_wifi_password,"staged password");
 assert(select_wifi_network(0) && !strcmp(g_wifi_ssid,"Current") && !g_wifi_needs_password && !strcmp(g_wifi_password,"staged password"));
 assert(select_wifi_network(1) && !strcmp(g_wifi_ssid,"Other") && g_wifi_needs_password && !g_wifi_password[0] && g_wifi_password_changed);
 strcpy(g_wifi_password,"some other password");
 assert(select_wifi_network(2) && !strcmp(g_wifi_ssid,"Open") && !g_wifi_needs_password && !g_wifi_password[0] && g_wifi_password_changed);
 assert(!select_wifi_network(-1) && !select_wifi_network(3));
 g_wifi_scan.state=1;assert(!select_wifi_network(0) && !strcmp(g_wifi_ssid,"Open"));
}
'''
  run(code)

 def test_scan_snapshot_is_validated_before_publishing(self):
  code=PREFIX+r'''
#include "pax_wifi_scan.h"
static pax_wifi_scan_status_t g_wifi_scan,next;
static bool g_wifi_scan_available;static size_t bytes;static int closes;
static void *open_file(const char *path,const char *mode){assert(!strcmp(path,PAX_WIFI_SCAN_FILE)&&!strcmp(mode,"rb"));return &next;}
static size_t read_file(void *out,size_t size,size_t count,void *file){assert(file==&next&&size==1&&count==sizeof(next));memcpy(out,&next,bytes);return bytes;}
static int close_file(void *file){assert(file==&next);closes++;return 0;}
'''+function('read_wifi_scan')+r'''
int main(void){
 services.file_open=open_file;services.file_read=read_file;services.file_close=close_file;
 next.magic=PAX_WIFI_SCAN_MAGIC;next.state=2;next.generation=1;bytes=sizeof(next);
 assert(read_wifi_scan() && g_wifi_scan_available && g_wifi_scan.state==2 && closes==1);
 assert(!read_wifi_scan() && closes==2);
 next.count=PAX_WIFI_SCAN_MAX+1;next.generation=2;
 assert(!read_wifi_scan() && g_wifi_scan.generation==1 && closes==3);
 next.count=0;bytes=4;assert(!read_wifi_scan() && g_wifi_scan.generation==1 && closes==4);
}
'''
  run(code)
if __name__=='__main__':unittest.main()
