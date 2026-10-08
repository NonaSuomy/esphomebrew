#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::pax_launcher {

// Launcher-only state. Never consume the PAX's accumulated mouse deltas.
struct Pointer {
  int32_t x{512};
  int32_t y{300};
  bool left_down{false};
  bool release_guard{true};

  void move(float dx, float dy, bool enabled) {
    if (!enabled || !std::isfinite(dx) || !std::isfinite(dy))
      return;
    x = static_cast<int32_t>(std::clamp(float(x) + dx, 0.0f, 1023.0f));
    y = static_cast<int32_t>(std::clamp(float(y) + dy, 0.0f, 599.0f));
  }
  void button(bool down, bool enabled) {
    left_down = down;
    if (!enabled)
      release_guard = true;
    if (!down)
      release_guard = false;
  }
  bool pressed(bool enabled) {
    if (!enabled) {
      release_guard = true;
      return false;
    }
    if (!left_down)
      release_guard = false;
    return left_down && !release_guard;
  }
};

}  // namespace esphome::pax_launcher
