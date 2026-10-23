#include "waveshare43.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <algorithm>
namespace esphome::waveshare43 {
static const char *const TAG = "waveshare43";
bool Control::write_control_(uint8_t reg, uint8_t value) {
  for (int attempt = 0; attempt < 5; attempt++) {
    if (this->write_byte(reg, value)) {
      delay(10);
      return true;
    }
    delay(20);
  }
  ESP_LOGE(TAG, "Control register %02X write failed", reg);
  this->status_set_warning();
  return false;
}
void Control::setup() {
  if (!this->write_control_(0x86, 0) || !this->write_control_(0x85, 0)) {
    this->mark_failed();
    return;
  }
  delay(30);
  if (!this->write_control_(0x83, 0) || !this->write_control_(0x81, 4) || !this->write_control_(0x82, 0x80) ||
      !this->write_control_(0x83, 1)) {
    this->mark_failed();
    return;
  }
  delay(80);
  if (!this->write_control_(0x86, 0x80))
    this->mark_failed();
}
void Control::finish_setup() {
  if (this->is_failed())
    return;
  if (!this->write_control_(0x83, 0x0D))
    return;
  delay(10);
  if (!this->write_control_(0x83, 0x0F))
    return;
  delay(300);
  this->ready_ = true;
  this->set_brightness(this->brightness_);
  ESP_LOGI(TAG, "4.3-inch display and touch reset released");
}
void Control::set_brightness(float value) {
  this->brightness_ = std::clamp(value, 0.0f, 1.0f);
  if (!this->ready_)
    return;
  if (this->write_control_(0x86, static_cast<uint8_t>(this->brightness_ * 255 + 0.5f)))
    this->status_clear_warning();
}
void Touch::setup() {
  delay(300);
  uint8_t vendor = 0;
  if (this->read_byte(0xA8, &vendor))
    ESP_LOGI(TAG, "FT5x06 polling at 0x38, vendor %02X", vendor);
  if (this->x_raw_max_ == this->x_raw_min_) {
    this->x_raw_min_ = 0;
    this->x_raw_max_ = 799;
  }
  if (this->y_raw_max_ == this->y_raw_min_) {
    this->y_raw_min_ = 0;
    this->y_raw_max_ = 479;
  }
}
void Touch::update_touches() {
  uint8_t count = 0, data[60]{};
  if (!this->read_byte(0x02, &count) || count > 10 || (count && !this->read_bytes(0x03, data, count * 6))) {
    this->errors_++;
    this->skip_update_ = true;
    this->status_set_warning();
    return;
  }
  this->packets_++;
  this->status_clear_warning();
  for (unsigned i = 0; i < count; i++) {
    const uint8_t *p = data + i * 6;
    const unsigned event = p[0] >> 6, id = p[2] >> 4;
    const int x = ((p[0] & 15) << 8) | p[1], y = ((p[2] & 15) << 8) | p[3];
    if ((event != 0 && event != 2) || id >= 10 || x < this->x_raw_min_ || x > this->x_raw_max_ ||
        y < this->y_raw_min_ || y > this->y_raw_max_)
      continue;
    this->add_raw_touch_position_(id, x, y);
    ESP_LOGD(TAG, "Touch id=%u raw=(%d,%d)", id, x, y);
  }
  if (millis() - this->last_health_ >= 15000) {
    this->last_health_ = millis();
    ESP_LOGI(TAG, "Touch polling: %lu valid, %lu errors", (unsigned long) this->packets_,
             (unsigned long) this->errors_);
  }
}
}  // namespace esphome::waveshare43
