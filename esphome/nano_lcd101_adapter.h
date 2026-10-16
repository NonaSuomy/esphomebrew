#pragma once
#include "esphome/components/display/display.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"

namespace esphome::pax_nano {
// Only this profile installs the adapter. The common PAX renderer supplies
// 180-degree-oriented landscape pixels; convert them to portrait scanout.
class LandscapePanel : public display::Display {
 public:
  explicit LandscapePanel(display::Display *panel) : panel_(panel) {}
  ~LandscapePanel() { heap_caps_free(buffer_); }
  void update() override {}
  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_COLOR; }
  void draw_pixel_at(int x, int y, Color color) override { panel_->draw_pixel_at(y, 1279 - x, color); }
  void draw_pixels_at(int x, int y, int w, int h, const uint8_t *data, display::ColorOrder order,
                      display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset,
                      int x_pad) override {
    if (w <= 0 || h <= 0 || data == nullptr || bitness != display::COLOR_BITNESS_565) return;
    const size_t bytes = static_cast<size_t>(w) * h * sizeof(uint16_t);
    if (bytes > capacity_) {
      auto *next = static_cast<uint16_t *>(heap_caps_realloc(buffer_, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (next == nullptr) {
        ESP_LOGE("nano_panel", "Cannot allocate portrait transfer (%zu bytes)", bytes);
        return;
      }
      buffer_ = next;
      capacity_ = bytes;
    }
    const auto *source = reinterpret_cast<const uint16_t *>(data);
    const int stride = w + x_offset + x_pad;
    for (int sy = 0; sy < h; ++sy)
      for (int sx = 0; sx < w; ++sx)
        buffer_[static_cast<size_t>(w - 1 - sx) * h + sy] =
            source[static_cast<size_t>(sy + y_offset) * stride + sx + x_offset];
    esp_cache_msync(buffer_, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    panel_->draw_pixels_at(y, 1280 - x - w, h, w, reinterpret_cast<const uint8_t *>(buffer_),
                           order, bitness, big_endian);
  }
 protected:
  int get_width_internal() override { return 1280; }
  int get_height_internal() override { return 800; }
  display::Display *panel_;
  uint16_t *buffer_{nullptr};
  size_t capacity_{0};
};
}
