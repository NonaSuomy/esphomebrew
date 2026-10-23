#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace esphome::pax_frame {
// The input is tightly packed with source_width pixels per row. Clip once,
// then copy spans instead of branching on every pixel. Widen coordinates
// before arithmetic so off-screen/invalid requests cannot wrap into the canvas.
inline void copy_rect(uint16_t *canvas, int canvas_width, int canvas_height,
                      int x, int y, int source_width, int source_height,
                      const uint16_t *source) {
  if (!canvas || !source || canvas_width <= 0 || canvas_height <= 0 ||
      source_width <= 0 || source_height <= 0) return;
  const int64_t left = std::max<int64_t>(0, x), top = std::max<int64_t>(0, y);
  const int64_t right = std::min<int64_t>(canvas_width, int64_t(x) + source_width);
  const int64_t bottom = std::min<int64_t>(canvas_height, int64_t(y) + source_height);
  if (left >= right || top >= bottom) return;
  const uint64_t max_pixels = std::numeric_limits<size_t>::max() / sizeof(uint16_t);
  const uint64_t source_end = uint64_t(bottom - y - 1) * source_width + uint64_t(right - x);
  if (source_end > max_pixels || uint64_t(canvas_width) * canvas_height > max_pixels) return;
  const size_t source_offset = size_t(uint64_t(top - y) * source_width + uint64_t(left - x));
  const size_t target_offset = size_t(top) * canvas_width + size_t(left);
  const size_t columns = size_t(right - left), rows = size_t(bottom - top);
  if (columns == size_t(canvas_width) && columns == size_t(source_width)) {
    std::memmove(canvas + target_offset, source + source_offset, rows * columns * sizeof(uint16_t));
    return;
  }
  for (size_t row = 0; row < rows; ++row)
    std::memmove(canvas + target_offset + row * canvas_width,
                 source + source_offset + row * source_width, columns * sizeof(uint16_t));
}
}  // namespace esphome::pax_frame
