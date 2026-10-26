#include <cassert>
#include <string>
#include "pax_ble_remote.h"
struct Fake {
 bool connected=true,paired=true,advertising=false;int sent=0,release=0;uint16_t buttons=0;uint8_t hat=8;
 bool is_connected(){return connected;}bool is_paired(){return paired;}bool is_advertising(){return advertising;}
 void send_string(const std::string&){sent++;}void send_key_combo(uint8_t,uint8_t){sent++;}
 void send_mouse_move(int8_t,int8_t){sent++;}void send_mouse_scroll(int8_t){sent++;}
 void send_mouse_click_start(uint8_t b){buttons=b;sent++;}void send_mouse_click_release(){buttons=0;release++;}
 void send_gamepad(uint16_t b,uint8_t h){buttons=b;hat=h;sent++;}void release_held(){release++;}
 void set_slot_broadcast(uint8_t,bool v){advertising=v;}
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
}
