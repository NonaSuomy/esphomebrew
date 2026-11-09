#include <cassert>
#include <string>
#include "pax_ble_remote.h"
struct Fake {
 struct Slot{bool occupied=false,has_identity=false;std::string name;uint8_t addr[6]{},identity[6]{};};Slot slots[10]{};bool broadcasts[10]={true,true,true,true,true,true,true,true,true,true};uint8_t active=0;
 bool test_ready_=true;
 bool connected=true,paired=true,advertising=false;int sent=0,release=0;uint16_t buttons=0;uint8_t hat=8;
 bool is_connected(){return connected;}bool is_paired(){return paired;}bool is_advertising(){return advertising;}
 void send_string(const std::string&){sent++;}void send_key_combo(uint8_t,uint8_t){sent++;}
 void send_mouse_move(int8_t,int8_t){sent++;}void send_mouse_scroll(int8_t){sent++;}
 void send_mouse_click_start(uint8_t b){buttons=b;sent++;}void send_mouse_click_release(){buttons=0;release++;}
 void send_gamepad(uint16_t b,uint8_t h){buttons=b;hat=h;sent++;}void release_held(){release++;}
 uint8_t active_host_slot(){return active;}uint8_t host_slots(){return 10;}
 Slot &get_host_slot(uint8_t i){return slots[i];}bool slot_broadcasts(uint8_t i){return broadcasts[i];}
 void set_slot_broadcast(uint8_t i,bool v){broadcasts[i]=v;if(i==active){advertising=v;if(!v)connected=false;}}
 void switch_host(uint8_t i,bool keep=false){if(keep&&i==active&&connected)return;active=i;connected=false;advertising=broadcasts[i];}
 void forget_host(uint8_t i){slots[i].occupied=false;slots[i].name.clear();}
 bool set_host_name(uint8_t i,const std::string &n){slots[i].name=n;return true;}

};
int main(){
 using esphome::pax_loader::ble_remote::dispatch;
 Fake f;auto call=[&](int cmd,int a=0,int b=0,const char*t=nullptr){return dispatch(&f,cmd,a,b,t);};
 assert(dispatch<Fake>(nullptr,PAX_BLE_STATUS,0,0,nullptr)==-1);
 assert(call(PAX_BLE_STATUS)==7);
 assert(call(PAX_BLE_KEY,2,4)==0);assert(call(PAX_BLE_KEY,256,4)==-2);assert(call(PAX_BLE_KEY,0,0)==-2);
 assert(call(PAX_BLE_MOUSE_MOVE,-127,127)==0);assert(call(PAX_BLE_MOUSE_MOVE,128)==-2);
 assert(call(PAX_BLE_MOUSE_BUTTONS,7)==0);assert(f.buttons==7);assert(call(PAX_BLE_MOUSE_BUTTONS,8)==-2);
 assert(call(PAX_BLE_MOUSE_SCROLL,-127)==0);assert(call(PAX_BLE_MOUSE_SCROLL,-128)==-2);
 assert(call(PAX_BLE_GAMEPAD,65535,7)==0);assert(call(PAX_BLE_GAMEPAD,65536,7)==-2);assert(call(PAX_BLE_GAMEPAD,1,9)==-2);
 std::string s(128,'x');assert(call(PAX_BLE_TEXT,0,0,s.c_str())==0);s+='x';assert(call(PAX_BLE_TEXT,0,0,s.c_str())==-2);assert(call(PAX_BLE_TEXT)==-2);
 assert(call(99)==-2);f.connected=false;int previous=f.sent;assert(call(PAX_BLE_KEY,0,4)==-4);assert(f.sent==previous);
 assert(call(PAX_BLE_RELEASE)==0);assert(f.buttons==0&&f.hat==8&&f.release==2);
 assert(call(PAX_BLE_ADVERTISE,1)==0);assert(f.advertising);assert(call(PAX_BLE_ADVERTISE,2)==-2);
 assert(call(PAX_BLE_HOST_COUNT)==10);assert(call(PAX_BLE_ACTIVE_HOST)==0);

 assert(call(PAX_BLE_HOST_NAME_GET,0,0)==0);
 assert(call(PAX_BLE_HOST_NAME_SET,0,0,"iPhone")==-3);
 f.slots[0].occupied=true;f.slots[0].addr[0]=0x45;f.slots[0].identity[0]=0xAA;
 assert(call(PAX_BLE_HOST_ADDRESS,0,0)==0x45);f.slots[0].has_identity=true;
 assert(call(PAX_BLE_HOST_ADDRESS,0,0)==0xAA);
 assert(call(PAX_BLE_HOST_ADDRESS,0,6)==-2);
 assert(call(PAX_BLE_HOST_NAME_SET,0,0,"iPhone SE")==0);
 assert(call(PAX_BLE_HOST_NAME_GET,0,0)=='i');assert(call(PAX_BLE_HOST_NAME_GET,0,9)==0);
 assert(call(PAX_BLE_HOST_NAME_GET,0,32)==-2);assert(call(PAX_BLE_HOST_NAME_GET,10,0)==-2);
 assert(call(PAX_BLE_HOST_NAME_SET,0,0,std::string(32,'x').c_str())==-2);
 assert(call(PAX_BLE_HOST_NAME_SET,0,0,nullptr)==-2);
 f.slots[0].occupied=true;f.connected=true;f.advertising=false;
 assert(call(PAX_BLE_HOST_STATE,0)==(PAX_BLE_HOST_SAVED|PAX_BLE_HOST_ACTIVE|PAX_BLE_HOST_CONNECTED|PAX_BLE_HOST_ENABLED));
 assert(call(PAX_BLE_SELECT_HOST,0)==0&&f.connected); // keep the same live link
 assert(call(PAX_BLE_SELECT_HOST,-1)==-2);assert(call(PAX_BLE_HOST_STATE,10)==-2);assert(call(PAX_BLE_FORGET_HOST,10)==-2);
 assert(call(PAX_BLE_PAIR_NEW)==1&&f.active==1&&f.slots[0].occupied&&!f.connected);
 assert(call(PAX_BLE_ADVERTISE,0)==0&&!f.broadcasts[1]&&f.broadcasts[0]);
 assert(call(PAX_BLE_SELECT_HOST,1)==1&&f.broadcasts[1]);
 f.slots[1].occupied=true;assert(call(PAX_BLE_FORGET_HOST,1)==0&&!f.slots[1].occupied&&!f.broadcasts[1]);
 assert(f.slots[0].occupied); // no other pairing lost
 for(auto &s:f.slots)s.occupied=true;
 assert(call(PAX_BLE_PAIR_NEW)==-5&&f.active==1);
 for(auto &s:f.slots)assert(s.occupied);
 f.test_ready_=false;previous=f.sent;auto old_active=f.active;
 assert(call(PAX_BLE_STATUS)==0&&call(PAX_BLE_HOST_COUNT)==10);
 assert(call(PAX_BLE_RELEASE)==0&&f.sent==previous);
 assert(call(PAX_BLE_SELECT_HOST,0)==-6&&call(PAX_BLE_PAIR_NEW)==-6);
 assert(call(PAX_BLE_FORGET_HOST,0)==-6&&call(PAX_BLE_ADVERTISE,1)==-6);
 assert(f.active==old_active);for(auto &s:f.slots)assert(s.occupied);

}
