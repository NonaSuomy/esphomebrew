#ifndef PAX_MIDI_H
#define PAX_MIDI_H
#include <fluidsynth.h>
#include <cstring>
#include <cstdio>
#include "pax_midi_resample.h"

// The embedded OSAL has no real locks. Only the audio worker may use these
// objects; requests arrive through the existing locked music-path mailbox.
class PaxMidi {
  fluid_settings_t *settings = nullptr;
  fluid_synth_t *synth = nullptr;
  fluid_player_t *player = nullptr;
  char loadedFont[256]{};
  PaxMidiResample resample;
  // Batch multiple FluidSynth DSP blocks per call. One 64-frame call at a
  // time defeats its bulk renderer and spends too much time in setup.
  static constexpr int sourceFrames = 512;
  short source[sourceFrames*2]{};
  int sourceAt = sourceFrames;
public:
  struct Timing { int64_t settings=0, synth=0, font=0, player=0; } timing;
  ~PaxMidi() { close(); }
  void stop() {
    if (player) { fluid_player_stop(player); delete_fluid_player(player); }
    player = nullptr;
    resample.reset(); sourceAt = sourceFrames;
    if (synth) fluid_synth_all_sounds_off(synth, -1);
  }
  void close() {
    stop();
    if (synth) delete_fluid_synth(synth);
    if (settings) delete_fluid_settings(settings);
    player = nullptr; synth = nullptr; settings = nullptr;
    loadedFont[0] = '\0';
  }
  bool open(const char *path, const char *soundfont, int64_t (*clock)() = nullptr,
            const void *memory = nullptr, size_t length = 0, bool looping = true,
            void (*setupLoader)(fluid_synth_t *,fluid_settings_t *) = nullptr) {
    timing = {};
    int64_t stamp = clock ? clock() : 0;
    if (synth && std::strcmp(loadedFont, soundfont)) close();
    stop();
    if (!synth) {
    settings = new_fluid_settings();
    if (!settings) return false;
    fluid_settings_setnum(settings, "synth.sample-rate", 16000);
    fluid_settings_setnum(settings, "synth.gain", 0.5);
    fluid_settings_setint(settings, "synth.polyphony", 16);
    fluid_settings_setint(settings, "synth.reverb.active", 0);
    fluid_settings_setint(settings, "synth.chorus.active", 0);
    fluid_settings_setint(settings, "synth.lock-memory", 0);
    fluid_settings_setint(settings, "synth.threadsafe-api", 0);
    fluid_settings_setint(settings, "synth.cpu-cores", 1);
    // Only load samples for selected instruments, not the complete GM bank.
    fluid_settings_setint(settings, "synth.dynamic-sample-loading", 1);
    fluid_settings_setstr(settings, "player.timing-source", "sample");
    if (clock) { timing.settings = clock()-stamp; stamp = clock(); }
    synth = new_fluid_synth(settings);
    if (clock) { timing.synth = clock()-stamp; stamp = clock(); }
    if(synth && setupLoader) setupLoader(synth,settings);
    if (!synth || fluid_synth_sfload(synth, soundfont, 1) < 0) { close(); return false; }
    if (clock) { timing.font = clock()-stamp; stamp = clock(); }
    fluid_synth_set_interp_method(synth, -1, FLUID_INTERP_LINEAR);
    std::snprintf(loadedFont,sizeof(loadedFont),"%s",soundfont);
    }
    player = new_fluid_player(synth);
    if (!player || (memory ? fluid_player_add_mem(player, memory, length) : fluid_player_add(player, path)) != FLUID_OK) { close(); return false; }
    fluid_player_set_loop(player, looping ? -1 : 1);
    if (fluid_player_play(player) != FLUID_OK) { close(); return false; }
    if (clock) timing.player = clock()-stamp;
    return true;
  }
  bool active() const { return player != nullptr; }
  bool playing() const { return player && fluid_player_get_status(player) != FLUID_PLAYER_DONE; }
  int render(short *pcm, int frames) {
    if (!player) return 0;
    auto next = [this](short *frame) {
      if (sourceAt == sourceFrames) {
        if (fluid_synth_write_s16(synth, sourceFrames, source, 0, 2, source, 1, 2) != FLUID_OK) return false;
        sourceAt = 0;
      }
      frame[0] = source[sourceAt*2]; frame[1] = source[sourceAt*2+1];
      ++sourceAt;
      return true;
    };
    return resample.render(pcm, frames, next) ? frames : 0;
  }
};
#endif
