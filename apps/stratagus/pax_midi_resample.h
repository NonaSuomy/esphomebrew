#pragma once
#include <cstdint>

// Pull-based, continuous 16000 -> 22050 conversion. The caller supplies
// stereo source frames; fractional phase survives output block boundaries.
class PaxMidiResample {
  uint32_t phase = 0;
  short left[2]{}, right[2]{};
  bool ready = false;
public:
  void reset() { phase = 0; ready = false; }
  template<class Next> bool render(short *output, int frames, Next next) {
    if (!ready) {
      if (!next(left) || !next(right)) return false;
      ready = true;
    }
    for (int i = 0; i < frames; ++i) {
      for (int c = 0; c < 2; ++c)
        output[i*2+c] = short(int(left[c]) + (int(right[c])-left[c])*int(phase)/22050);
      phase += 16000;
      if (phase >= 22050) {
        phase -= 22050;
        left[0] = right[0]; left[1] = right[1];
        if (!next(right)) return false;
      }
    }
    return true;
  }
};
