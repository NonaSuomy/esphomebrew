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
static std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
static int frames=0,audioFrames=0;static long long audioEnergy=0;static bool menu=false;static bool uiTest=false;
static void delay(int n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
static int64_t clockUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();}
static int statFile(const char *p,pax_file_stat_t *o){std::error_code ec;auto st=std::filesystem::status(p,ec);if(ec||!std::filesystem::exists(st))return -1;o->is_dir=std::filesystem::is_directory(st);o->size=o->is_dir?0:std::filesystem::file_size(p);o->mtime=0;return 0;}
static int mkdirFile(const char *p){std::error_code ec;std::filesystem::create_directories(p,ec);return ec?-1:0;}
static void pad(pax_gamepad_state_t *p){memset(p,0,sizeof(*p));p->values[PAX_INPUT_MENU]=menu||(uiTest&&frames>35);if(uiTest&&frames==1)p->values[PAX_INPUT_A]=1;}
static void frame(const uint16_t *p,uint16_t w,uint16_t h,float,bool){assert(w==480&&h==360);frames++;FILE *f=fopen("/tmp/pax-scratch-test.ppm","wb");fprintf(f,"P6\n%d %d\n255\n",w,h);for(int i=0;i<w*h;i++){unsigned char rgb[]={static_cast<unsigned char>(((p[i]>>11)&31)*255/31),static_cast<unsigned char>(((p[i]>>5)&63)*255/63),static_cast<unsigned char>((p[i]&31)*255/31)};fwrite(rgb,1,3,f);}fclose(f);}
static void submit(short *p,int n){audioFrames+=n;for(int i=0;i<n*2;i++)audioEnergy+=abs(p[i]);}
static int noTouch(int*,int*){return 0;}
static int noKey(pax_keyboard_event_t*){return 0;}
extern int pax_host_run();
int main(){
 app_services_t svc{};svc.abi_version=1;svc.log_printf=printf;svc.log_vprintf=vprintf;svc.delay_ms=delay;svc.get_time_us=clockUs;svc.mem_alloc=malloc;svc.mem_calloc=calloc;svc.mem_realloc=realloc;svc.mem_free=free;svc.mem_caps_alloc=[](size_t n,uint32_t)->void*{return malloc(n);};svc.file_stat=statFile;svc.file_mkdir=mkdirFile;svc.input_gamepad_read=pad;svc.touch_read=noTouch;svc.input_keyboard_read=noKey;svc.display_write_frame_custom=frame;svc.display_get_size=[](int*w,int*h){*w=480;*h=360;};svc.audio_init=[](int rate){assert(rate==48000);};svc.audio_submit=submit;
 pax_svc=&svc;

 // Touch coordinates use the scaled and centered Scratch viewport.
 svc.display_get_size=[](int*w,int*h){*w=800;*h=480;};svc.touch_read=[](int*x,int*y){*x=240;*y=240;return 1;};Input::getInput();assert(Input::mousePointer.x==120&&Input::mousePointer.y==180&&Input::mousePointer.isPressed);
 svc.touch_read=[](int*x,int*y){*x=10;*y=240;return 1;};Input::getInput();assert(!Input::mousePointer.isPressed);
 svc.touch_read=noTouch;svc.display_get_size=[](int*w,int*h){*w=480;*h=360;};
 static int report=0;svc.input_mouse_read=[](int*dx,int*dy,int*b){*dx=*dy=0;if(report==0){*b=1;report++;return 1;}if(report==2){*b=0;report++;return 1;}return 0;};Input::getInput();assert(Input::mousePointer.isPressed);Input::getInput();assert(Input::mousePointer.isPressed);report=2;Input::getInput();assert(!Input::mousePointer.isPressed);
 puts("PASS letterboxed touch and held USB mouse input");
 assert(Scratch::initializeRuntime());
 Unzip::filePath=OS::getScratchFolderLocation()+"PAX Demo.sb3";assert(Unzip::load());assert(Scratch::sprites.size()==3);assert(!Scratch::sb3InRam);
 Scratch::initializeScratchProject();ScriptThread monitor;
 for(int i=0;i<90;i++){auto status=Scratch::stepScratchProject(monitor);assert(status.first);delay(5);}
 Sprite *sprite=nullptr;for(auto s:Scratch::sprites)if(s->name=="PAX Sprite")sprite=s;
 assert(sprite);double ticks=BlockExecutor::getVariableValue("ticks",sprite).get<double>();assert(ticks>5);assert(sprite->xPosition>-120);assert(frames>5);assert(audioFrames>1000);assert(audioEnergy>0);
 assert(pax_pixels[250*480+390]==0x261f);
 int black=0;for(int y=100;y<160;y++)for(int x=200;x<300;x++)if(pax_pixels[y*480+x]==0)black++;assert(black>10); // Bitmap costume is rendered on stage.
 printf("PASS runtime: ticks=%g x=%g frames=%d audio=%d energy=%lld\n",ticks,sprite->xPosition,frames,audioFrames,audioEnergy);
 menu=true;Input::getInput();delay(2050);Input::getInput();assert(OS::toExit);
 Scratch::cleanupScratchProject();SoundPlayer::deinit();Render::deInit();OS::deinit();
 // Re-open to exercise runtime cleanup/reinitialization.
 OS::toExit=false;assert(Scratch::initializeRuntime());Unzip::filePath=OS::getScratchFolderLocation()+"PAX Demo.sb3";assert(Unzip::load());Scratch::cleanupScratchProject();SoundPlayer::deinit();Render::deInit();OS::deinit();
 puts("PASS exit and project reload");
 svc.file_list_dir=[](const char *path,char *buf,int len){auto f=std::string(path)+"PAX Demo.sb3";if(!std::filesystem::exists(f))return -1;const char names[]="PAX Demo.sb3\0";if(len<(int)sizeof(names))return -1;memcpy(buf,names,sizeof(names));return (int)sizeof(names);};
 menu=false;uiTest=true;frames=0;OS::toExit=false;assert(pax_host_run()==0);assert(frames>35);puts("PASS project browser and full app lifecycle");return 0;
}
