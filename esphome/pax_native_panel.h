#pragma once
#include "esphome/components/display/display.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
namespace esphome::pax_native {
class NativePanel : public display::Display {
 public:
  explicit NativePanel(display::Display *panel)
      : panel_(panel), width_(panel->get_width()), height_(panel->get_height()) {}
  ~NativePanel() { heap_caps_free(buffer_); }
  void update() override {}
  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_COLOR; }
  void draw_pixel_at(int x, int y, Color color) override {
    panel_->draw_pixel_at(width_ - 1 - x, height_ - 1 - y, color);
  }
  void draw_pixels_at(int x, int y, int w, int h, const uint8_t *data, display::ColorOrder order,
                      display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) override {
    if (w <= 0 || h <= 0 || data == nullptr || bitness != display::COLOR_BITNESS_565)
      return;
    const size_t bytes = static_cast<size_t>(w) * h * sizeof(uint16_t);
    if (bytes > capacity_) {
      auto *next = static_cast<uint16_t *>(heap_caps_realloc(buffer_, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (next == nullptr) {
        ESP_LOGE("jc1060_panel", "Cannot allocate native transfer (%zu bytes)", bytes);
        return;
      }
      buffer_ = next;
      capacity_ = bytes;
    }
    const auto *source = reinterpret_cast<const uint16_t *>(data);
    const int stride = w + x_offset + x_pad;
    for (int dy = 0; dy < h; ++dy)
      for (int dx = 0; dx < w; ++dx)
        buffer_[static_cast<size_t>(dy) * w + dx] =
            source[static_cast<size_t>(h - 1 - dy + y_offset) * stride + w - 1 - dx + x_offset];
    esp_cache_msync(buffer_, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    panel_->draw_pixels_at(width_ - x - w, height_ - y - h, w, h, reinterpret_cast<const uint8_t *>(buffer_), order,
                           bitness, big_endian);
  }

 protected:
  int get_width_internal() override { return width_; }
  int get_height_internal() override { return height_; }
  display::Display *panel_;
  int width_, height_;
  uint16_t *buffer_{nullptr};
  size_t capacity_{0};
};
}  // namespace esphome::pax_native
