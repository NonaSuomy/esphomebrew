#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace esphome {
namespace pax_loader {

enum UiSound : uint8_t {
  UI_SOUND_CLICK,    // D-pad / cursor move: crisp wooden micro-tap
  UI_SOUND_CONFIRM,  // Tile tap / select / A: cheerful rising pop/snap
  UI_SOUND_CHIME,    // Page / tab change: sweet marimba two-tone chord
  UI_SOUND_BACK,     // Back / cancel / B: soft warm descending swoosh
  UI_SOUND_VOLUME,   // Volume slider tick: pitch scaled to volume level
};

class UiSoundGenerator {
 public:
  static constexpr int SAMPLE_RATE = 22050;

  // Generates 16-bit interleaved stereo PCM into out_buf (returns number of stereo frames).
  static size_t generate(UiSound sound, uint8_t param, int16_t *out_buf, size_t max_frames) {
    if (out_buf == nullptr || max_frames == 0)
      return 0;

    switch (sound) {
      case UI_SOUND_CLICK:
        return generate_click(out_buf, max_frames);
      case UI_SOUND_CONFIRM:
        return generate_confirm(out_buf, max_frames);
      case UI_SOUND_CHIME:
        return generate_chime(out_buf, max_frames);
      case UI_SOUND_BACK:
        return generate_back(out_buf, max_frames);
      case UI_SOUND_VOLUME:
        return generate_volume_tick(param, out_buf, max_frames);
      default:
        return 0;
    }
  }

 private:
  static constexpr float TWO_PI = 6.28318530717958647692f;

  // Nintendo Switch cursor click: crisp, percussive high tap with rapid decay (~16ms)
  static size_t generate_click(int16_t *buf, size_t max_frames) {
    const size_t frames = std::min<size_t>(max_frames, SAMPLE_RATE * 16 / 1000);  // ~16 ms
    float phase = 0.0f;
    for (size_t i = 0; i < frames; i++) {
      const float t = static_cast<float>(i) / frames;
      // Exponential decay envelope
      const float env = std::exp(-t * 6.5f) * (1.0f - t);
      // Pitch drops rapidly from 2200 Hz down to 600 Hz (wooden percussive transient)
      const float freq = 600.0f + 1600.0f * (1.0f - t) * (1.0f - t);
      phase += freq * (TWO_PI / SAMPLE_RATE);
      if (phase > TWO_PI) phase -= TWO_PI;

      // Pure sine with a touch of second harmonic for body
      const float sample = (std::sin(phase) * 0.85f + std::sin(phase * 2.0f) * 0.15f) * env;
      const int16_t val = static_cast<int16_t>(std::clamp(sample * 14000.0f, -32767.0f, 32767.0f));
      buf[i * 2] = val;
      buf[i * 2 + 1] = val;
    }
    return frames;
  }

  // Nintendo Switch select/confirm: bright, bubbly rising pop with harmonic chime (~45ms)
  static size_t generate_confirm(int16_t *buf, size_t max_frames) {
    const size_t frames = std::min<size_t>(max_frames, SAMPLE_RATE * 45 / 1000);  // ~45 ms
    float phase1 = 0.0f;
    float phase2 = 0.0f;
    for (size_t i = 0; i < frames; i++) {
      const float t = static_cast<float>(i) / frames;
      // Attack-decay envelope: quick 2ms attack, smooth exponential decay
      const float attack = std::min(1.0f, static_cast<float>(i) / (SAMPLE_RATE * 0.002f));
      const float env = attack * std::exp(-t * 4.2f) * (1.0f - t);

      // Pitch swoops up from 450 Hz to 1350 Hz
      const float freq1 = 450.0f + 900.0f * std::sqrt(t);
      // Bell/sparkle harmonic at 2.5x
      const float freq2 = freq1 * 2.5f;

      phase1 += freq1 * (TWO_PI / SAMPLE_RATE);
      if (phase1 > TWO_PI) phase1 -= TWO_PI;
      phase2 += freq2 * (TWO_PI / SAMPLE_RATE);
      if (phase2 > TWO_PI) phase2 -= TWO_PI;

      const float sample = (std::sin(phase1) * 0.75f + std::sin(phase2) * 0.25f) * env;
      const int16_t val = static_cast<int16_t>(std::clamp(sample * 16000.0f, -32767.0f, 32767.0f));
      buf[i * 2] = val;
      buf[i * 2 + 1] = val;
    }
    return frames;
  }

