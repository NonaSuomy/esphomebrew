#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::pax_launcher {

// Launcher-only state. Never consume the PAX's accumulated mouse deltas.
struct Pointer {
  explicit Pointer(int32_t width = 1024, int32_t height = 600)
      : max_x(width - 1), max_y(height - 1), x(width / 2), y(height / 2) {}
  int32_t max_x;
  int32_t max_y;
  int32_t x{512};
  int32_t y{300};
  bool left_down{false};
  bool release_guard{true};

  void move(float dx, float dy, bool enabled) {
    if (!enabled || !std::isfinite(dx) || !std::isfinite(dy))
      return;
    x = static_cast<int32_t>(std::clamp(float(x) + dx, 0.0f, float(max_x)));
    y = static_cast<int32_t>(std::clamp(float(y) + dy, 0.0f, float(max_y)));
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
