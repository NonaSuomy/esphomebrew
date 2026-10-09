#include "duke_sf2_midi.h"
#include "psram_app.h"
#include "../stratagus/pax_midi.h"
#include <cstring>
extern const app_services_t *_pax_svc;
static PaxMidi midi;
static bool paused;
static int volume=64;
static char font[128];
static int selectedFont;
static int preferMidi;
static const char *const choices[][2]={
  {"/sd/roms/duke3d/WeedsGM3.duke-fluid.sf2", "/sd/roms/duke3d/WeedsGM3.duke.sf2"},
  {"/sd/roms/duke3d/GeneralUser-GS.duke-fluid.sf2", nullptr}
};
static const char *const labels[]={"WEEDS GM3","GENERALUSER GS"};
static const char *available_font(int index) {
  if(index<0 || index>1)return nullptr;
  for(const char *path: choices[index]) {
    if(!path)continue;
    void *f=_pax_svc->file_open(path,"rb");if(!f)continue;
    _pax_svc->file_seek(f,0,SEEK_END);long size=_pax_svc->file_tell(f);
    _pax_svc->file_close(f);
    if(size>0 && size<=24*1024*1024)return path;
  }
  return nullptr;
}
extern "C" int duke_sf2_midi_selection() { return __atomic_load_n(&selectedFont,__ATOMIC_ACQUIRE); }
extern "C" const char *duke_sf2_midi_name() { return labels[duke_sf2_midi_selection()]; }
extern "C" int duke_sf2_midi_force_midi() { return __atomic_load_n(&preferMidi,__ATOMIC_ACQUIRE); }
extern "C" int duke_sf2_midi_select(int index) {
  if(!available_font(index))return 0;
  void *f=_pax_svc->file_open("/sd/roms/duke3d/soundfont.cfg","wb");
  const char value[]={char('0'+index),'\n'};
  bool saved=false;
  if(f) {
    saved=_pax_svc->file_write(value,1,sizeof(value),f)==sizeof(value);
    if(_pax_svc->file_close(f)!=0)saved=false;
  }
  __atomic_store_n(&selectedFont,index,__ATOMIC_RELEASE);
  __atomic_store_n(&preferMidi,1,__ATOMIC_RELEASE);
  _pax_svc->log_printf("DUKE FluidSynth: selected %s (%s)\n",labels[index],saved?"saved":"preference save failed");
  return 1;
}
static short pcm[1024];
struct BankStream { void *file; unsigned at,count; unsigned char bytes[4096]; };
static void *bank_open(const char *path) {
  auto *s=static_cast<BankStream *>(_pax_svc->mem_calloc(1,sizeof(BankStream)));
  if(!s)return nullptr;
  s->file=_pax_svc->file_open(path,"rb");
  if(!s->file){_pax_svc->mem_free(s);return nullptr;}
  return s;
}
static int bank_close(void *handle) {
  auto *s=static_cast<BankStream *>(handle);
  int result=_pax_svc->file_close(s->file);_pax_svc->mem_free(s);return result;
}
static int bank_read(void *out,fluid_long_long_t length,void *handle) {
  if(length<0 || length>0x7fffffff)return FLUID_FAILED;
  auto *s=static_cast<BankStream *>(handle);size_t got=0,size=size_t(length);
  while(got<size) {
    if(s->at==s->count) {
      s->at=s->count=0;
      if(size-got>=sizeof(s->bytes)) {
        got+=_pax_svc->file_read(static_cast<unsigned char *>(out)+got,1,size-got,s->file);break;
      }
      s->count=unsigned(_pax_svc->file_read(s->bytes,1,sizeof(s->bytes),s->file));
      if(!s->count)break;
    }
    size_t take=s->count-s->at;if(take>size-got)take=size-got;
    std::memcpy(static_cast<unsigned char *>(out)+got,s->bytes+s->at,take);
    got+=take;s->at+=unsigned(take);
  }
  return got==size?FLUID_OK:FLUID_FAILED;
}
static fluid_long_long_t bank_tell(void *handle) {
  auto *s=static_cast<BankStream *>(handle);long p=_pax_svc->file_tell(s->file);
  return p<0?-1:p-long(s->count-s->at);
}
static int bank_seek(void *handle,fluid_long_long_t offset,int whence) {
  auto *s=static_cast<BankStream *>(handle);
  if(offset < -0x7fffffff || offset > 0x7fffffff)return FLUID_FAILED;
  long o=long(offset);
  if(whence==SEEK_CUR && o>=-long(s->at) && o<=long(s->count-s->at)) {
    s->at=unsigned(long(s->at)+o);return FLUID_OK;
  }
  if(whence==SEEK_SET && s->count) {
    long p=_pax_svc->file_tell(s->file);
    if(p>=long(s->count) && o>=p-long(s->count) && o<=p) {
      s->at=unsigned(o-(p-long(s->count)));return FLUID_OK;
    }
  }
  if(whence==SEEK_CUR)o-=long(s->count-s->at);
  s->at=s->count=0;
  return _pax_svc->file_seek(s->file,o,whence)==0?FLUID_OK:FLUID_FAILED;
}
static void setup_loader(fluid_synth_t *synth,fluid_settings_t *settings) {
  fluid_sfloader_t *loader=new_fluid_defsfloader(settings);
  if(!loader)return;
  fluid_sfloader_set_callbacks(loader,bank_open,bank_read,bank_seek,bank_tell,bank_close);
  fluid_synth_add_sfloader(synth,loader);
}
extern "C" int duke_sf2_midi_init(int rate) {
  if(rate!=22050)return 0;
  selectedFont=0;preferMidi=0;
  void *preference=_pax_svc->file_open("/sd/roms/duke3d/soundfont.cfg","rb");
  if(preference) {
    char value[3]{};size_t count=_pax_svc->file_read(value,1,sizeof(value),preference);
    _pax_svc->file_close(preference);
    if(count==2 && (value[0]=='0' || value[0]=='1') && value[1]=='\n') {
      selectedFont=value[0]-'0';preferMidi=1;
    }
  }
  // Prefer the lower-rate bank with unused definitions removed, while
  // retaining the original bank as a fallback. Never load the 53 MiB source.
  const char *path=available_font(selectedFont);
  if(!path) { selectedFont=1-selectedFont;path=available_font(selectedFont); }
  if(!path) { selectedFont=0;return 0; }
  std::strcpy(font,path);paused=false;volume=64;
  _pax_svc->log_printf("DUKE FluidSynth: soundfont %s\n",font);
  return 1;
}
extern "C" int duke_sf2_midi_play(const void *data,size_t length,int loop) {
  if(!data || length<14 || length>256*1024)return 0;
  const char *path=available_font(duke_sf2_midi_selection());
  if(!path)return 0;
  std::strcpy(font,path);
  paused=false;
  bool ok=midi.open(nullptr,font,_pax_svc->get_time_us,data,length,loop!=0,setup_loader);
  if(ok)_pax_svc->log_printf("DUKE FluidSynth: MIDI playing, 16 voices, 16000 -> 22050 Hz; init settings=%d synth=%d font=%d player=%d ms\n",int(midi.timing.settings/1000),int(midi.timing.synth/1000),int(midi.timing.font/1000),int(midi.timing.player/1000));
  else _pax_svc->log_printf("DUKE FluidSynth: initialization failed; OPL fallback\n");
  return ok;
}
extern "C" void duke_sf2_midi_stop() { midi.stop(); }
extern "C" void duke_sf2_midi_shutdown() { midi.close();font[0]=0; }
extern "C" void duke_sf2_midi_pause(int p) { paused=p!=0; }
extern "C" int duke_sf2_midi_is_playing() { return midi.playing(); }
extern "C" void duke_sf2_midi_set_volume(int v) { volume=v<0?0:v>64?64:v; }
extern "C" void duke_sf2_midi_render(int16_t *mono,int frames) {
  if(paused || !mono || frames<=0)return;
  for(int at=0;at<frames;at+=512) {
    int n=frames-at<512?frames-at:512;
    if(midi.render(pcm,n)!=n)return;
    for(int i=0;i<n;++i) {
      int music=(int(pcm[i*2])+pcm[i*2+1])*volume/128;
      int mixed=int(mono[at+i])+music;
      mono[at+i]=short(mixed>32767?32767:mixed<-32768?-32768:mixed);
    }
  }
}
