#include "pax_port.h"
#include <algorithm>
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

static std::vector<int16_t> editorSamples;
static size_t editorAudioPosition;
void pax_stop_editor_sound() {
  Mixer::stopSound("pax_editor_preview");
  std::vector<int16_t>().swap(editorSamples);
}
void pax_preview_editor_sound(const std::vector<int16_t> &samples, int rate) {
  pax_stop_editor_sound();
  editorSamples = samples;
  editorAudioPosition = 0;
  SoundPlayer::init();
  auto stream = new SoundStream(
      "pax_editor_preview",
      [](SoundStream *, float *out, int length) {
        int n = std::min<size_t>(length,
                                 editorSamples.size() - editorAudioPosition);
        for (int i = 0; i < n; i++)
          out[i] = editorSamples[editorAudioPosition++] / 32768.f;
        return n;
      },
      1, rate);
  stream->auto_clean = true;
  Mixer::streams["pax_editor_preview"] = stream;
}
