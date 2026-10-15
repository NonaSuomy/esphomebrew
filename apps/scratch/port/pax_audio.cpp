#include "pax_port.h"
#include <algorithm>
#include <audio.hpp>
#include <audiostack.hpp>
static int64_t lastAudio;
static bool active;
bool SoundPlayer::init() { pax_svc->audio_init(Mixer::rate); lastAudio=pax_time_us();active=true;return true; }
void SoundPlayer::deinit() {active=false;Mixer::cleanupAudio();}
// Audio playback follows wall time even when a long interpreter operation
// prevents pumping. Advance missed source samples instead of extending sounds.
static void skipElapsedAudio(int64_t elapsedUs) {
 for (auto &entry : Mixer::streams) {
  auto *sound = entry.second;
  if (sound->paused) continue;
  uint64_t frames = static_cast<uint64_t>(elapsedUs * (double)sound->rate * sound->config.pitch / 1000000.0);
  if (sound->type == SoundStreamWAV) {
   const auto cursor = sound->wav.readCursorInPCMFrames;
   const auto total = sound->wav.totalPCMFrameCount;
   if (cursor >= total || frames >= total - cursor) sound->paused = true;
   else drwav_seek_to_pcm_frame(&sound->wav, cursor + frames);
  } else {
   float discarded[1024];
   const int capacity = std::max(1, 1024 / std::max(1, sound->channels));
   while (frames && !sound->paused) {
    const int count = static_cast<int>(std::min<uint64_t>(frames, capacity));
    const int got = sound->read(discarded, count);
    if (got <= 0) sound->paused = true;
    else frames -= got;
   }
  }
 }
}
void pax_audio_pump() {
 if(!active)return;short samples[512*2]; auto now=pax_time_us();
 if(now<lastAudio) lastAudio=now;
 if(now-lastAudio>200000) {
  const auto skipped=now-lastAudio-10000;
  skipElapsedAudio(skipped);
  lastAudio+=skipped;
 }
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
