#pragma once
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <cstdint>

// One active PAX and one video decoder task own this cache. The loader first
// stops app tasks, then resets it with the other app resources on every exit.
class PaxJpegAccelerator {
 public:
  void reset() {
    if (engine_) jpeg_del_decoder_engine(engine_);
    engine_ = nullptr;
    heap_caps_free(input_); heap_caps_free(output_);
    input_ = output_ = nullptr; input_cap_ = output_cap_ = 0;
  }
  int decode(const uint8_t *data, uint32_t size, uint16_t *out, uint32_t capacity,
             int width, int height, int stride) {
    if (!data || !out || size < 4 || size > 8 * 1024 * 1024 ||
        width < 1 || height < 1 || width > 4096 || height > 4096 ||
        stride < width * 2 || uint64_t(stride) * height > capacity) return -1;
    jpeg_decode_picture_info_t info{};
    if (jpeg_decoder_get_info(data, size, &info) != ESP_OK ||
        info.width != uint32_t(width) || info.height != uint32_t(height) ||
        info.sample_method == JPEG_DOWN_SAMPLING_GRAY) return -2;
    // P4 DMA writes MCU-padded rows. RGB output for 4:2:0 and 4:2:2 uses
    // 16-pixel padding; 4:4:4 uses 8. Copy only visible pixels to the caller.
    const int block_w = info.sample_method == JPEG_DOWN_SAMPLING_YUV444 ? 8 : 16;
    const int block_h = info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
    const uint32_t pw = (width + block_w - 1) & ~(block_w - 1);
    const uint32_t ph = (height + block_h - 1) & ~(block_h - 1);
    const uint64_t need = uint64_t(pw) * ph * 2;
    if (need > 16 * 1024 * 1024) return -2;
    if (!grow(input_, input_cap_, size, JPEG_DEC_ALLOC_INPUT_BUFFER) ||
        !grow(output_, output_cap_, need, JPEG_DEC_ALLOC_OUTPUT_BUFFER)) return -3;
    if (!engine_) {
      jpeg_decode_engine_cfg_t engine_cfg{};
      engine_cfg.timeout_ms = 100;
      if (jpeg_new_decoder_engine(&engine_cfg, &engine_) != ESP_OK) return -4;
    }
    std::memcpy(input_, data, size);
    jpeg_decode_cfg_t config{};
    config.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    config.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    config.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t written = 0;
    if (jpeg_decoder_process(engine_, &config, input_, size, output_, output_cap_, &written) != ESP_OK)
      return -4;
    if (written < need) return -4;
    for (int y = 0; y < height; ++y)
      std::memcpy(reinterpret_cast<uint8_t *>(out) + size_t(y) * stride,
                  output_ + size_t(y) * pw * 2, width * 2);
    return 0;
  }
 private:
  static bool grow(uint8_t *&buffer, size_t &capacity, size_t size, jpeg_dec_buffer_alloc_direction_t direction) {
    if (capacity >= size) return true;
    heap_caps_free(buffer); buffer = nullptr; capacity = 0;
    jpeg_decode_memory_alloc_cfg_t config{};
    config.buffer_direction = direction;
    buffer = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(size, &config, &capacity));
    return buffer != nullptr;
  }
  jpeg_decoder_handle_t engine_{nullptr};
  uint8_t *input_{nullptr}, *output_{nullptr};
  size_t input_cap_{0}, output_cap_{0};
};
static PaxJpegAccelerator jpeg_accelerator;
