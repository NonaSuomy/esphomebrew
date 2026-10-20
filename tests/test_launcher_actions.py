"""Mutations retain results and transfer polling never leaks into other details."""
import unittest
from test_launcher_background_jobs import function,run,PREFIX

class Actions(unittest.TestCase):
 def test_requests_run_on_worker_and_failures_keep_navigation(self):
  code=PREFIX+r'''
#include "pax_wifi_settings.h"
static bool g_action_busy,busy,worker;static int g_open_index,calls;static char g_settings_message[160];
static bool mutations_busy(void){return g_action_busy || busy;}
static int update_selection_on_host(void *arg){(void)arg;return 0;}
static int ui(int (*fn)(void *),void *arg,uint32_t timeout){(void)timeout;assert(!worker);return fn(arg);}
static int log_message(const char *format,...){(void)format;return 0;}
static int io_run_committed(int (*fn)(void *),void *arg){
 assert(g_action_busy && g_open_index==-1);worker=true;int result=fn(arg);worker=false;
 g_open_index=-3;g_discard_io=true;return result;
}
static int mount(int32_t storage,int32_t mounted){assert(worker && storage==1 && !mounted);calls++;return -4;}
static int network(const pax_network_config_t *request){assert(worker && request->ipv4==1234);calls++;return -3;}
static int install(const char *url,const char *name,const char *title,const char *version,uint32_t size,const char *sha){
 assert(worker && !strcmp(url,"http://example/app.json") && !strcmp(name,"probe") && !strcmp(title,"Probe") && !strcmp(version,"1.0.0") && size==42 && !strcmp(sha,"hash"));calls++;return 0;
}
static int stream;static int close_result;static size_t written;
static void *open_file(const char *path,const char *mode){assert(worker && !strcmp(path,PAX_WIFI_SETTINGS_FILE) && !strcmp(mode,"wb"));return &stream;}
static size_t write_file(const void *data,size_t size,size_t count,void *file){assert(worker && file==&stream && size==1 && ((const pax_wifi_request_t *)data)->magic==PAX_WIFI_SETTINGS_MAGIC);written=count;return count;}
static int close_file(void *file){assert(worker && file==&stream);calls++;return close_result;}
'''+ '\n'.join(function(n) for n in ('mount_job','network_apply_job','wifi_apply_job'))+r'''
typedef struct {char url[256],name[65],title[97],version[33],sha256[65];uint32_t size;} InstallJob;
'''+function('install_start_job')+'\n'+function('run_action')+r'''
int main(void){
 services.storage_set_mounted=mount;services.network_config_set=network;services.store_install_start=install;services.ui_call_sync=ui;services.log_printf=log_message;
 services.file_open=open_file;services.file_write=write_file;services.file_close=close_file;
 struct {int storage,mounted;} mount_request={1,0};
 assert(run_action(mount_job,&mount_request,"Ejecting...")==-4 && calls==1 && g_discard_io && g_open_index==-3 && !g_action_busy);
 g_discard_io=false;pax_network_config_t request={.ipv4=1234};assert(run_action(network_apply_job,&request,"Applying...")==-3 && calls==2);
 g_discard_io=false;InstallJob job={"http://example/app.json","probe","Probe","1.0.0","hash",42};assert(run_action(install_start_job,&job,"Starting...")==0 && calls==3);
 g_discard_io=false;pax_wifi_request_t wifi={.magic=PAX_WIFI_SETTINGS_MAGIC};assert(run_action(wifi_apply_job,&wifi,"Wi-Fi...")==0 && written==sizeof(wifi) && calls==4);
 g_discard_io=false;close_result=-1;assert(run_action(wifi_apply_job,&wifi,"Wi-Fi...")==-2 && calls==5);
 busy=true;assert(run_action(mount_job,&mount_request,"Ejecting...")==-4 && calls==5);
}
'''
  # Worker request has a deliberately trivial two-integer payload.
  code=code.replace('static int mount_job(void *argument)','typedef struct {int storage,mounted;} MountJob;\nstatic int mount_job(void *argument)')
  run(code)

 def test_transfer_progress_survives_navigation_without_overwriting_other_app(self):
  code=PREFIX+r'''
static bool g_transfer_busy,g_transfer_verify_pending,g_catalog_stale,g_settings_scan_pending,g_detail_is_remote;
static int g_detail_index;static uint32_t g_install_state,g_install_done,g_install_total;
static char g_transfer_name[65]="probe",g_transfer_message[160],g_detail_name[65]="other",g_install_status[96]="other app";
static pax_install_progress_t next;
static int status(pax_install_progress_t *out){*out=next;return 0;}
'''+function('poll_install_status')+r'''
int main(void){
 services.store_install_status=status;next.state=1;next.done=50;next.total=100;strcpy(next.status,"Downloading 50%");
 poll_install_status();assert(g_transfer_busy && !strcmp(g_transfer_message,"Downloading 50%") && !strcmp(g_install_status,"other app"));
 g_detail_is_remote=true;strcpy(g_detail_name,"probe");poll_install_status();assert(g_install_state==1 && g_install_done==50 && g_install_total==100);
 next.state=2;strcpy(next.status,"Installed");poll_install_status();assert(!g_transfer_busy && g_transfer_verify_pending && g_catalog_stale && g_settings_scan_pending && g_install_state==2);
 g_transfer_verify_pending=false;g_detail_index=-1;next.state=3;poll_install_status();assert(!g_transfer_verify_pending && g_install_state==2);
}
'''
  run(code)

 def test_controller_c6_request_cannot_bypass_transfer_guard(self):
  code=PREFIX+r'''
#include "pax_c6_update.h"
static char g_settings_message[160],g_c6_source[sizeof(((pax_c6_request_t *)0)->source)],g_c6_checked_source[sizeof(g_c6_source)];
static bool busy=true;static int opens,refreshes;
static bool mutations_busy(void){return busy;}
static void refresh_c6_status(void){refreshes++;}
static void *open_file(const char *path,const char *mode){(void)path;(void)mode;opens++;return NULL;}
'''+function('request_c6_update')+r'''
int main(void){services.file_open=open_file;request_c6_update(PAX_C6_INSTALL);assert(!opens && !refreshes && strstr(g_settings_message,"active transfer"));busy=false;request_c6_update(PAX_C6_PREPARE);assert(opens==1 && refreshes==1);}
'''
  run(code)

 def test_pending_storage_refresh_runs_after_transfer_without_leaving_settings(self):
  code=PREFIX+r'''
static bool g_settings_mode=true,g_settings_scan_pending=true,g_settings_scan_busy,busy;static int g_io_state,scans;
static bool mutations_busy(void){return busy;}
static void refresh_settings_snapshot(bool load_network){assert(!load_network && !g_settings_scan_pending);scans++;}
'''+function('refresh_pending_settings_scan')+r'''
int main(void){
 busy=true;refresh_pending_settings_scan();assert(!scans && g_settings_scan_pending);
 busy=false;g_io_state=1;refresh_pending_settings_scan();assert(!scans && g_settings_scan_pending);
 g_io_state=0;g_discard_io=true;refresh_pending_settings_scan();assert(!scans);
 g_discard_io=false;g_settings_mode=false;refresh_pending_settings_scan();assert(!scans);
 g_settings_mode=true;refresh_pending_settings_scan();assert(scans==1 && !g_settings_scan_pending);
 refresh_pending_settings_scan();assert(scans==1);
}
'''
  run(code)
