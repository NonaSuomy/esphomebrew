"""Replay hosted discovery and HID reports through the production settings endpoint."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class Bluetooth(unittest.TestCase):
 def test_companion_and_host_share_the_wire_contract(self):
  companion=ROOT.parent/'pax-recipe/firmware/bluedroid-hid/components/pax_bluetooth/include'
  if not companion.exists():self.skipTest('Workspace companion source is not present in the standalone store')
  for name in ['pax_bt_discovery.h','pax_hosted_protocol.h']:
   self.assertEqual((ROOT/'components/pax_loader'/name).read_bytes(),(companion/name).read_bytes())

 def test_discovery_selection_bounds_inputs_and_link_loss(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td)
   (p/'esphome/core').mkdir(parents=True)
   (p/'esphome/core/log.h').write_text('#pragma once\n#define ESP_LOGI(...) ((void)0)\n')
   (p/'esp_timer.h').write_text('#pragma once\n#include <stdint.h>\nstatic int64_t now=100;\nstatic int64_t esp_timer_get_time(){return now;}\n')
   (p/'esp_hosted_misc.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
static int sends,registrations;static uint8_t sent[12];
static int esp_hosted_register_custom_callback(uint32_t,void (*)(uint32_t,const uint8_t *,size_t,void *),void *){registrations++;return 0;}
static int esp_hosted_send_custom_data(uint32_t id,const uint8_t *data,size_t n){assert(id==PAX_BT_DISCOVERY_COMMAND_ID && n==12);memcpy(sent,data,n);sends++;return 0;}
''')
   (p/'test.cpp').write_text(r'''
#include <cassert>
#include <cstring>
#include <climits>
#define CONFIG_ESP_HOSTED_ENABLED 1
#define ESP_OK 0
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#include "pax_bluetooth_stream.h"
#include "pax_bluetooth_keys.h"
using namespace pax_bluetooth;
static pax_bluetooth_status_t snapshot(){FILE *f=pax_bluetooth::open("rb");assert(f);pax_bluetooth_status_t s{};assert(fread(&s,1,sizeof(s),f)==sizeof(s));assert(fgetc(f)==EOF);fclose(f);return s;}
static void event(pax_bt_discovery_event_t e){received(PAX_BT_DISCOVERY_MSG_ID,(uint8_t *)&e,sizeof(e),nullptr);}
int main(){
 static_assert(sizeof(pax_bt_discovery_command_t)==12);static_assert(sizeof(pax_bt_discovery_event_t)==63);
 loop();assert(registrations==4 && !snapshot().available);
 pax_bluetooth_request_t q{};q.magic=PAX_BLUETOOTH_MAGIC;q.command.version=1;q.command.operation=PAX_BT_SCAN;
 assert(submit(&q)!=0);pax_bt_discovery_event_t e{};e.version=1;e.event=PAX_BT_HEARTBEAT;e.capabilities=3;event(e);assert(snapshot().available && snapshot().capabilities==3);
 q.command.operation=99;assert(submit(&q)!=0);q.command.operation=PAX_BT_SCAN;
 FILE *f=pax_bluetooth::open("wb");assert(fwrite(&q,1,2,f)==2 && fclose(f)!=0);assert(!snapshot().busy);
 f=pax_bluetooth::open("wb");assert(fwrite(&q,1,sizeof(q),f)==sizeof(q) && fclose(f)==0);
 assert(submit(&q)!=0 && snapshot().busy==1);loop();assert(sends==1 && sent[1]==PAX_BT_SCAN);
 e.event=PAX_BT_DEVICE;e.generation=snapshot().generation;e.ble=1;e.hid=1;memset(e.name,'x',sizeof(e.name));e.address[0]=1;
 event(e);event(e);assert(snapshot().count==1 && snapshot().devices[0].name[47]==0);
 for(int i=2;i<30;i++){e.address[0]=i;event(e);}assert(snapshot().count==16);
 e.event=PAX_BT_SCAN_DONE;e.generation++;event(e);assert(snapshot().busy==1);e.generation--;event(e);assert(!snapshot().busy);
 q.command.operation=PAX_BT_CONNECT;q.command.ble=1;memcpy(q.command.address,snapshot().devices[0].address,6);assert(submit(&q)==0);loop();assert(sends==2 && sent[1]==PAX_BT_CONNECT && sent[4]==1);
 e.event=PAX_BT_PAIRING_CODE;strcpy(e.name,"Type 123456 then Enter");event(e);assert(strstr(snapshot().message,"123456"));
 e.event=PAX_BT_DEVICE;e.address[0]=1;e.connected=1;event(e);assert(!snapshot().busy && snapshot().devices[0].connected);
 bluepad32_hosted_keyboard_t k{};k.magic=BLUEPAD32_HOSTED_MAGIC;k.version=1;k.connected=1;k.key_count=255;k.pressed_keys[0]=4;
 received(BLUEPAD32_HOSTED_KEYBOARD_MSG_ID,(uint8_t *)&k,sizeof(k),nullptr);
 KeyEvent key{};bool reset=false;assert(pop_key(&key,&reset) && key.down && key.usage==4 && !reset);assert(pax_bluetooth_key(key.usage,0)=='a');
 k.connected=0;received(BLUEPAD32_HOSTED_KEYBOARD_MSG_ID,(uint8_t *)&k,sizeof(k),nullptr);assert(pop_key(&key,&reset) && !key.down);
 k.connected=1;received(BLUEPAD32_HOSTED_KEYBOARD_MSG_ID,(uint8_t *)&k,sizeof(k),nullptr);assert(pop_key(&key,&reset) && key.down);
 now+=6000000;loop();assert(pop_key(&key,&reset) && !key.down);
 assert(pax_bluetooth_key(79,0)==154 && pax_bluetooth_key(82,0)==153 && pax_bluetooth_key(4,2)=='A');
 bluepad32_hosted_mouse_t m{};m.magic=BLUEPAD32_HOSTED_MAGIC;m.version=1;m.connected=1;m.delta_x=INT_MAX;m.delta_y=INT_MIN;m.buttons=1;
 received(BLUEPAD32_HOSTED_MOUSE_MSG_ID,(uint8_t *)&m,sizeof(m),nullptr);int32_t x,y,w;take_mouse(&x,&y,&w);assert(x==32767 && y==-32767 && buttons()==1);
 bluepad32_hosted_state_t pad_state{};pad_state.magic=BLUEPAD32_HOSTED_MAGIC;pad_state.version=1;pad_state.connected=1;pad_state.slot=255;
 received(BLUEPAD32_HOSTED_MSG_ID,(uint8_t *)&pad_state,sizeof(pad_state),nullptr);assert(!pad(&pad_state));
 now+=13000000;loop();assert(!snapshot().available && !buttons() && !snapshot().devices[0].connected);
}
''')
   subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(p),'-I'+str(ROOT/'components/pax_loader'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   import os
   env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
   subprocess.run([str(p/'test')],check=True,env=env)
