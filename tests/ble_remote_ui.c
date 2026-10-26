#define PAX_APP_SIDE 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../apps/ble_remote/main.c"
static uint64_t now_us;
static bool link_up=true;
static pax_touch_point_t fingers[5];static int fingers_n;
static int keys,shift_a,ctrl_c,move_x,move_y,clicks,scrolls,releases,pad_combo,pad_diagonal;
static int mock_remote(uint32_t cmd,int32_t a,int32_t b,const char *text){
 (void)text;
 switch(cmd){
 case PAX_BLE_STATUS:return link_up?7:5;
 case PAX_BLE_KEY:keys++;if(a==2&&b==4)shift_a++;if(a==1&&b==6)ctrl_c++;break;
 case PAX_BLE_MOUSE_MOVE:move_x+=a;move_y+=b;break;
 case PAX_BLE_MOUSE_BUTTONS:if(a)clicks++;break;
 case PAX_BLE_MOUSE_SCROLL:scrolls+=a;break;
 case PAX_BLE_RELEASE:releases++;break;
 case PAX_BLE_GAMEPAD:if(a==1&&b==0)pad_combo++;if(a==0&&b==3)pad_diagonal++;break;
 }
 return 0;
}
static int64_t clock_us(void){return now_us;}
static int touch(int*x,int*y){if(!fingers_n)return 0;*x=fingers[0].x;*y=fingers[0].y;return 1;}
static int touches(pax_touch_point_t*out,int max){int n=fingers_n<max?fingers_n:max;memcpy(out,fingers,n*sizeof(*out));return n;}
static void frame(const uint16_t*p,uint16_t w,uint16_t h,float scale,bool swap){(void)p;(void)w;(void)h;(void)scale;(void)swap;}
static void advance(int ms){for(int i=0;i<ms;i+=10){now_us+=10000;pump(NULL);}}
static void tap(int x,int y){fingers_n=1;fingers[0]=(pax_touch_point_t){x,y,0};advance(100);fingers_n=0;advance(100);}
static void snapshot(const char *path){FILE*f=fopen(path,"wb");assert(f);fprintf(f,"P6\n800 480\n255\n");for(int i=0;i<W*H;i++){unsigned v=canvas[i];unsigned char rgb[]={((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};fwrite(rgb,1,3,f);}fclose(f);}
int main(int argc,char **argv){
 (void)argc;(void)argv;
 app_services_t mock={0};mock.ble_remote=mock_remote;mock.get_time_us=clock_us;mock.touch_read=touch;mock.touch_read_points=touches;mock.display_write_frame_custom=frame;svc=&mock;
 canvas=calloc(W*H,2);assert(canvas);assert(setup(NULL)==0);advance(600);
 snapshot("/tmp/pax-ble-remote/keyboard.ppm");
 tap(42,186); // Q
 tap(160,353);tap(70,240); // Shift + A
 tap(60,353);tap(225,294); // Ctrl + C
 assert(keys==3&&shift_a==1&&ctrl_c==1);
 tap(400,70);assert(tab==1);advance(200);snapshot("/tmp/pax-ble-remote/mousepad.ppm");
 tap(70,420);tap(390,420);assert(dragging);
 fingers_n=1;fingers[0]=(pax_touch_point_t){200,220,0};advance(100);
 fingers[0].x+=24;fingers[0].y+=10;advance(100);fingers_n=0;advance(100);
 assert(move_x==24&&move_y==10);tap(545,420);assert(scrolls==1);
 tap(660,70);assert(tab==2&&!dragging);advance(200);snapshot("/tmp/pax-ble-remote/gamepad.ppm");
 fingers_n=2;fingers[0]=(pax_touch_point_t){145,170,0};fingers[1]=(pax_touch_point_t){430,210,1};advance(100);assert(pad_combo>0);
 fingers[0]=(pax_touch_point_t){225,250,0};fingers[1]=(pax_touch_point_t){145,350,1};advance(100);assert(pad_diagonal>0);
 fingers_n=0;advance(100);assert(game_buttons==0&&game_hat==8);
 fingers_n=1;fingers[0]=(pax_touch_point_t){145,170,0};advance(100);assert(game_hat==0);
 link_up=false;advance(600);assert(game_hat==8&&game_buttons==0);fingers_n=0;link_up=true;advance(600);
 tap(100,70);assert(tab==0);tap(747,20);assert(quitting&&releases>=4);
 assert(teardown(NULL)==0);free(canvas);
 printf("PASS: keyboard modifiers, mouse click/drag/motion/scroll, multitouch gamepad, diagonal hat and releases (%d keys, %d clicks, %d releases)\n",keys,clicks,releases);
}
