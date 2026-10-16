#pragma once
#include "esphome/components/display/display.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
namespace esphome::pax_jc1060 {
class NativePanel : public display::Display {
 public:
  explicit NativePanel(display::Display *panel) : panel_(panel) {}
  ~NativePanel() { heap_caps_free(buffer_); }
  void update() override {}
  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_COLOR; }
  void draw_pixel_at(int x, int y, Color color) override { panel_->draw_pixel_at(1023-x, 599-y, color); }
  void draw_pixels_at(int x, int y, int w, int h, const uint8_t *data, display::ColorOrder order,
                      display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset,
                      int x_pad) override {
    if (w <= 0 || h <= 0 || data == nullptr || bitness != display::COLOR_BITNESS_565) return;
    const size_t bytes = static_cast<size_t>(w) * h * sizeof(uint16_t);
    if (bytes > capacity_) {
      auto *next = static_cast<uint16_t *>(heap_caps_realloc(buffer_, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (next == nullptr) {
        ESP_LOGE("jc1060_panel", "Cannot allocate native transfer (%zu bytes)", bytes);
        return;
      }
      buffer_=next; capacity_=bytes;
    }
    const auto *source=reinterpret_cast<const uint16_t *>(data);
    const int stride=w+x_offset+x_pad;
    for (int dy=0; dy<h; ++dy)
      for (int dx=0; dx<w; ++dx)
        buffer_[static_cast<size_t>(dy)*w+dx]=
            source[static_cast<size_t>(h-1-dy+y_offset)*stride+w-1-dx+x_offset];
    esp_cache_msync(buffer_,bytes,ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    panel_->draw_pixels_at(1024-x-w,600-y-h,w,h,reinterpret_cast<const uint8_t *>(buffer_),order,bitness,big_endian);
  }
 protected:
  int get_width_internal() override { return 1024; }
  int get_height_internal() override { return 600; }
  display::Display *panel_;
  uint16_t *buffer_{nullptr};
  size_t capacity_{0};
};
}
