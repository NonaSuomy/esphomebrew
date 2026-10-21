#pragma once
#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/touchscreen/touchscreen.h"
namespace esphome::waveshare43 {
class Control : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  float get_setup_priority() const override { return 810; }
  void finish_setup();
  void set_brightness(float value);

 protected:
  bool write_control_(uint8_t reg, uint8_t value);
  bool ready_{false};
  float brightness_{0.5f};
};
class Finalizer : public Component {
 public:
  explicit Finalizer(Control *parent) : parent_(parent) {}
  void setup() override { this->parent_->finish_setup(); }
  float get_setup_priority() const override { return 599; }

 protected:
  Control *parent_;
};
class Touch : public touchscreen::Touchscreen, public i2c::I2CDevice {
 public:
  void setup() override;
  float get_setup_priority() const override { return 580; }

 protected:
  void update_touches() override;
  uint32_t errors_{0};
  uint32_t packets_{0};
  uint32_t last_health_{0};
};
}  // namespace esphome::waveshare43
