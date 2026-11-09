"""Exercise the production audio initializer against a running/stopped speaker."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'components/pax_loader/pax_loader.cpp').read_text()
method = source.split('void PaxLoader::audio_init_(int sample_rate) {', 1)[1].split('void PaxLoader::audio_submit_', 1)[0]
code = r'''#include <cassert>
#define ESP_LOGI(...) ((void)0)
namespace audio { struct AudioStreamInfo { AudioStreamInfo(int,int,int) {} }; }
int heap_logs=0;void log_heap(const char*){++heap_logs;}
struct Speaker {
 bool running=false;int starts=0,formats=0;bool muted=false;float volume=0;
 void set_audio_stream_info(audio::AudioStreamInfo){++formats;}
 void set_mute_state(bool v){muted=v;}void set_volume(float v){volume=v;}
 bool is_running(){return running;}void start(){++starts;running=true;}
};
struct PaxLoader {Speaker *speaker_=nullptr;int audio_sample_rate_=0,master_volume_=70;bool game_menu_open_=false;void audio_init_(int);};
void PaxLoader::audio_init_(int sample_rate) {''' + method + r'''
int main(){
 PaxLoader loader;loader.audio_init_(22050);assert(heap_logs==0);
 Speaker speaker;loader.speaker_=&speaker;
 loader.audio_init_(0);assert(speaker.starts==0);
 loader.audio_init_(22050);assert(speaker.starts==1&&speaker.formats==1&&heap_logs==1);
 for(int i=0;i<20;++i)loader.audio_init_(22050);
 assert(speaker.starts==1&&speaker.formats==1&&heap_logs==1);
 loader.game_menu_open_=true;loader.audio_init_(22050);assert(speaker.muted&&speaker.starts==1);
 loader.game_menu_open_=false;loader.master_volume_=0;loader.audio_init_(22050);assert(speaker.muted&&speaker.volume==0);
 speaker.running=false;loader.audio_init_(22050);assert(speaker.starts==2&&speaker.formats==1&&heap_logs==2);
 loader.audio_init_(48000);assert(speaker.starts==3&&speaker.formats==2&&heap_logs==3);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS audio initialization: healthy chain reuse, stopped-chain restart, rate change, mute and invalid inputs')
