#pragma once
#include "esphome/components/display/display.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#if __has_include("driver/ppa.h")
#include "driver/ppa.h"
#define PAX_NATIVE_PPA 1
#endif
#if __has_include("esphome/components/mipi_dsi/mipi_dsi.h")
#include "esphome/components/mipi_dsi/mipi_dsi.h"
#define PAX_NATIVE_MIPI 1
#endif
namespace esphome::pax_native {
class NativePanel : public display::Display {
 public:
  explicit NativePanel(display::Display *panel)
      : panel_(panel), width_(panel->get_width()), height_(panel->get_height()) {}
#ifdef PAX_NATIVE_MIPI
  explicit NativePanel(mipi_dsi::MIPI_DSI *panel) : NativePanel(static_cast<display::Display *>(panel)) {
    mipi_panel_ = panel;
  }
#endif
  ~NativePanel() {
#ifdef PAX_NATIVE_PPA
    if (ppa_) ppa_unregister_client(ppa_);
#endif
    heap_caps_free(buffer_);
  }
  void update() override {}
  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_COLOR; }
  void draw_pixel_at(int x, int y, Color color) override {
    panel_->draw_pixel_at(width_ - 1 - x, height_ - 1 - y, color);
  }
  void draw_pixels_at(int x, int y, int w, int h, const uint8_t *data, display::ColorOrder order,
                      display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) override {
    if (w <= 0 || h <= 0 || data == nullptr || bitness != display::COLOR_BITNESS_565)
      return;
#ifdef PAX_NATIVE_MIPI
    if (mipi_panel_ && mipi_panel_->draw_rotated_rgb565(width_ - x - w, height_ - y - h, w, h, data,
                                                     x_offset, y_offset, x_pad, big_endian))
      return;
#endif
    const size_t bytes = static_cast<size_t>(w) * h * sizeof(uint16_t);
    if (bytes > capacity_) {
#ifdef PAX_NATIVE_PPA
      const size_t aligned_bytes = (bytes + 127) & ~size_t(127);
      auto *next = static_cast<uint16_t *>(heap_caps_aligned_alloc(128, aligned_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
      const size_t aligned_bytes = bytes;
      auto *next = static_cast<uint16_t *>(heap_caps_realloc(buffer_, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
      if (next == nullptr) {
        ESP_LOGE("jc1060_panel", "Cannot allocate native transfer (%zu bytes)", bytes);
        return;
      }
#ifdef PAX_NATIVE_PPA
      heap_caps_free(buffer_);
#endif
      buffer_ = next;
      capacity_ = aligned_bytes;
    }
    const auto *source = reinterpret_cast<const uint16_t *>(data);
    const int stride = w + x_offset + x_pad;
    bool accelerated = false;
#ifdef PAX_NATIVE_PPA
    // Large app frames are already cache-line aligned. Keep small overlays
    // and unusual source strides on the CPU path, preserving their contract.
    if (bytes >= 4096 && (reinterpret_cast<uintptr_t>(data) & 15) == 0 &&
        x_offset >= 0 && y_offset >= 0 && x_pad >= 0) {
      if (!ppa_) {
        ppa_client_config_t client{};
        client.oper_type = PPA_OPERATION_SRM;
        ppa_register_client(&client, &ppa_);
      }
      if (ppa_) {
        esp_cache_msync(const_cast<uint8_t *>(data), size_t(stride) * (h + y_offset) * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        ppa_srm_oper_config_t config{};
        config.in.buffer = const_cast<uint8_t *>(data);
        config.in.pic_w = stride; config.in.pic_h = h + y_offset;
        config.in.block_w = w; config.in.block_h = h;
        config.in.block_offset_x = x_offset; config.in.block_offset_y = y_offset;
        config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        config.out.buffer = buffer_; config.out.buffer_size = capacity_;
        config.out.pic_w = w; config.out.pic_h = h;
        config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        config.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
        config.scale_x = config.scale_y = 1;
        config.mode = PPA_TRANS_MODE_BLOCKING;
        accelerated = ppa_do_scale_rotate_mirror(ppa_, &config) == ESP_OK;
        if (accelerated)
          esp_cache_msync(buffer_, capacity_, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
      }
    }
#endif
    if (!accelerated) {
      for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx)
          buffer_[static_cast<size_t>(dy) * w + dx] =
              source[static_cast<size_t>(h - 1 - dy + y_offset) * stride + w - 1 - dx + x_offset];
    }
    esp_cache_msync(buffer_, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    panel_->draw_pixels_at(width_ - x - w, height_ - y - h, w, h, reinterpret_cast<const uint8_t *>(buffer_), order,
                           bitness, big_endian);
  }

 protected:
  int get_width_internal() override { return width_; }
  int get_height_internal() override { return height_; }
  display::Display *panel_;
#ifdef PAX_NATIVE_MIPI
  mipi_dsi::MIPI_DSI *mipi_panel_{nullptr};
#endif
  int width_, height_;
  uint16_t *buffer_{nullptr};
  size_t capacity_{0};
#ifdef PAX_NATIVE_PPA
  ppa_client_handle_t ppa_{nullptr};
#endif
};
}  // namespace esphome::pax_native
