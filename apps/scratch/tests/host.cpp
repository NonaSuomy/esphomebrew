#include "pax_port.h"
#include <miniz.h>
#include <runtime.hpp>
#include <render.hpp>
#include <unzip.hpp>
#include <input.hpp>
#include <audio.hpp>
#include <audiostack.hpp>
#include <filesystem>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
static std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
static int frames=0,audioFrames=0;static long long audioEnergy=0;static bool menu=false;static bool uiTest=false;static bool keyboardUiTest=false;
static void delay(int n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
static int64_t clockUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();}
static int statFile(const char *p,pax_file_stat_t *o){std::error_code ec;auto st=std::filesystem::status(p,ec);if(ec||!std::filesystem::exists(st))return -1;o->is_dir=std::filesystem::is_directory(st);o->size=o->is_dir?0:std::filesystem::file_size(p);o->mtime=0;return 0;}
static int mkdirFile(const char *p){std::error_code ec;std::filesystem::create_directories(p,ec);return ec?-1:0;}
static void pad(pax_gamepad_state_t *p){memset(p,0,sizeof(*p));p->values[PAX_INPUT_MENU]=menu||(uiTest&&frames>35);if(uiTest&&!keyboardUiTest&&frames==1)p->values[PAX_INPUT_A]=1;}
static void frame(const uint16_t *p,uint16_t w,uint16_t h,float,bool){assert((w==480&&h==360)||(w==800&&h==480)||(w==1024&&h==600));frames++;FILE *f=fopen("/tmp/pax-scratch-test.ppm","wb");fprintf(f,"P6\n%d %d\n255\n",w,h);for(int i=0;i<w*h;i++){unsigned char rgb[]={static_cast<unsigned char>(((p[i]>>11)&31)*255/31),static_cast<unsigned char>(((p[i]>>5)&63)*255/63),static_cast<unsigned char>((p[i]&31)*255/31)};fwrite(rgb,1,3,f);}fclose(f);}
static void submit(short *p,int n){audioFrames+=n;for(int i=0;i<n*2;i++)audioEnergy+=abs(p[i]);}
static int noTouch(int*,int*){return 0;}
static int noKey(pax_keyboard_event_t*){return 0;}
extern int pax_host_run();
void testEditor();
void testProjectPicker(app_services_t&);
void testCloud();
static std::string rowPngFixture(int w,int h,int channels,int solid=0,bool badAdler=false) {
 std::vector<unsigned char> raw;std::vector<unsigned char> previous(w*channels),row(w*channels);
 auto paeth=[](int a,int b,int c){int p=a+b-c,pa=abs(p-a),pb=abs(p-b),pc=abs(p-c);return pa<=pb&&pa<=pc?a:pb<=pc?b:c;};
 for(int y=0;y<h;y++){for(int x=0;x<w;x++){row[x*channels]=solid==2?x/80:solid?17:x%256;row[x*channels+1]=solid==2?y/20:solid?31:y%256;row[x*channels+2]=solid==2?(x/80+y/20)%256:solid?63:(x+y)%256;if(channels==4)row[x*channels+3]=solid==2?x/160:solid?128:(x*3+y)%256;}
  int filter=y%5;raw.push_back(filter);for(int x=0;x<w*channels;x++){int a=x>=channels?row[x-channels]:0,b=previous[x],c=x>=channels?previous[x-channels]:0;int predicted=filter==1?a:filter==2?b:filter==3?(a+b)/2:filter==4?paeth(a,b,c):0;raw.push_back((unsigned char)(row[x]-predicted));}previous=row;
 }
 mz_ulong n=mz_compressBound(raw.size());std::string compressed(n,'\0');assert(mz_compress2((unsigned char*)compressed.data(),&n,raw.data(),raw.size(),6)==MZ_OK);compressed.resize(n);if(badAdler)compressed.back()^=1;
 auto u32=[](std::string &v,uint32_t x){for(int shift:{24,16,8,0})v.push_back((x>>shift)&255);};
 std::string out("\x89PNG\r\n\x1a\n",8);
 auto chunk=[&](const char *type,const std::string &data){u32(out,data.size());size_t at=out.size();out.append(type,4);out+=data;u32(out,mz_crc32(0,(const unsigned char*)out.data()+at,data.size()+4));};
 std::string header;u32(header,w);u32(header,h);header.push_back(8);header.push_back(channels==4?6:2);header.append(3,'\0');chunk("IHDR",header);
 chunk("IDAT",compressed.substr(0,compressed.size()/2));chunk("IDAT",compressed.substr(compressed.size()/2));chunk("IEND","");return out;
}
int main(){
 setvbuf(stdout,nullptr,_IONBF,0);
 // Color storage must retain precision across every union lifetime transition.
 { Color color{21.125f,63.5f,87.25f,12.75f};Value a(color),b(a),c(std::move(b));
   assert(a==c && a.asDouble()==c.asDouble() && a.asString()==c.asString());
   Value number(3.25);number=c;assert(number==a);number=number;assert(number==a);
   Value text(std::string("old"));text=std::move(number);assert(text==a);
   Value different(Color{21.25f,63.5f,87.25f,12.75f});assert(!(a==different));
   a=Value(false);c=Value(7.0);text=Value(std::string("done"));
   assert(!a.asBoolean() && c.asDouble()==7 && text.asString()=="done");
   std::vector<Value> mixed;for(int i=0;i<1000;i++){mixed.push_back(Value(color));mixed.push_back(Value(i));}
   for(int i=0;i<1000;i++){assert(mixed[i*2]==Value(color));assert(mixed[i*2+1].asDouble()==i);}
 }
 puts("PASS shared color values preserve equality/conversions and union copy/move/destruction");

 app_services_t svc{};svc.abi_version=1;svc.log_printf=printf;svc.log_vprintf=vprintf;svc.delay_ms=delay;svc.get_time_us=clockUs;svc.mem_alloc=malloc;svc.mem_calloc=calloc;svc.mem_realloc=realloc;svc.mem_free=free;svc.mem_caps_alloc=[](size_t n,uint32_t)->void*{return malloc(n);};svc.file_stat=statFile;svc.file_mkdir=mkdirFile;svc.input_gamepad_read=pad;svc.touch_read=noTouch;svc.input_keyboard_read=noKey;svc.display_write_frame_custom=frame;svc.display_get_size=[](int*w,int*h){*w=480;*h=360;};svc.audio_init=[](int rate){assert(rate==48000);};svc.audio_submit=submit;
 svc.file_open=[](const char*p,const char*m)->void*{return fopen(p,m);};
 svc.file_close=[](void*f){return fclose((FILE*)f);};
 svc.file_seek=[](void*f,long n,int whence){return fseek((FILE*)f,n,whence);};
 svc.file_read=[](void*b,size_t n,size_t c,void*f){return fread(b,n,c,(FILE*)f);};
 pax_svc=&svc;
 testCloud();
 { Sprite stage;auto previous=Scratch::stageSprite;Scratch::stageSprite=&stage;
   auto &list=stage.lists["clear-test"];list.id="clear-test";list.name="test";
   Block block;block.fields.push_back({"LIST",ParsedField{"test","clear-test"}});block.fieldMap["LIST"]=&block.fields.back().second;
   ScriptThread thread;auto &handler=BlockExecutor::getHandlers().at("data_deletealloflist");
   list.items.resize(8192,Value(42));assert(handler(&block,&thread,&stage,nullptr)==BlockResult::CONTINUE);
   assert(list.items.empty() && list.items.capacity()==0);
   list.items.emplace_back(3);handler(&block,&thread,&stage,nullptr);assert(list.items.empty());
   list.items.emplace_back(std::string("preserved"));assert(list.items[0].asString()=="preserved");
   Scratch::stageSprite=previous;
 }
 puts("PASS delete-all releases large list capacity and allows reuse");
 // Test the real bundled font and its grayscale coverage, not the fallback.
 auto fontPath=std::filesystem::path(OS::getRomFSLocation())/"gfx/ingame/fonts/NotoSans-Medium.ttf";
 std::filesystem::create_directories(fontPath.parent_path());
 std::filesystem::copy_file(std::filesystem::path(__FILE__).parent_path().parent_path()/"core/gfx/ingame/fonts/NotoSans-Medium.ttf",fontPath,std::filesystem::copy_options::overwrite_existing);
 assert(pax_resize_surface(1024,600));Render::beginFrame(0,255,255,255);
 float fw,fh;assert(pax_font_measure("output",.833333f,fw,fh));assert(fw>20&&fh==20);
 pax_draw_text("output",512,160,0x000000ff,.833333f,true);
 int gray=0;for(int i=0;i<1024*600;i++)if(pax_pixels[i]!=0xffff&&pax_pixels[i]!=0)++gray;
 assert(gray>30);Render::hasFrameBegan=false;assert(pax_resize_surface(480,360));
 puts("PASS native-resolution Noto Sans text has antialiased glyph coverage");
 assert(pax_resize_surface(1024,600));
 for(const char* label:{"1","[PASSED] gpy%","42"}) {
  Render::hasFrameBegan=false;Render::beginFrame(0,0,0,0);
  pax_draw_text(label,512,pax_text_center_y(label,.833333f,200.5f),0xffffffff,.833333f,true);
  int top=999,bottom=-1;for(int y=180;y<222;y++)for(int x=400;x<624;x++)if(pax_pixels[y*1024+x]){top=std::min(top,y);bottom=std::max(bottom,y);}
  assert(bottom>=top&&std::abs((top+bottom)/2.f-200.5f)<=1);
 }
 Render::hasFrameBegan=false;assert(pax_resize_surface(480,360));
 puts("PASS list row numbers and text share the ink-bounds center, including descenders");

 // A simulated clock checks that turbo batches work while yielding regularly.
 static int64_t simClock=1000000;static int yieldCalls=0;
 svc.get_time_us=[]()->int64_t{return simClock+=100;};svc.delay_ms=[](int){++yieldCalls;};
 pax_project_playing=true;Scratch::turbo=false;
 for(int i=0;i<100;i++)assert(Render::appShouldRun());assert(yieldCalls==100);
 yieldCalls=0;Scratch::turbo=true;
 for(int i=0;i<100;i++)assert(Render::appShouldRun());assert(yieldCalls>0&&yieldCalls<10);
 printf("PASS turbo timed slices: normal=100 yields, turbo=%d yields for equal work\n",yieldCalls);
 // A slow frame must be followed by an interpreter work interval.
 simClock+=80000;pax_turbo_frame_finish();assert(!pax_turbo_frame_ready());
 simClock+=34000;assert(pax_turbo_frame_ready());
 puts("PASS turbo reserves work time after a slow LCD redraw");

 pax_project_playing=false;Scratch::turbo=false;svc.get_time_us=clockUs;svc.delay_ms=delay;


 // Touch coordinates use the scaled and centered Scratch viewport.
 svc.display_get_size=[](int*w,int*h){*w=800;*h=480;};svc.touch_read=[](int*x,int*y){*x=240;*y=240;return 1;};Input::getInput();assert(Input::mousePointer.x==120&&Input::mousePointer.y==180&&Input::mousePointer.isPressed);
 svc.touch_read=[](int*x,int*y){*x=10;*y=240;return 1;};Input::getInput();assert(!Input::mousePointer.isPressed);
 svc.touch_read=noTouch;svc.display_get_size=[](int*w,int*h){*w=480;*h=360;};
 static int report=0;svc.input_mouse_read=[](int*dx,int*dy,int*b){*dx=*dy=0;if(report==0){*b=1;report++;return 1;}if(report==2){*b=0;report++;return 1;}return 0;};Input::getInput();assert(Input::mousePointer.isPressed);Input::getInput();assert(Input::mousePointer.isPressed);report=2;Input::getInput();assert(!Input::mousePointer.isPressed);
 puts("PASS letterboxed touch and held USB mouse input");
 // Desktop absolute hover remains aligned after leaving/re-entering and
 // across the editor/player surfaces; the OS cursor replaces the software one.
 assert(pax_resize_surface(1024,600));
 svc.display_get_size=[](int*w,int*h){*w=1024;*h=600;};
 svc.touch_read=[](int*x,int*y){*x=440;*y=110;return 0;};
 svc.input_mouse_read=[](int*dx,int*dy,int*b){*dx=75;*dy=40;*b=0;return 1;};
 Input::getInput();assert(pax_native_cursor&&Input::mousePointer.x==440&&Input::mousePointer.y==110&&!Input::mousePointer.isPressed);
 assert(pax_resize_surface(480,360));svc.touch_read=[](int*x,int*y){*x=240;*y=300;return 0;};
 Input::getInput();assert(Input::mousePointer.x==76&&Input::mousePointer.y==180);
 assert(pax_resize_surface(1024,600));svc.touch_read=[](int*x,int*y){*x=975;*y=25;return 1;};
 Input::getInput();assert(Input::mousePointer.x==975&&Input::mousePointer.y==25&&Input::mousePointer.isPressed);
 svc.touch_read=noTouch;svc.input_mouse_read=nullptr;svc.display_get_size=[](int*w,int*h){*w=480;*h=360;};
 assert(pax_resize_surface(480,360));Input::getInput();assert(!pax_native_cursor);
 puts("PASS absolute desktop hover, canvas transitions and native cursor selection");
 assert(pax_resize_surface(1024,600));svc.display_get_size=[](int*w,int*h){*w=1024;*h=600;};
 svc.touch_read=[](int*x,int*y){*x=512;*y=300;return 0;};pax_project_playing=true;Render::renderScale=600.f/360;
 Input::getInput();assert(Input::mousePointer.x==0&&Input::mousePointer.y==0);assert(Input::getTouchPosition()[0]==512);
 svc.touch_read=[](int*x,int*y){*x=112;*y=0;return 0;};Input::getInput();assert(Input::mousePointer.x==-240&&Input::mousePointer.y==180);
 pax_project_playing=false;svc.touch_read=noTouch;svc.display_get_size=[](int*w,int*h){*w=480;*h=360;};Render::renderScale=1;assert(pax_resize_surface(480,360));
 puts("PASS native-resolution playback mouse maps into Scratch stage coordinates");

 // A quick F6 tap must stop a builder preview even if both edges are queued.
 static int stopKeyEdge=0;svc.input_keyboard_read=[](pax_keyboard_event_t *e){if(stopKeyEdge==2)return 0;*e={140,stopKeyEdge++==0};return 1;};
 pax_editor_playing=true;Scratch::shouldStop=false;Input::getInput();assert(Scratch::shouldStop);
 pax_editor_playing=false;Scratch::shouldStop=false;stopKeyEdge=0;Input::getInput();assert(!Scratch::shouldStop);
 svc.input_keyboard_read=noKey;
 puts("PASS queued F6 press/release stops preview only");
 pax_project_playing=true;Scratch::turbo=false;
 static int turboEdge=0;svc.input_keyboard_read=[](pax_keyboard_event_t*e){if(turboEdge==2)return 0;*e={141,turboEdge++==0};return 1;};
 Input::getInput();assert(Scratch::turbo);Input::getInput();assert(Scratch::turbo);
 turboEdge=0;Input::getInput();assert(!Scratch::turbo);pax_project_playing=false;
 svc.input_keyboard_read=[](pax_keyboard_event_t*){return 0;};
 puts("PASS F7 toggles turbo once per press during playback");

 // Disconnected sockets must initialize their result, including Boolean sockets.
 Block emptyBlock;ScriptThread emptyThread;Sprite emptySprite;
 bool emptyBool=true;double emptyNumber=123;std::string emptyText="stale";Value emptyValue("stale");
 assert(Scratch::getInputValueAs(&emptyBlock,"OPERAND1",&emptyThread,&emptySprite,emptyBool)&&!emptyBool);
 assert(Scratch::getInputValueAs(&emptyBlock,"OPERAND1",&emptyThread,&emptySprite,emptyNumber)&&emptyNumber==0);
 assert(Scratch::getInputValueAs(&emptyBlock,"OPERAND1",&emptyThread,&emptySprite,emptyText)&&emptyText.empty());
 assert(Scratch::getInputValue(&emptyBlock,"OPERAND1",&emptyThread,&emptySprite,emptyValue)&&!emptyValue.asBoolean());
 puts("PASS disconnected Boolean, number, text and generic input defaults");
 // Project timer is stable within one Scratch execution step; wait timers
 // still measure real time. This reproduces both supplied SB3 timer checks.
 pax_update_project_clock();BlockExecutor::timer.start();Timer waitTimer(true);
 delay(25);assert(BlockExecutor::timer.getTimeMs()==0);assert(waitTimer.getTimeMs()>=20);
 pax_update_project_clock();assert(BlockExecutor::timer.getTimeMs()>=20);
 BlockExecutor::timer.start();assert(BlockExecutor::timer.getTimeMs()==0);
 puts("PASS Scratch frame clock reset/read and independent wait timers");
 static int64_t audioNow;static int silenceRemaining;
 svc.get_time_us=[](){return audioNow;};audioNow=1000000;assert(SoundPlayer::init());silenceRemaining=44100*6;
 auto silence=new SoundStream("timing-stall",[](SoundStream*,float *out,int count){int n=std::min(count,silenceRemaining);std::fill(out,out+n,0.f);silenceRemaining-=n;return n;},1,44100);Mixer::streams["timing-stall"]=silence;
 int elapsed=0;while(Mixer::isSoundPlaying("timing-stall")&&elapsed<9000){int advance=elapsed==250||elapsed==2000?1500:10;audioNow+=advance*1000;elapsed+=advance;pax_audio_pump();}
 assert(elapsed>=5900&&elapsed<=6100);assert(silenceRemaining==0);SoundPlayer::deinit();svc.get_time_us=clockUs;
 puts("PASS six-second sound remains within 100ms of wall time through 1.5-second interpreter stalls");

 // Real Scratch zero-viewport SVGs are valid blank costumes, not failures.
 const std::string blankSvg="<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"0\" height=\"0\" viewBox=\"0 0 0 0\"></svg>";
 auto writeSvg=[](const char *name,const std::string &data){auto path=OS::getRomFSLocation()+name;FILE *f=fopen(path.c_str(),"wb");assert(f);fwrite(data.data(),1,data.size(),f);fclose(f);};
 writeSvg("pax-empty.svg",blankSvg);
 {auto image=createImageFromFile("pax-empty.svg",false);assert(image);assert(image.value()->getAlphaAt(0,0)==0);assert(image.value()->resizeSVG(2));assert(image.value()->getAlphaAt(0,0)==0);}
 writeSvg("pax-solid.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\"><rect width=\"8\" height=\"8\" fill=\"red\"/></svg>");
 {auto image=createImageFromFile("pax-solid.svg",false);assert(image);assert(image.value()->getAlphaAt(4,4)==255);}
 writeSvg("pax-invalid.svg","not an svg");assert(!createImageFromFile("pax-invalid.svg",false));
 puts("PASS blank SVG load/resize, opaque SVG and invalid SVG error");
 const std::string alphaSvg=R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="23" height="17" viewBox="-5 -3 23 17"><defs><linearGradient id="a"><stop stop-color="red"/><stop offset="1" stop-color="blue" stop-opacity=".3"/></linearGradient></defs><ellipse cx="6" cy="5" rx="9" ry="7" fill="url(#a)" opacity=".7"/></svg>)SVG";
 writeSvg("pax-alpha.svg",alphaSvg);
 {auto loaded=createImageFromFile("pax-alpha.svg",false);assert(loaded);auto doc=lunasvg::Document::loadFromData(alphaSvg);
 for(int scale: {1,2,3}){assert(loaded.value()->resizeSVG(scale));auto reference=doc->renderToBitmap(23*scale,17*scale);auto *src=reference.data();auto *dst=static_cast<unsigned char*>(loaded.value()->getNativeTexture());assert(dst);
 for(int i=0;i<23*17*scale*scale;i++){unsigned a=src[i*4+3],r=src[i*4+2],g=src[i*4+1],b=src[i*4];if(a>0&&a<255){r=r*255/a;g=g*255/a;b=b*255/a;}assert(dst[i*4]==r&&dst[i*4+1]==g&&dst[i*4+2]==b&&dst[i*4+3]==a);}}
 }
 puts("PASS direct SVG rasterization and resize match every reference pixel, including antialiasing and alpha");
 for(int channels:{3,4}){auto png=rowPngFixture(600,129,channels);writeSvg("pax-rows.png",png);
  {auto loaded=createImageFromFile("pax-rows.png",false);assert(loaded);auto pixels=loaded.value()->getPixels();assert(pixels.scanlines);assert(pixels.width==600&&pixels.height==129);
   for(int y=0;y<129;y++)for(int x=0;x<600;x++){auto row=pixels.rowData(y);assert(row[x*4]==x%256&&row[x*4+1]==y%256&&row[x*4+2]==(x+y)%256&&row[x*4+3]==(channels==4?(x*3+y)%256:255));assert(loaded.value()->getAlphaAt(x,y)==row[x*4+3]);}
   auto sub=loaded.value()->getPixels({17,9,4,5});assert(sub.rowData(2)[0]==17&&sub.rowData(2)[1]==11);
  }
  {auto loaded=createImageFromFile("pax-rows.png",false,true);assert(loaded);assert(loaded.value()->getWidth()==300&&loaded.value()->getHeight()==64);auto p=loaded.value()->getPixels();assert(p.rowData(3)[4]==2&&p.rowData(3)[5]==6);}
  png.back()^=1;writeSvg("pax-bad-crc.png",png);assert(!createImageFromFile("pax-bad-crc.png",false));
  writeSvg("pax-bad-adler.png",rowPngFixture(600,129,channels,false,true));assert(!createImageFromFile("pax-bad-adler.png",false));
  png.resize(png.size()-10);writeSvg("pax-truncated.png",png);assert(!createImageFromFile("pax-truncated.png",false));
 }
 writeSvg("pax-solid-rows.png",rowPngFixture(960,720,4,true));{auto loaded=createImageFromFile("pax-solid-rows.png",false);assert(loaded);auto p=loaded.value()->getPixels();assert(p.scanlines->rows.front()==p.scanlines->rows.back());assert(p.rowData(719)[3836]==17&&p.rowData(719)[3839]==128);}
 writeSvg("pax-striped-rows.png",rowPngFixture(960,720,4,2));
 {auto loaded=createImageFromFile("pax-striped-rows.png",false);assert(loaded);auto p=loaded.value()->getPixels();
  size_t bytes=0;for(auto &row:p.scanlines->rows)bytes+=row->size();assert(bytes<960*720*4/8);
  for(int y=719;y>=0;y--)for(int x=0;x<960;x++){auto row=p.rowData(y);assert(row[x*4]==x/80&&row[x*4+1]==y/20&&row[x*4+2]==(x/80+y/20)%256&&row[x*4+3]==x/160);}
  auto sub=loaded.value()->getPixels({240,100,160,40});assert(sub.rowData(25)[0]==3&&sub.rowData(25)[1]==6);
  assert(p.rowData(0)[0]==0&&p.rowData(719)[1]==35&&p.rowData(0)[0]==0);
  printf("PASS lossless striped RGBA rows use %zu bytes versus 2764800 raw; reverse-row and subrect reads preserve every pixel\n",bytes);
 }
 {SoundStream missing((ZipArchive*)nullptr,"pax-missing-audio.wav");assert(missing.error.has_value());}
 {SoundStream missing("/tmp/pax-missing-audio.wav",true,true);assert(missing.error.has_value());}
 puts("PASS failed ZIP and file sound loads remain safely destructible");
 auto largeSvg=alphaSvg;largeSvg.replace(largeSvg.find("width=\"23\""),10,"width=\"600\"");largeSvg.replace(largeSvg.find("height=\"17\""),11,"height=\"400\"");writeSvg("pax-large.svg",largeSvg);
 {auto loaded=createImageFromFile("pax-large.svg",false);assert(loaded);auto doc=lunasvg::Document::loadFromData(largeSvg);for(int scale:{1,2}){assert(loaded.value()->resizeSVG(scale));auto ref=doc->renderToBitmap(600*scale,400*scale);auto p=loaded.value()->getPixels();assert(p.scanlines);int maxAlpha=0,maxComposite=0;
 for(int y=0;y<400*scale;y++)for(int x=0;x<600*scale;x++){auto src=ref.data()+(y*600*scale+x)*4;auto dst=p.rowData(y)+x*4;int a=src[3],r=src[2],g=src[1],b=src[0];if(a>0&&a<255){r=r*255/a;g=g*255/a;b=b*255/a;}maxAlpha=std::max(maxAlpha,abs(int(dst[3])-a));int expected[]={r,g,b};for(int c=0;c<3;c++)for(int bg:{0,255}){int actual=(int(dst[c])*dst[3]+bg*(255-dst[3]))/255;int reference=(expected[c]*a+bg*(255-a))/255;maxComposite=std::max(maxComposite,abs(actual-reference));}}
 printf("SVG bands scale=%d maximum alpha difference=%d composited channel difference=%d/255\n",scale,maxAlpha,maxComposite);assert(maxAlpha<=2&&maxComposite<=3);}}
 puts("PASS large PNG scanlines preserve RGB/RGBA, five filters, split IDAT, subrects and half quality; CRC, Adler and truncation are rejected");
 puts("PASS identical PNG rows share exact pixels; large SVG bands and resize retain antialiasing within bounded rasterizer rounding");


 assert(Scratch::initializeRuntime());
 Unzip::filePath=OS::getScratchFolderLocation()+"PAX Demo.sb3";assert(Unzip::load());assert(Scratch::sprites.size()==3);assert(!Scratch::sb3InRam);
 Scratch::initializeScratchProject();ScriptThread monitor;
 for(int i=0;i<90;i++){auto status=Scratch::stepScratchProject(monitor);assert(status.first);delay(5);}
 Sprite *sprite=nullptr;for(auto s:Scratch::sprites)if(s->name=="PAX Sprite")sprite=s;
 assert(sprite);double ticks=BlockExecutor::getVariableValue("ticks",sprite).get<double>();assert(ticks>5);assert(sprite->xPosition>-120);assert(frames>5);assert(audioFrames>1000);assert(audioEnergy>0);
 assert(pax_pixels[250*480+390]==0x261f);
 int black=0;int faceX=(int)sprite->renderInfo.renderX,faceY=(int)sprite->renderInfo.renderY;for(int y=std::max(0,faceY-30);y<std::min(360,faceY+30);y++)for(int x=std::max(0,faceX-30);x<std::min(480,faceX+30);x++)if(pax_pixels[y*480+x]==0)black++;assert(black>10); // SVG face follows the moving sprite, independent of timer speed.
 printf("PASS runtime: ticks=%g x=%g frames=%d audio=%d energy=%lld\n",ticks,sprite->xPosition,frames,audioFrames,audioEnergy);
 // Hidden pen stamps must reload an evicted costume and still draw.
 sprite->visible=false;const auto stampedName=sprite->costumes[sprite->currentCostume].fullName;
 Scratch::loadCurrentCostumeImage(Scratch::stageSprite);assert(!Scratch::costumeImages.count(stampedName));
 Render::penClear();
 // A fragmented heap can service a scanline even when a full layer would fail.
 static size_t largestPenRequest=0;static bool rejectPenRow=true;
 auto normalAlloc=svc.mem_caps_alloc;
 svc.mem_caps_alloc=[](size_t n,uint32_t)->void*{largestPenRequest=std::max(largestPenRequest,n);return rejectPenRow?nullptr:malloc(n);};
 Render::penStamp(sprite);assert(!Render::initPen());Render::penClear();rejectPenRow=false;
 Render::penStamp(sprite);assert(largestPenRequest<=480*4);svc.mem_caps_alloc=normalAlloc;
 assert(!sprite->visible);assert(Scratch::costumeImages.count(stampedName));
 Render::beginFrame(0,0,0,0);Render::renderPenLayer();int stampedPixels=0;for(int i=0;i<480*360;i++)if(pax_pixels[i])stampedPixels++;assert(stampedPixels>20);Render::endFrame();
 sprite->visible=true;Render::penClear();puts("PASS hidden stamp reloads evicted pixels, draws, and preserves sprite visibility");puts("PASS pen uses scanline allocations and recovers after row allocation failure");
 menu=true;Input::getInput();delay(2050);Input::getInput();assert(OS::toExit);
 Scratch::cleanupScratchProject();SoundPlayer::deinit();Render::deInit();OS::deinit();
 // Re-open to exercise runtime cleanup/reinitialization.
 OS::toExit=false;assert(Scratch::initializeRuntime());Unzip::filePath=OS::getScratchFolderLocation()+"PAX Demo.sb3";assert(Unzip::load());Scratch::cleanupScratchProject();SoundPlayer::deinit();Render::deInit();OS::deinit();
 puts("PASS exit and project reload");
 svc.file_list_dir=[](const char *path,char *buf,int len){auto f=std::string(path)+"PAX Demo.sb3";if(!std::filesystem::exists(f))return -1;const char names[]="PAX Demo.sb3\0";if(len<(int)sizeof(names))return -1;memcpy(buf,names,sizeof(names));return (int)sizeof(names);};
 menu=false;OS::toExit=false;testEditor();menu=false;uiTest=true;frames=0;OS::toExit=false;assert(pax_host_run()==0);assert(frames>35);puts("PASS project browser and full app lifecycle");// Keyboard taps can be pressed and released in a single input poll.
 std::filesystem::copy_file(OS::getScratchFolderLocation()+"PAX Demo.sb3",OS::getScratchFolderLocation()+"Z Keyboard Demo.sb3",std::filesystem::copy_options::overwrite_existing);
 svc.file_list_dir=[](const char*,char *buf,int len){const char names[]="PAX Demo.sb3\0Z Keyboard Demo.sb3\0";assert(len>=(int)sizeof(names));memcpy(buf,names,sizeof(names));return (int)sizeof(names);};
 static int keyStage=0;
 svc.input_keyboard_read=[](pax_keyboard_event_t *e){if(keyStage==0&&frames>=1){*e={PAX_KEY_DOWN,1};keyStage++;return 1;}if(keyStage==1){*e={PAX_KEY_DOWN,0};keyStage++;return 1;}if(keyStage==2&&frames>=2){*e={PAX_KEY_ENTER,1};keyStage++;return 1;}if(keyStage==3){*e={PAX_KEY_ENTER,0};keyStage++;return 1;}return 0;};
 keyboardUiTest=true;frames=0;OS::toExit=false;assert(pax_host_run()==0);assert(Unzip::filePath.find("Z Keyboard Demo.sb3")!=std::string::npos);assert(keyStage==4);puts("PASS keyboard arrows and short Enter tap select the second project");
 testProjectPicker(svc);
 return 0;
}