  // Page / Tab change: pleasant marimba dual-note chime (C6 1046 Hz -> G6 1568 Hz) (~55ms)
  static size_t generate_chime(int16_t *buf, size_t max_frames) {
    const size_t frames = std::min<size_t>(max_frames, SAMPLE_RATE * 55 / 1000);
    const size_t note_split = frames * 35 / 100;  // first note is short, second note sustains
    float phase1 = 0.0f;
    float phase2 = 0.0f;
    for (size_t i = 0; i < frames; i++) {
      float sample = 0.0f;
      if (i < note_split) {
        const float t = static_cast<float>(i) / note_split;
        const float env = (1.0f - t * t) * 0.7f;
        phase1 += 1046.5f * (TWO_PI / SAMPLE_RATE);  // C6
        if (phase1 > TWO_PI) phase1 -= TWO_PI;
        sample = std::sin(phase1) * env;
      } else {
        const float t = static_cast<float>(i - note_split) / (frames - note_split);
        const float env = std::exp(-t * 3.5f) * (1.0f - t);
        phase2 += 1567.98f * (TWO_PI / SAMPLE_RATE);  // G6
        if (phase2 > TWO_PI) phase2 -= TWO_PI;
        // Warm marimba: fundamental + soft octave overtone
        sample = (std::sin(phase2) * 0.8f + std::sin(phase2 * 2.0f) * 0.2f) * env;
      }
      const int16_t val = static_cast<int16_t>(std::clamp(sample * 15000.0f, -32767.0f, 32767.0f));
      buf[i * 2] = val;
      buf[i * 2 + 1] = val;
    }
    return frames;
  }

  // Back / Cancel / Dismiss: soft warm descending swoosh (900 Hz down to 420 Hz) (~35ms)
  static size_t generate_back(int16_t *buf, size_t max_frames) {
    const size_t frames = std::min<size_t>(max_frames, SAMPLE_RATE * 35 / 1000);
    float phase = 0.0f;
    for (size_t i = 0; i < frames; i++) {
      const float t = static_cast<float>(i) / frames;
      const float env = std::exp(-t * 3.8f) * (1.0f - t);
      const float freq = 900.0f - 480.0f * t;
      phase += freq * (TWO_PI / SAMPLE_RATE);
      if (phase > TWO_PI) phase -= TWO_PI;

      const float sample = std::sin(phase) * env;
      const int16_t val = static_cast<int16_t>(std::clamp(sample * 13000.0f, -32767.0f, 32767.0f));
      buf[i * 2] = val;
      buf[i * 2 + 1] = val;
    }
    return frames;
  }

  // Volume slider tick: pitch scales with volume percent (0-100) from 800 Hz up to 2400 Hz (~12ms)
  static size_t generate_volume_tick(uint8_t volume, int16_t *buf, size_t max_frames) {
    const size_t frames = std::min<size_t>(max_frames, SAMPLE_RATE * 12 / 1000);
    const float base_freq = 800.0f + static_cast<float>(std::min<uint8_t>(volume, 100)) * 16.0f;
    float phase = 0.0f;
    for (size_t i = 0; i < frames; i++) {
      const float t = static_cast<float>(i) / frames;
      const float env = (1.0f - t) * (1.0f - t);
      phase += base_freq * (TWO_PI / SAMPLE_RATE);
      if (phase > TWO_PI) phase -= TWO_PI;

      const float sample = std::sin(phase) * env;
      const int16_t val = static_cast<int16_t>(std::clamp(sample * 12000.0f, -32767.0f, 32767.0f));
      buf[i * 2] = val;
      buf[i * 2 + 1] = val;
    }
    return frames;
  }
};

}  // namespace pax_loader
}  // namespace esphome
