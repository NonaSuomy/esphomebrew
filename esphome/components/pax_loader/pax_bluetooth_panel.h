// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lvgl.h"
#include "pax_bluetooth_settings.h"
#include <stdio.h>
#include <string.h>
// Shared native/PAX settings panel. Backends copy snapshots; all LVGL work
// runs on the display owner through its timer/event callbacks.
typedef struct {int (*read)(pax_bluetooth_status_t *);int (*write)(const pax_bluetooth_request_t *);} pax_bluetooth_panel_backend_t;
static lv_obj_t *pax_bt_panel_root,*pax_bt_status_label,*pax_bt_radio_label,*pax_bt_picker,*pax_bt_scan_button,*pax_bt_connect_button;
static lv_timer_t *pax_bt_panel_timer;
static lv_group_t *pax_bt_panel_group,*pax_bt_saved_default,*pax_bt_old_groups[16];
static lv_indev_t *pax_bt_inputs[16];static unsigned pax_bt_inputs_count;
static pax_bluetooth_status_t pax_bt_panel_snapshot;
static pax_bluetooth_panel_backend_t pax_bt_panel_backend;
static uint32_t pax_bt_panel_generation=UINT32_MAX,pax_bt_panel_count=UINT32_MAX;
static void pax_bt_panel_close(void){
 if(pax_bt_panel_timer){lv_timer_delete(pax_bt_panel_timer);pax_bt_panel_timer=NULL;}
 if(pax_bt_panel_root){
  for(unsigned i=0;i<pax_bt_inputs_count;i++)lv_indev_set_group(pax_bt_inputs[i],pax_bt_old_groups[i]);
  lv_obj_delete(pax_bt_panel_root);pax_bt_panel_root=NULL;
  lv_group_delete(pax_bt_panel_group);pax_bt_panel_group=NULL;
 }
}
static void pax_bt_panel_refresh(lv_timer_t *timer){
 (void)timer;if(!pax_bt_panel_root)return;
 pax_bluetooth_status_t s={0};
 if(!pax_bt_panel_backend.read || pax_bt_panel_backend.read(&s)!=0 || s.magic!=PAX_BLUETOOTH_MAGIC || s.count>PAX_BLUETOOTH_MAX_DEVICES){
  lv_label_set_text(pax_bt_status_label,"Bluetooth settings unavailable; update P4 firmware");
  lv_obj_add_state(pax_bt_scan_button,LV_STATE_DISABLED);lv_obj_add_state(pax_bt_connect_button,LV_STATE_DISABLED);return;
 }
 pax_bt_panel_snapshot=s;
 lv_label_set_text(pax_bt_radio_label,!s.available ? "Radio not detected" : (s.capabilities&3)==3 ? "Radio supports: Classic + BLE" : (s.capabilities&2) ? "Radio supports: Classic" : "Radio supports: BLE");
 lv_label_set_text(pax_bt_status_label,s.available ? s.message : "Bluetooth companion firmware required. Transport support follows the installed radio.");
 if(s.available && !s.busy)lv_obj_remove_state(pax_bt_scan_button,LV_STATE_DISABLED);else lv_obj_add_state(pax_bt_scan_button,LV_STATE_DISABLED);
 if(s.available && !s.busy && s.count && s.devices[lv_dropdown_get_selected(pax_bt_picker)<s.count?lv_dropdown_get_selected(pax_bt_picker):0].hid)lv_obj_remove_state(pax_bt_connect_button,LV_STATE_DISABLED);else lv_obj_add_state(pax_bt_connect_button,LV_STATE_DISABLED);
 // Preserve the user's selection while signal/connection status refreshes.
 uint32_t selected=lv_dropdown_get_selected(pax_bt_picker);
 char options[2048]={0};size_t used=0;
 for(uint32_t i=0;i<s.count;i++){
  const pax_bt_discovery_event_t *d=&s.devices[i];
  int n=lv_snprintf(options+used,sizeof(options)-used,"%s%s [%02X:%02X:%02X:%02X:%02X:%02X] %s%s%s",i?"\n":"",d->name,d->address[0],d->address[1],d->address[2],d->address[3],d->address[4],d->address[5],d->ble?"BLE":"Classic",d->hid?" | Input":" | Nearby only",d->connected?" | Connected":"");
  if(n<0 || (size_t)n>=sizeof(options)-used)break;
  used+=(size_t)n;
 }
 if(!s.count)strcpy(options,s.busy==1 ? "Scanning...":"No devices scanned");
 const char *old=lv_dropdown_get_options(pax_bt_picker);
 if(!old || strcmp(old,options)){
  lv_dropdown_set_options(pax_bt_picker,options);
  if(s.generation==pax_bt_panel_generation && selected<s.count)lv_dropdown_set_selected(pax_bt_picker,selected);
 }
 pax_bt_panel_generation=s.generation;pax_bt_panel_count=s.count;
}
static void pax_bt_panel_event(lv_event_t *event){
 uintptr_t action=(uintptr_t)lv_event_get_user_data(event);
 if(action==0){pax_bt_panel_close();return;}
 pax_bluetooth_request_t request={0};request.magic=PAX_BLUETOOTH_MAGIC;request.command.version=PAX_BT_DISCOVERY_VERSION;
 request.command.operation=action==1 ? PAX_BT_SCAN:PAX_BT_CONNECT;
 if(action==2){
  unsigned i=lv_dropdown_get_selected(pax_bt_picker);if(i>=pax_bt_panel_snapshot.count)return;
  const pax_bt_discovery_event_t *d=&pax_bt_panel_snapshot.devices[i];if(!d->hid){lv_label_set_text(pax_bt_status_label,"Nearby device: this profile does not support connecting it");return;}memcpy(request.command.address,d->address,6);request.command.ble=d->ble;
 }
 if(!pax_bt_panel_backend.write || pax_bt_panel_backend.write(&request)!=0)lv_label_set_text(pax_bt_status_label,"Bluetooth is unavailable or busy; retry shortly");
 else pax_bt_panel_refresh(NULL);
}
static void pax_bt_panel_key(int key){
 if(!pax_bt_panel_root)return;
 if(key==27){if(lv_dropdown_is_open(pax_bt_picker))lv_dropdown_close(pax_bt_picker);else pax_bt_panel_close();return;}
 if(key==153 || key==156){if(lv_dropdown_is_open(pax_bt_picker))lv_group_send_data(pax_bt_panel_group,LV_KEY_UP);else lv_group_focus_prev(pax_bt_panel_group);}
 else if(key==155 || key==154 || key==9){if(lv_dropdown_is_open(pax_bt_picker))lv_group_send_data(pax_bt_panel_group,LV_KEY_DOWN);else lv_group_focus_next(pax_bt_panel_group);}
 else if(key==13 || key==32)lv_group_send_data(pax_bt_panel_group,LV_KEY_ENTER);
}
static lv_obj_t *pax_bt_panel_button(lv_obj_t *parent,const char *text,uintptr_t action){
 lv_obj_t *button=lv_button_create(parent);lv_obj_set_size(button,180,44);
 lv_obj_set_style_bg_color(button,lv_color_hex(0x003452),0);lv_obj_set_style_border_color(button,lv_color_hex(0x23CFFF),0);lv_obj_set_style_border_width(button,1,0);
 lv_obj_t *label=lv_label_create(button);lv_label_set_text(label,text);lv_obj_center(label);
 lv_obj_add_event_cb(button,pax_bt_panel_event,LV_EVENT_CLICKED,(void *)action);return button;
}
static void pax_bt_panel_open(pax_bluetooth_panel_backend_t backend){
 if(pax_bt_panel_root)return;
 pax_bt_saved_default=lv_group_get_default();pax_bt_panel_group=lv_group_create();lv_group_set_default(pax_bt_panel_group);
 pax_bt_inputs_count=0;lv_indev_t *input=NULL;
 while((input=lv_indev_get_next(input)) && pax_bt_inputs_count<16)if(lv_indev_get_type(input)==LV_INDEV_TYPE_KEYPAD || lv_indev_get_type(input)==LV_INDEV_TYPE_ENCODER){pax_bt_inputs[pax_bt_inputs_count]=input;pax_bt_old_groups[pax_bt_inputs_count++]=lv_indev_get_group(input);lv_indev_set_group(input,pax_bt_panel_group);}
 pax_bt_panel_backend=backend;pax_bt_panel_generation=UINT32_MAX;pax_bt_panel_count=UINT32_MAX;
 pax_bt_panel_root=lv_obj_create(lv_layer_top());lv_obj_set_size(pax_bt_panel_root,lv_pct(100),lv_pct(100));lv_obj_center(pax_bt_panel_root);
 lv_obj_set_style_bg_color(pax_bt_panel_root,lv_color_hex(0x061326),0);lv_obj_set_style_bg_opa(pax_bt_panel_root,LV_OPA_COVER,0);lv_obj_set_style_text_color(pax_bt_panel_root,lv_color_hex(0xD7F3FF),0);
 lv_obj_set_flex_flow(pax_bt_panel_root,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(pax_bt_panel_root,14,0);
 lv_obj_t *title=lv_label_create(pax_bt_panel_root);lv_label_set_text(title,"BLUETOOTH INPUT DEVICES");
 lv_obj_t *help=lv_label_create(pax_bt_panel_root);lv_obj_set_width(help,lv_pct(100));lv_label_set_text(help,"Put your controller, keyboard or mouse in pairing mode. Scan, select the device, then connect. BLE and Classic availability depends on your radio. Audio needs a separate integration.");
 pax_bt_radio_label=lv_label_create(pax_bt_panel_root);
 pax_bt_status_label=lv_label_create(pax_bt_panel_root);lv_obj_set_width(pax_bt_status_label,lv_pct(100));
 pax_bt_picker=lv_dropdown_create(pax_bt_panel_root);lv_obj_set_width(pax_bt_picker,lv_pct(100));lv_dropdown_set_options(pax_bt_picker,"No devices scanned");
 lv_obj_set_style_bg_color(pax_bt_picker,lv_color_hex(0x003452),0);lv_obj_set_style_text_color(pax_bt_picker,lv_color_hex(0xD7F3FF),0);
 lv_obj_t *list=lv_dropdown_get_list(pax_bt_picker);lv_obj_set_style_bg_color(list,lv_color_hex(0x003452),0);lv_obj_set_style_text_color(list,lv_color_hex(0xD7F3FF),0);
 pax_bt_scan_button=pax_bt_panel_button(pax_bt_panel_root,"SCAN NEARBY",1);pax_bt_connect_button=pax_bt_panel_button(pax_bt_panel_root,"CONNECT",2);pax_bt_panel_button(pax_bt_panel_root,"BACK",0);
 lv_group_set_default(pax_bt_saved_default);
 pax_bt_panel_timer=lv_timer_create(pax_bt_panel_refresh,500,NULL);pax_bt_panel_refresh(NULL);
}
