#!/usr/bin/env python3
"""Exercise the real shared Bluetooth panel against the existing host LVGL build."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];build=root/'.esphome/build/pax-desktop'
libs=list((build/'.pioenvs/pax-desktop').glob('*/liblvgl.a'))
assert len(libs)==1,'Build desktop profile first'
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(r'''
#include <cassert>
#include <cstring>
#include <cstdlib>
#include "pax_bluetooth_panel.h"
extern "C" {
void lv_mem_init() {}
void lv_mem_deinit() {}
void *lv_malloc_core(size_t n){return malloc(n);}
void lv_free_core(void *p){free(p);}
void *lv_realloc_core(void *p,size_t n){return realloc(p,n);}
lv_result_t lv_mem_test_core(){return LV_RESULT_OK;}
void lv_mem_monitor_core(lv_mem_monitor_t *p){memset(p,0,sizeof(*p));}
}
static pax_bluetooth_status_t current{};static unsigned writes;
static int read_status(pax_bluetooth_status_t *out){*out=current;return 0;}
static int write_command(const pax_bluetooth_request_t *r){writes++;assert(r->magic==PAX_BLUETOOTH_MAGIC);if(r->command.operation==PAX_BT_SCAN){current.busy=1;strcpy(current.message,"Scanning...");}else{assert(r->command.address[0]==1);current.busy=2;}return 0;}
int main(){
 lv_init();lv_display_t *display=lv_display_create(800,480);
 current.magic=PAX_BLUETOOTH_MAGIC;
 pax_bt_panel_open({read_status,write_command});assert(pax_bt_panel_root);
 assert(lv_obj_has_state(pax_bt_scan_button,LV_STATE_DISABLED));
 current.available=1;current.capabilities=3;pax_bt_panel_refresh(nullptr);
 assert(!lv_obj_has_state(pax_bt_scan_button,LV_STATE_DISABLED));assert(lv_obj_has_state(pax_bt_connect_button,LV_STATE_DISABLED));
 lv_obj_send_event(pax_bt_scan_button,LV_EVENT_CLICKED,nullptr);assert(writes==1 && current.busy==1);
 current.busy=0;current.generation=1;current.count=2;
 strcpy(current.devices[0].name,"Keyboard");current.devices[0].address[0]=1;current.devices[0].hid=1;current.devices[0].ble=1;
 strcpy(current.devices[1].name,"Beacon");current.devices[1].address[0]=2;current.devices[1].ble=1;
 pax_bt_panel_refresh(nullptr);assert(!lv_obj_has_state(pax_bt_connect_button,LV_STATE_DISABLED));
 assert(strstr(lv_dropdown_get_options(pax_bt_picker),"Nearby only"));
 lv_dropdown_set_selected(pax_bt_picker,1);pax_bt_panel_refresh(nullptr);assert(lv_obj_has_state(pax_bt_connect_button,LV_STATE_DISABLED));
 lv_obj_send_event(pax_bt_connect_button,LV_EVENT_CLICKED,nullptr);assert(writes==1);
 lv_dropdown_set_selected(pax_bt_picker,0);pax_bt_panel_refresh(nullptr);lv_obj_send_event(pax_bt_connect_button,LV_EVENT_CLICKED,nullptr);assert(writes==2);
 strcpy(current.message,"Type 123456 then Enter");pax_bt_panel_refresh(nullptr);assert(strstr(lv_label_get_text(pax_bt_status_label),"123456"));
 pax_bt_panel_key(27);assert(!pax_bt_panel_root && !pax_bt_panel_timer);
 lv_display_delete(display);lv_deinit();
 puts("Bluetooth panel: availability, scan, device selection, nearby-only guard, pairing status and close passed");
}
''')
 # The cached desktop build predates the runtime dropdown; compile that one
 # widget with the same configuration, leaving the shared cached library intact.
 (p/'lv_conf.h').write_text((build/'src/lv_conf.h').read_text().replace('#define LV_USE_DROPDOWN 0','#define LV_USE_DROPDOWN 1'))
 includes=['-I'+str(p),'-I'+str(root/'components/pax_loader'),'-I'+str(build/'src'),'-I'+str(build/'.piolibdeps/pax-desktop/lvgl')]
 subprocess.run(['cc','-std=c11','-DLV_CONF_INCLUDE_SIMPLE',*includes,'-c',str(build/'.piolibdeps/pax-desktop/lvgl/src/widgets/dropdown/lv_dropdown.c'),'-o',str(p/'dropdown.o')],check=True)
 subprocess.run(['c++','-std=c++20','-DLV_CONF_INCLUDE_SIMPLE',*includes,str(p/'test.cpp'),str(p/'dropdown.o'),str(libs[0]),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
