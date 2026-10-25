// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "pax_bluetooth_settings.h"
#include "pax_hosted_protocol.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "esp_timer.h"
#include "esphome/core/log.h"
#if __has_include("esp_hosted_misc.h") && defined(CONFIG_ESP_HOSTED_ENABLED)
extern "C" {
#include "esp_hosted_misc.h"
}
#define PAX_BLUETOOTH_HOSTED 1
#endif
namespace pax_bluetooth {
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static pax_bluetooth_status_t state{PAX_BLUETOOTH_MAGIC,0,0,0,0,0,"Bluetooth companion firmware required"};
static int64_t heartbeat,deadline,seen[PAX_BLUETOOTH_MAX_DEVICES];
static bool queued;
static uint8_t connecting_address[6];static bool connecting_ble;
static pax_bluetooth_request_t pending;
static bluepad32_hosted_state_t pads[4];static int64_t pad_seen[4];
static bluepad32_hosted_keyboard_t keyboard;static int64_t keyboard_seen;
struct KeyEvent{uint8_t usage,modifiers;bool down;};
static KeyEvent keys[64];static unsigned key_head,key_tail;static bool key_reset;
static bool held[256];
static void keyboard_update(const bluepad32_hosted_keyboard_t &report){
 bool next[256]={};
 if(report.connected){for(unsigned i=0;i<report.key_count;i++)if(report.pressed_keys[i]>=4)next[report.pressed_keys[i]]=true;for(unsigned i=0;i<8;i++)if(report.modifiers&(1<<i))next[224+i]=true;}
 for(unsigned i=4;i<256;i++)if(next[i]!=held[i]){
  unsigned end=(key_head+1)%64;
  if(end==key_tail){key_reset=true;key_head=key_tail=0;memset(held,0,sizeof(held));return;}
  keys[key_head]={uint8_t(i),report.modifiers,next[i]};key_head=end;
 }
 memcpy(held,next,sizeof(held));
}
static bool pop_key(KeyEvent *event,bool *reset){portENTER_CRITICAL(&lock);*reset=key_reset;key_reset=false;bool ok=key_head!=key_tail;if(ok){*event=keys[key_tail];key_tail=(key_tail+1)%64;}portEXIT_CRITICAL(&lock);return ok;}

static int32_t mouse_dx,mouse_dy,mouse_wheel,pointer_dx,pointer_dy;static uint16_t mouse_buttons;static int64_t mouse_seen;
static void received(uint32_t id,const uint8_t *bytes,size_t n,void *) {
 const int64_t now=esp_timer_get_time();
 portENTER_CRITICAL(&lock);
 if(id==PAX_BT_DISCOVERY_MSG_ID && n==sizeof(pax_bt_discovery_event_t)){
  pax_bt_discovery_event_t e;memcpy(&e,bytes,n);
  if(e.version==PAX_BT_DISCOVERY_VERSION && e.event>=PAX_BT_DEVICE && e.event<=PAX_BT_PAIRING_CODE && e.ble<=1 && e.connected<=1 && e.hid<=1){
   heartbeat=now;state.available=1;state.capabilities=e.capabilities&3;
   if(e.event==PAX_BT_DEVICE && (e.generation==state.generation || e.connected)){
    unsigned i=0;for(;i<state.count;i++)if(state.devices[i].ble==e.ble && !memcmp(state.devices[i].address,e.address,6))break;
    if(i>=PAX_BLUETOOTH_MAX_DEVICES && e.hid)for(unsigned j=0;j<state.count;j++)if(!state.devices[j].hid){i=j;break;}
    if(i<PAX_BLUETOOTH_MAX_DEVICES){
     if(i==state.count)state.count++;
     e.name[sizeof(e.name)-1]=0;
     for(char &ch:e.name)if(ch && (uint8_t(ch)<32 || ch=='\n' || ch=='\r'))ch=' ';
     state.devices[i]=e;seen[i]=now;
     if(e.connected && state.busy==2 && e.ble==connecting_ble && !memcmp(e.address,connecting_address,6)){state.busy=0;snprintf(state.message,sizeof(state.message),"Connected: %s",e.name);}
    }
   }else if(e.event==PAX_BT_PAIRING_CODE){e.name[sizeof(e.name)-1]=0;snprintf(state.message,sizeof(state.message),"%s",e.name);deadline=now+90000000;
   }else if(e.event==PAX_BT_SCAN_DONE && e.generation==state.generation && state.busy==1){state.busy=0;snprintf(state.message,sizeof(state.message),state.count ? "Select a device, then CONNECT" : "No Bluetooth devices found; put the device in pairing mode and rescan");}
   else if(!state.busy && !state.count)snprintf(state.message,sizeof(state.message),"Bluetooth input ready; choose SCAN NEARBY");
  }
 }else if(n>=4 && bytes[0]==(BLUEPAD32_HOSTED_MAGIC&255) && bytes[1]==(BLUEPAD32_HOSTED_MAGIC>>8) && bytes[2]==1 && bytes[3]<=1){
  if(id==BLUEPAD32_HOSTED_MSG_ID && n==sizeof(bluepad32_hosted_state_t)){
   bluepad32_hosted_state_t p;memcpy(&p,bytes,n);if(p.slot<4){pads[p.slot]=p;pad_seen[p.slot]=now;}
  }else if(id==BLUEPAD32_HOSTED_KEYBOARD_MSG_ID && n==sizeof(keyboard)){
   memcpy(&keyboard,bytes,n);keyboard.key_count=std::min<uint8_t>(keyboard.key_count,10);keyboard_seen=now;keyboard_update(keyboard);
  }else if(id==BLUEPAD32_HOSTED_MOUSE_MSG_ID && n==sizeof(bluepad32_hosted_mouse_t)){
   bluepad32_hosted_mouse_t m;memcpy(&m,bytes,n);mouse_seen=now;mouse_buttons=m.connected ? m.buttons : 0;
   // Saturate event accumulators rather than allowing malformed/repeated input to overflow.
   mouse_dx=std::clamp<int64_t>(int64_t(mouse_dx)+(m.connected?m.delta_x:0),-32767,32767);
   mouse_dy=std::clamp<int64_t>(int64_t(mouse_dy)+(m.connected?m.delta_y:0),-32767,32767);
   pointer_dx=std::clamp<int64_t>(int64_t(pointer_dx)+(m.connected?m.delta_x:0),-32767,32767);
   pointer_dy=std::clamp<int64_t>(int64_t(pointer_dy)+(m.connected?m.delta_y:0),-32767,32767);
   mouse_wheel=std::clamp<int>(mouse_wheel+(m.connected?m.scroll_wheel:0),-127,127);
  }
 }
 portEXIT_CRITICAL(&lock);
}
static void loop(){
#ifdef PAX_BLUETOOTH_HOSTED
 static uint8_t registered;
 static int64_t retry;
 const uint32_t ids[]={PAX_BT_DISCOVERY_MSG_ID,BLUEPAD32_HOSTED_MSG_ID,BLUEPAD32_HOSTED_KEYBOARD_MSG_ID,BLUEPAD32_HOSTED_MOUSE_MSG_ID};
 if(registered!=15 && esp_timer_get_time()>=retry){
  retry=esp_timer_get_time()+5000000;
  for(unsigned i=0;i<4;i++)if(!(registered&(1<<i)) && esp_hosted_register_custom_callback(ids[i],received,nullptr)==ESP_OK)registered|=1<<i;
 }
#endif
 pax_bluetooth_request_t request{};bool send=false;
 const int64_t now=esp_timer_get_time();
 portENTER_CRITICAL(&lock);
 state.available=heartbeat && now-heartbeat<12000000;
 if(keyboard_seen && now-keyboard_seen>5000000){bluepad32_hosted_keyboard_t empty{};keyboard_update(empty);keyboard_seen=0;}
 if(mouse_seen && now-mouse_seen>5000000)mouse_buttons=0;
 for(unsigned i=0;i<state.count;i++)if(now-seen[i]>12000000)state.devices[i].connected=0;
 if(state.busy && now>=deadline){state.busy=0;snprintf(state.message,sizeof(state.message),"Bluetooth request timed out; retry with the device in pairing mode");}
 if(queued){request=pending;queued=false;send=true;}
 portEXIT_CRITICAL(&lock);
 static uint32_t last_generation,last_count,last_busy,last_available;
 pax_bluetooth_status_t status;portENTER_CRITICAL(&lock);status=state;portEXIT_CRITICAL(&lock);
 if(status.available!=last_available){last_available=status.available;ESP_LOGI("pax_bluetooth","Companion %s; capabilities BLE=%u Classic=%u",status.available?"ready":"unavailable",unsigned(status.capabilities&1),unsigned((status.capabilities>>1)&1));}
 if(status.generation!=last_generation || status.count!=last_count || status.busy!=last_busy){last_generation=status.generation;last_count=status.count;last_busy=status.busy;ESP_LOGI("pax_bluetooth","Discovery generation=%u state=%u devices=%u: %s",unsigned(status.generation),unsigned(status.busy),unsigned(status.count),status.message);}
 if(send){
  bool ok=false;
#ifdef PAX_BLUETOOTH_HOSTED
  ok=esp_hosted_send_custom_data(PAX_BT_DISCOVERY_COMMAND_ID,reinterpret_cast<const uint8_t *>(&request.command),sizeof(request.command))==ESP_OK;
#endif
  if(!ok){portENTER_CRITICAL(&lock);state.busy=0;snprintf(state.message,sizeof(state.message),"Bluetooth command failed; check companion firmware and retry");portEXIT_CRITICAL(&lock);}
 }
}
static bool get(pax_bluetooth_status_t *out){portENTER_CRITICAL(&lock);*out=state;portEXIT_CRITICAL(&lock);return true;}
static int submit(const pax_bluetooth_request_t *r){
 if(!r || r->magic!=PAX_BLUETOOTH_MAGIC || !pax_bt_discovery_command_valid(&r->command,sizeof(r->command)))return -1;
 portENTER_CRITICAL(&lock);
 if(!state.available || state.busy || queued){portEXIT_CRITICAL(&lock);return -1;}
 if(r->command.operation==PAX_BT_CONNECT){
  bool supported=false;
  for(unsigned i=0;i<state.count;i++)if(state.devices[i].ble==r->command.ble && !memcmp(state.devices[i].address,r->command.address,6))supported=state.devices[i].hid;
  if(!supported || !(state.capabilities&(r->command.ble?1:2))){portEXIT_CRITICAL(&lock);return -1;}
 }
 pending=*r;pending.command.generation=uint16_t(state.generation+1);if(!pending.command.generation)pending.command.generation=1;
 state.generation=pending.command.generation;queued=true;
 state.busy=r->command.operation==PAX_BT_SCAN ? 1:2;
 if(state.busy==1)state.count=0;
 else {memcpy(connecting_address,r->command.address,6);connecting_ble=r->command.ble;}
 snprintf(state.message,sizeof(state.message),state.busy==1 ? "Scanning nearby Bluetooth input devices..." : "Connecting; follow any pairing instructions on the device...");
 deadline=esp_timer_get_time()+(state.busy==1?15000000:40000000);
 portEXIT_CRITICAL(&lock);return 0;
}
static uint16_t buttons(){portENTER_CRITICAL(&lock);uint16_t result=mouse_buttons;portEXIT_CRITICAL(&lock);return result;}
static void take_pointer(int *x,int *y,bool *down){portENTER_CRITICAL(&lock);*x=pointer_dx;*y=pointer_dy;pointer_dx=pointer_dy=0;*down=(mouse_buttons&1)!=0;portEXIT_CRITICAL(&lock);}
static void take_mouse(int32_t *x,int32_t *y,int32_t *wheel){portENTER_CRITICAL(&lock);*x=mouse_dx;*y=mouse_dy;*wheel=mouse_wheel;mouse_dx=mouse_dy=mouse_wheel=0;portEXIT_CRITICAL(&lock);}
static bool pad(bluepad32_hosted_state_t *out){
 bool found=false;const int64_t now=esp_timer_get_time();portENTER_CRITICAL(&lock);
 for(unsigned i=0;i<4;i++)if(pads[i].connected && now-pad_seen[i]<1000000){*out=pads[i];found=true;break;}
 portEXIT_CRITICAL(&lock);return found;
}
struct Stream{bool writing;size_t offset;pax_bluetooth_request_t request;pax_bluetooth_status_t response;};
static int close(void *cookie){auto *s=static_cast<Stream *>(cookie);int result=s->writing ? (s->offset==sizeof(s->request)?submit(&s->request):-1):0;free(s);return result;}
static FILE *open(const char *mode){
 if(!mode || (strcmp(mode,"rb") && strcmp(mode,"wb")))return nullptr;
 auto *s=static_cast<Stream *>(calloc(1,sizeof(Stream)));if(!s)return nullptr;s->writing=mode[0]=='w';get(&s->response);
 auto read=[](void *c,void *out,size_t n)->ssize_t{auto *s=static_cast<Stream *>(c);n=std::min(n,sizeof(s->response)-s->offset);memcpy(out,reinterpret_cast<char *>(&s->response)+s->offset,n);s->offset+=n;return n;};
 auto write=[](void *c,const void *in,size_t n)->ssize_t{auto *s=static_cast<Stream *>(c);if(n>sizeof(s->request)-s->offset)return -1;memcpy(reinterpret_cast<char *>(&s->request)+s->offset,in,n);s->offset+=n;return n;};
#ifdef ESP_PLATFORM
 FILE *f=funopen(s,s->writing?nullptr:+read,s->writing?+write:nullptr,nullptr,close);
#else
 cookie_io_functions_t io{};if(s->writing)io.write=+[](void *c,const char *in,size_t n)->ssize_t{auto *s=static_cast<Stream *>(c);if(n>sizeof(s->request)-s->offset)return -1;memcpy(reinterpret_cast<char *>(&s->request)+s->offset,in,n);s->offset+=n;return n;};else io.read=+[](void *c,char *out,size_t n)->ssize_t{auto *s=static_cast<Stream *>(c);n=std::min(n,sizeof(s->response)-s->offset);memcpy(out,reinterpret_cast<char *>(&s->response)+s->offset,n);s->offset+=n;return n;};io.close=close;
 FILE *f=fopencookie(s,mode,io);(void)read;(void)write;
#endif
 if(!f){free(s);return nullptr;}setvbuf(f,nullptr,_IONBF,0);return f;
}
}
