#include "pax_port.h"
#include <input.hpp>
#include <render.hpp>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

std::string pax_host_choose_project();
namespace {
int frameNo, mode, scenario, total, width, height;
int top=86,rowHeight=38;
void position(int &x,int &y,bool &down) {
  int rows=(height-top-64)/rowHeight;
  x=120;y=top+(scenario==1?rows-1:scenario==3?8:3)*rowHeight+15;
  down=frameNo>=2;
  if(scenario==2) {
    if(frameNo==1){x=width-24;y=top+10;down=true;}
    else if(frameNo==2){x=width-24;y=top+rows*rowHeight-2;down=true;}
    else if(frameNo==3){down=false;}
    else if(frameNo>=4){x=120;y=top+15;down=true;}
  }
}
}
void testProjectPicker(app_services_t &svc) {
  auto saved=svc;
  svc.delay_ms=[](int){};
  svc.input_keyboard_read=[](pax_keyboard_event_t*){return 0;};
  svc.display_get_size=[](int*w,int*h){*w=width;*h=height;};
  svc.file_list_dir=[](const char*,char *out,int capacity){
    int at=0;for(int i=0;i<total;i++){char name[32];snprintf(name,sizeof(name),"Project%02d.sb3",i);int n=strlen(name)+1;assert(at+n<capacity);memcpy(out+at,name,n);at+=n;}out[at]=0;return at+1;
  };
  svc.input_gamepad_read=[](pax_gamepad_state_t*p){memset(p,0,sizeof(*p));if(frameNo>=2)p->values[PAX_INPUT_A]=1;};
  svc.touch_read=[](int*x,int*y){int px,py;bool down;position(px,py,down);if(mode!=1){*x=px;*y=py;}return mode==0&&down?1:0;};
  svc.input_mouse_read=[](int*dx,int*dy,int*b){int x,y;bool down;position(x,y,down);auto old=Input::getTouchPosition();*dx=x-old[0];*dy=y-old[1];*b=mode!=0&&down?1:0;return 1;};
  svc.input_mouse_scroll_read=[](int*delta){*delta=scenario==1&&frameNo==1?-20:0;return *delta!=0?1:0;};
  svc.display_write_frame_custom=[](const uint16_t *pixels,uint16_t w,uint16_t h,float,bool){
    if(scenario==3&&frameNo==0) {
      for(int row=0;row<9;row++){int ink=0;for(int y=top+row*rowHeight+5;y<top+row*rowHeight+33;y++)for(int x=25;x<300;x++)if(pixels[y*w+x]==0xffff)ink++;assert(ink>10);}
      FILE*f=fopen("/tmp/scratch-picker-nine-projects.ppm","wb");fprintf(f,"P6\n%d %d\n255\n",w,h);
      for(int i=0;i<w*h;i++){unsigned v=pixels[i];unsigned char rgb[]={static_cast<unsigned char>(((v>>11)&31)*255/31),static_cast<unsigned char>(((v>>5)&63)*255/63),static_cast<unsigned char>((v&31)*255/31)};fwrite(rgb,1,3,f);}fclose(f);
    }
    assert(++frameNo<20);
  };
  auto run=[&](int inputMode,int testCase,int w,int h,int count,const char*expected){
    mode=inputMode;scenario=testCase;total=count;width=w;height=h;frameNo=0;OS::toExit=false;
    pax_editor_width=w;pax_editor_height=h;pax_project_playing=pax_editor_playing=false;
    assert(pax_host_choose_project()==expected);
  };
  for(int backend=0;backend<3;backend++)run(backend,0,1024,600,40,"@edit:Project03.sb3");
  puts("PASS picker row under touch/relative mouse/desktop pointer wins over simultaneous controller A");
  run(2,3,1024,600,9,"@edit:Project08.sb3");
  puts("PASS picker displays all nine project names on 1024x600 and selects the ninth");
  run(2,1,1024,600,40,"@edit:Project39.sb3");
  run(2,1,800,480,40,"@edit:Project39.sb3");
  puts("PASS wheel reaches final project at native and fallback resolutions");
  run(2,2,1024,600,40,"@edit:Project29.sb3");
  puts("PASS scrollbar thumb drag scrolls without launching the highlighted project");
  svc=saved;OS::toExit=false;
}
