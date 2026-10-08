#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace esphome::pax_launcher {
inline bool owns_pointer(std::string_view source) {
  source = source.substr(0, source.find_first_of("?#"));
  const auto slash = source.find_last_of('/');
  if (slash != std::string_view::npos) source.remove_prefix(slash + 1);
  return source == "launcher_ui.pax" || source == "launcher_ui";
}
struct DirtyRegion {
  int x{0}, y{0}, w{0}, h{0};
  void clear() { *this = {}; }
  void add(int px, int py, int pw, int ph, int cw, int ch) {
    if (pw <= 0 || ph <= 0) return;
    const int left = std::max(0, px), top = std::max(0, py);
    const int right = int(std::min<int64_t>(cw, int64_t(px) + pw));
    const int bottom = int(std::min<int64_t>(ch, int64_t(py) + ph));
    if (right <= left || bottom <= top) return;
    if (w == 0) { x = left; y = top; w = right - left; h = bottom - top; return; }
    const int nx = std::min(x, left), ny = std::min(y, top);
    w = std::max(x + w, right) - nx;
    h = std::max(y + h, bottom) - ny;
    x = nx; y = ny;
  }
  void rotated_pixels(const uint16_t *canvas, int stride, uint16_t *out) const {
    for (int row = 0; row < h; ++row)
      for (int col = 0; col < w; ++col)
        out[(h - 1 - row) * w + w - 1 - col] = canvas[(y + row) * stride + x + col];
  }
};
}  // namespace esphome::pax_launcher
