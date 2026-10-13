#include "pax_port.h"
#include <runtime.hpp>
#include <unzip.hpp>
#include <render.hpp>
#include <input.hpp>
#include <filesystem.hpp>
#include <audio.hpp>
#include <setjmp.h>
#include <atomic>
#include <cstdlib>
#include <settings.hpp>
const app_services_t *pax_svc;
static jmp_buf quitJmp;
static std::atomic<bool> done(false);
static int result;
extern "C" long long pax_time_us(){return pax_svc ? pax_svc->get_time_us() : 0;}
extern "C" void pax_quit(int code){result=code;longjmp(quitJmp,1);}
static std::string chooseProject(){
 auto listed=FileSystem::listDirectory(OS::getScratchFolderLocation());std::vector<std::string> files;
 if(listed)for(auto f:*listed)if(f.size()>4&&f.substr(f.size()-4)==".sb3")files.push_back(f);
 int selected=0;bool wasA=true,wasUp=false,wasDown=false,wasTouch=true;
 while(!OS::toExit){
  Input::getInput();pax_gamepad_state_t pad{};pax_svc->input_gamepad_read(&pad);
  bool up=pad.values[PAX_INPUT_UP],down=pad.values[PAX_INPUT_DOWN];
  if(!files.empty()){if(up&&!wasUp)selected=(selected+(int)files.size()-1)%files.size();if(down&&!wasDown)selected=(selected+1)%files.size();}
  bool a=pad.values[PAX_INPUT_A]||pad.values[PAX_INPUT_START]||std::find(Input::inputKeys.begin(),Input::inputKeys.end(),"enter")!=Input::inputKeys.end();
  if(a&&!wasA&&!files.empty())return files[selected];
  bool touch=Input::mousePointer.isPressed;
  if(touch&&!wasTouch){int row=(Input::mousePointer.y-85)/30;int first=(selected/7)*7;if(Input::mousePointer.y>=85&&row>=0&&row<7&&first+row<(int)files.size())return files[first+row];}
  wasA=a;wasUp=up;wasDown=down;wasTouch=touch;
  Render::beginFrame(0,20,25,40);
  pax_draw_text("Scratch Everywhere!",PAX_W/2,25,0xffffffff,1.5,true);
  pax_draw_text("Select a project",PAX_W/2,60,0xc8d4e8ff,1,true);
  int first=(selected/7)*7;for(int i=first;i<(int)files.size()&&i<first+7;i++){int y=85+(i-first)*30;if(i==selected)Render::drawBox(440,28,240,y+10,45,70,110);pax_draw_text(files[i].substr(0,35),25,y,0xffffffff,1,false);}
  if(files.empty()){pax_draw_text("Copy .sb3 files to",240,130,0xffffffff,1,true);pax_draw_text("/sd/roms/scratch/projects/",240,160,0xffffffff,1,true);}
  pax_draw_text("Touch or A: play   Hold Menu: exit",240,325,0xc8d4e8ff,.9,true);
  Render::endFrame();pax_svc->delay_ms(15);
 }
 return "";
}
static int run(){
 FileSystem::createDirectory(OS::getScratchFolderLocation());FileSystem::createDirectory(OS::getConfigFolderLocation());
 if(!FileSystem::fileExists(OS::getRomFSLocation()+"gfx/translations/languages.json")){pax_svc->log_printf("Scratch: missing data; install scratch-data.zip to /sd/roms/scratch/\n");return -2;}
 if(!Scratch::initializeRuntime())return -1;
 while(!OS::toExit){auto project=chooseProject();if(project.empty())break;
  Unzip::filePath=OS::getScratchFolderLocation()+project;
  Render::beginFrame(0,20,25,40);pax_draw_text("Loading " + project,240,150,0xffffffff,1,true);Render::endFrame();
  if(!Unzip::load()){Scratch::cleanupScratchProject();continue;}
  // Default to familiar arrows/space while honoring per-project control settings.
  if(!SettingsManager::getProjectSettings(project.substr(0,project.size()-4)).contains("controls")){Input::inputControls["dpadUp"]="up arrow";Input::inputControls["dpadDown"]="down arrow";Input::inputControls["dpadLeft"]="left arrow";Input::inputControls["dpadRight"]="right arrow";Input::inputControls["A"]="space";}
  Scratch::startScratchProject();
 }
 SoundPlayer::deinit();Render::deInit();OS::deinit();return 0;
}
#ifndef PAX_HOST_TEST
extern "C" { typedef void (*init_fn)(); extern init_fn __init_array_start[],__init_array_end[]; }
static void appTask(void *){
 if(setjmp(quitJmp)==0){for(auto fn=__init_array_start;fn<__init_array_end;fn++)(*fn)();result=run();}
 pax_close_all_files();pax_free_all_memory();done.store(true,std::memory_order_release);
 for(;;)pax_svc->delay_ms(1000);
}
extern "C" __attribute__((section(".text.entry"),used)) int app_entry(const app_services_t *svc){
 pax_svc=svc;if(!svc||svc->abi_version!=PAX_ABI_VERSION||!svc->file_stat||!svc->file_list_dir)return -1;
 done.store(false);result=0;void *handle=nullptr;
 svc->log_printf("Scratch Everywhere! PAX starting\n");
 if(svc->task_create(appTask,"scratch",256*1024,nullptr,3,&handle,0)!=0)return -1;
 while(!done.load(std::memory_order_acquire))svc->delay_ms(50);
 svc->task_delete(handle);return result;
}
#else
int pax_host_run(){if(setjmp(quitJmp)==0)result=run();return result;}
#endif
