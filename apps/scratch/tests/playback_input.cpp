#include "pax_port.h"
#include <runtime.hpp>
#include <render.hpp>
#include <unzip.hpp>
#include <input.hpp>
#include <audio.hpp>
#include <filesystem>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <map>
#include <string>
static std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
static int frames=0,audioFrames=0;static long long audioEnergy=0;static bool menu=false;static bool uiTest=false;static bool keyboardUiTest=false;
static void delay(int n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
static int64_t clockUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();}
static int statFile(const char *p,pax_file_stat_t *o){std::error_code ec;auto st=std::filesystem::status(p,ec);if(ec||!std::filesystem::exists(st))return -1;o->is_dir=std::filesystem::is_directory(st);o->size=o->is_dir?0:std::filesystem::file_size(p);o->mtime=0;return 0;}
static int mkdirFile(const char *p){std::error_code ec;std::filesystem::create_directories(p,ec);return ec?-1:0;}
static void pad(pax_gamepad_state_t *p){memset(p,0,sizeof(*p));p->values[PAX_INPUT_MENU]=menu||(uiTest&&frames>35);if(uiTest&&!keyboardUiTest&&frames==1)p->values[PAX_INPUT_A]=1;}
static void frame(const uint16_t*p,uint16_t w,uint16_t h,float,bool){frames++;FILE*f=fopen("/tmp/parity-progress.ppm","wb");fprintf(f,"P6\n%d %d\n255\n",w,h);for(int i=0;i<w*h;i++){unsigned v=p[i];unsigned char rgb[]={static_cast<unsigned char>(((v>>11)&31)*255/31),static_cast<unsigned char>(((v>>5)&63)*255/63),static_cast<unsigned char>((v&31)*255/31)};fwrite(rgb,1,3,f);}fclose(f);}
static void submit(short *p,int n){audioFrames+=n;for(int i=0;i<n*2;i++)audioEnergy+=abs(p[i]);}
static int noTouch(int*,int*){return 0;}
static int noKey(pax_keyboard_event_t*){return 0;}
extern int pax_host_run();
void testEditor();
int main(int argc,char **argv){
 app_services_t svc{};svc.abi_version=1;svc.log_printf=printf;svc.log_vprintf=vprintf;svc.delay_ms=delay;svc.get_time_us=clockUs;svc.mem_alloc=malloc;svc.mem_calloc=calloc;svc.mem_realloc=realloc;svc.mem_free=free;svc.mem_caps_alloc=[](size_t n,uint32_t)->void*{return malloc(n);};svc.file_stat=statFile;svc.file_mkdir=mkdirFile;svc.input_gamepad_read=pad;svc.touch_read=noTouch;svc.input_keyboard_read=noKey;svc.display_write_frame_custom=frame;svc.display_get_size=[](int*w,int*h){*w=1024;*h=600;};svc.audio_init=[](int rate){assert(rate==48000);};svc.audio_submit=submit;
 svc.file_open=[](const char*p,const char*m)->void*{return fopen(p,m);};
 svc.file_close=[](void*f){return fclose((FILE*)f);};
 svc.file_seek=[](void*f,long n,int whence){return fseek((FILE*)f,n,whence);};
 svc.file_read=[](void*b,size_t n,size_t c,void*f){return fread(b,n,c,(FILE*)f);};
 pax_svc=&svc;


 std::map<std::string,float> initial={{"temperature_c",23.456789f},{"humidity_pct",52.678f},{"distance_cm",41.23f},{"light_lux",125.43f},{"gas_voltage",1.23456f},{"motion",1},{"accel_x",.126f},{"accel_y",-.234f},{"accel_z",1},{"gyro_x",12.34f},{"gyro_y",-.456f},{"gyro_z",2.34f},{"buttons_voltage",.734f},{"hall",1},{"touch_pad",0},{"microphone_level",15.12f},{"microphone_enabled",0},{"camera_ready",1},{"camera_capture_status",0},{"camera_capture",0},{"led_red",0},{"led_green",0},{"led_blue",0},{"led_white",0},{"led_brightness",0},{"servo",0},{"relay",0},{"display_brightness",80},{"speaker_enabled",0}};
 static std::map<std::string,float> samples;
 static std::vector<std::pair<std::string,float>> writes;
 static bool present=false;
 static int mode=0, pointerX=512,pointerY=300, pendingDX=0,pendingDY=0,pointerButtons=0;
 static bool pointerDown=false,mouseReport=false;
 svc.touch_read=[](int*x,int*y){if(mode==0){if(pointerDown){*x=pointerX;*y=pointerY;return 1;}return 0;}if(mode==2){*x=pointerX;*y=pointerY;return 0;}return 0;};
 svc.input_mouse_read=[](int*x,int*y,int*b){if(!mouseReport)return 0;*x=pendingDX;*y=pendingDY;*b=pointerButtons;pendingDX=pendingDY=0;mouseReport=false;return 1;};
 svc.hardware_read=[](const char*name,float*out){if(!present||!samples.count(name))return -1;*out=samples.at(name);return 0;};
 svc.hardware_write=[](const char*name,float value){assert(present&&samples.count(name));writes.emplace_back(name,value);samples[name]=value;if(std::string(name)=="camera_capture")samples["camera_capture_status"]=1;return 0;};
 setvbuf(stdout,nullptr,_IONBF,0);
 for(int pass=0;pass<2;pass++) {
  present=pass==1;samples=initial;writes.clear();frames=audioFrames=0;audioEnergy=0;
  assert(Scratch::initializeRuntime());assert(pax_resize_surface(1024,600));Unzip::filePath=argv[1];assert(Unzip::load());pax_update_project_clock();Scratch::initializeScratchProject();ScriptThread monitor;pax_project_playing=true;
  auto run=[&](int ms){auto end=clockUs()+ms*1000;while(clockUs()<end){pax_update_project_clock();auto result=Scratch::stepScratchProject(monitor);assert(result.first);}};
  auto click=[&](const char*name){Sprite*found=nullptr;for(auto s:Scratch::sprites)if(s->name==name)found=s;assert(found);
    int x=std::lround(PAX_W/2.f+found->xPosition*Render::renderScale),y=std::lround(PAX_H/2.f-found->yPosition*Render::renderScale);
    auto old=Input::getTouchPosition();pointerX=x;pointerY=y;pendingDX=x-old[0];pendingDY=y-old[1];
    pointerDown=true;pointerButtons=mode==0?0:1;mouseReport=mode!=0;Input::getInput();
    assert(Input::mousePointer.heldFrames==1);run(75);
    pointerDown=false;pointerButtons=0;pendingDX=pendingDY=0;mouseReport=mode!=0;Input::getInput();run(75);
  };
  auto val=[&](const char*id){return BlockExecutor::getVariableValue(id,Scratch::stageSprite).asString();};
  run(800);
  assert(Scratch::sprites.size()==11);assert(writes.empty());assert(audioEnergy==0);
  puts("PASS loading starts sensor reads without activating outputs or playing audio");
  for(int page=1;page<=7;page++){
   printf("PAGE %d %s\n",page,val("hw_title").c_str());
   auto &items=Scratch::stageSprite->lists.at("hw_readings").items;assert(items.size()==8);
   for(auto&item:items)printf("  %s\n",item.asString().c_str());
   if(page==1)assert(items[0].asString()==(present?"Temperature: 23.5 C":"Temperature: unavailable"));
   Render::hasFrameBegan=false;Render::beginFrame(0,0,0,0);Render::renderSprites();Render::renderMonitors();Render::endFrame();
   std::filesystem::copy_file("/tmp/parity-progress.ppm","/tmp/pax-fun-page-"+std::to_string(pass)+"-"+std::to_string(page)+".ppm",std::filesystem::copy_options::overwrite_existing);
   if(page<7){click("Next");run(800);}
  }
  // Clicking and holding Next must advance only once; all pointer backends.
  for(int m=0;m<3;m++){mode=m;int before=std::stoi(val("hw_page"));click("Next");run(700);assert(std::stoi(val("hw_page"))==before%7+1);printf("PASS actual pointer dispatch mode=%d (touch, relative USB, desktop absolute)\n",m);}
  // A device mouse gets one visible pointer; desktop keeps its OS pointer.
  mode=1;auto oldPointer=Input::getTouchPosition();pendingDX=800-oldPointer[0];pendingDY=420-oldPointer[1];pointerButtons=0;mouseReport=true;Input::getInput();
  Render::hasFrameBegan=false;Render::beginFrame(0,0,0,0);Render::endFrame();auto mouse=Input::getTouchPosition();assert(pax_pixels[mouse[1]*PAX_W+mouse[0]]==0xffff);
  mode=2;pointerX=800;pointerY=420;Input::getInput();Render::beginFrame(0,0,0,0);Render::endFrame();assert(pax_pixels[420*PAX_W+800]==0);
  puts("PASS device cursor visible and duplicate desktop cursor suppressed");
  mode=0;
  // Queued key down/up and a second adjacent tap both dispatch; no text duplication.
  static int keyEdge=0;
  svc.input_keyboard_read=[](pax_keyboard_event_t*e){if(keyEdge>=2)return 0;*e={32,keyEdge++==0};return 1;};
  for(int k=0;k<2;k++){keyEdge=0;int before=std::stoi(val("hw_page"));Input::getInput();assert(std::count(Input::inputBuffer.begin(),Input::inputBuffer.end(),"space")==1);for(int step=0;step<10;step++)BlockExecutor::runThreads();assert(std::stoi(val("hw_page"))==before%7+1);}
  svc.input_keyboard_read=noKey;Input::getInput();puts("PASS separate queued Space taps and unduplicated keyboard text");
  click("PAX Sprite");run(300);
  Sprite*face=nullptr;for(auto s:Scratch::sprites)if(s->name=="PAX Sprite")face=s;
  // Native speech contents are additionally checked in the exported graph.
  assert(face);
  for(auto name:{"LED","Relay","Servo","Sound","Mic","Display","Camera"}){
   // Check the 500 ms relay while it is still active, allowing for slow frames.
   click(name);run(50);
   if(present&&(std::string(name)=="Relay"||std::string(name)=="Servo"||std::string(name)=="Mic")){
    assert(val("hw_busy")=="1");click(name);run(50); // repeat must not cancel cleanup
   }
   auto deadline=clockUs()+12000000;
   while((val("hw_busy")!="0"||val("hw_request")!="0")&&clockUs()<deadline)run(50);
   assert(val("hw_busy")=="0"&&val("hw_request")=="0");
   printf("PASS %s: %s\n",name,val("hw_action").c_str());
  }
  if(!present){assert(writes.empty());puts("PASS absent hardware stays unavailable and performs no writes");}
  else {
   for(auto c:{"relay","servo","led_red","led_green","led_blue","led_white","led_brightness","microphone_enabled","speaker_enabled"})assert(samples[c]==0);
   assert(samples["display_brightness"]==80);assert(samples["camera_capture"]==1&&samples["camera_capture_status"]==1);assert(audioEnergy>0);
   bool r=false,g=false,b=false,w=false;for(auto &v:writes){if(v.first=="led_red"&&v.second==40)r=true;if(v.first=="led_green"&&v.second==40)g=true;if(v.first=="led_blue"&&v.second==40)b=true;if(v.first=="led_white"&&v.second==40)w=true;}
   assert(r&&g&&b&&w);printf("PASS %zu queued writes, RGBW colors, neutral/off/restored output values, camera status and audible PCM\n",writes.size());
  }
  Scratch::cleanupScratchProject();SoundPlayer::deinit();Render::deInit();OS::deinit();pax_project_playing=false;OS::toExit=false;
 }
 puts("PASS real Scratch interpreter with actual touch/mouse/key dispatch: missing/full bridge, seven pages, rounding, timed actions and repeated-click cleanup");return 0;
}
