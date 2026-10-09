#pragma once
#include <algorithm>
#include <cstdint>
#include "pax_overlay_theme.h"

namespace esphome {
namespace pax_loader {
namespace volume_icon {
static constexpr int SIZE = 58;
inline uint16_t &BACKGROUND = overlay_theme::PANEL;
inline uint16_t &BODY = overlay_theme::TEXT;
inline uint16_t &ACTIVE = overlay_theme::ACCENT;
inline uint16_t &INACTIVE = overlay_theme::MUTED;

// Draw in visible coordinates, then map into the panel's 180-degree raw transfer.
// Keeping a rectangular driver and a wide mouth avoids an arrow-shaped silhouette.
inline void render(uint16_t *pixels, int level) {
  std::fill_n(pixels, SIZE * SIZE, BACKGROUND);
  auto point = [pixels](int x, int y, uint16_t color) {
    pixels[(SIZE - 1 - y) * SIZE + SIZE - 1 - x] = color;
  };
  for (int y = 23; y <= 35; ++y)
    for (int x = 8; x <= 17; ++x)
      point(x, y, BODY);
  for (int x = 17; x <= 29; ++x) {
    const int half_height = 6 + (x - 17) * 10 / 12;
    for (int y = 29 - half_height; y <= 29 + half_height; ++y)
      point(x, y, BODY);
  }
  // A straight rim makes the speaker opening distinct at the small icon size.
  for (int y = 13; y <= 45; ++y)
    for (int x = 28; x <= 30; ++x)
      point(x, y, BODY);

  const int bars = (std::max(0, std::min(100, level)) + 19) / 20;
  for (int bar = 0; bar < 5; ++bar) {
    const int height = 5 + bar * 3;
    const uint16_t color = bar < bars ? ACTIVE : INACTIVE;
    for (int y = 29 - height; y <= 29 + height; ++y)
      for (int x = 35 + bar * 4; x < 37 + bar * 4; ++x)
        point(x, y, color);
  }
}
}  // namespace volume_icon
}  // namespace pax_loader
}  // namespace esphome
