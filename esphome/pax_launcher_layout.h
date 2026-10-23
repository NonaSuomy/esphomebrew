#pragma once
#include <algorithm>
namespace esphome::pax_launcher {
struct LauncherLayout {
  int width, height, x[3], y[3];
};
inline LauncherLayout launcher_layout(int w, int h) {
  const bool landscape = w >= h;
  const int bw = landscape ? std::min(w * 270 / 1024, (w - 32) / 3) : w * 3 / 4;
  const int bh = std::min(landscape ? h / 4 : h * 150 / 800, 180);
  LauncherLayout out{bw, bh, {}, {}};
  if (landscape) {
    const int margin = w * 70 / 1024;
    const int gap = (w - 2 * margin - 3 * bw) / 2;
    for (int i = 0; i < 3; i++) { out.x[i] = margin + i * (bw + gap); out.y[i] = (h - bh) / 2; }
  } else {
    const int gap = h / 20;
    const int top = (h - 3 * bh - 2 * gap) / 2;
    for (int i = 0; i < 3; i++) { out.x[i] = (w - bw) / 2; out.y[i] = top + i * (bh + gap); }
  }
  return out;
}
}
