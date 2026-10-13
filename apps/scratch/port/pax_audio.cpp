#include "pax_port.h"
#include <audio.hpp>
#include <audiostack.hpp>
static int64_t lastAudio;
static bool active;
bool SoundPlayer::init() { pax_svc->audio_init(Mixer::rate); lastAudio=pax_time_us();active=true;return true; }
void SoundPlayer::deinit() {active=false;Mixer::cleanupAudio();}
void pax_audio_pump() {
 if(!active)return;short samples[512*2]; auto now=pax_time_us();
 if(now-lastAudio>200000) lastAudio=now-10000; // bound catch-up after loading
 while(now-lastAudio>=10000) {Mixer::requestSound(samples,480); pax_svc->audio_submit(samples,480); lastAudio+=10000;}
}
