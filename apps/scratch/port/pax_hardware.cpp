#include "pax_port.h"
#include <blockUtils.hpp>
#include <cmath>
#include <input.hpp>
static int lastHardwareStatus = -1;
static unsigned hardwareErrorReports = 0;
static unsigned hardwareCommandReports = 0;
static int writeHardware(const char *name, float value);
static BlockResult readHardware(const char *name, Value *out) {
  float value = 0;
  lastHardwareStatus =
      pax_svc->hardware_read ? pax_svc->hardware_read(name, &value) : -1;
  if (lastHardwareStatus == 0 && std::isfinite(value))
    *out = Value((double)value);
  else
    *out = Value(std::string("unavailable"));
  return BlockResult::CONTINUE;
}
SCRATCH_BLOCK(paxio, read) {
  std::string name;
  if (!Scratch::getInputValueAs(block, "CHANNEL", thread, sprite, name))
    return BlockResult::REPEAT;
  return readHardware(name.c_str(), outValue);
}
SCRATCH_BLOCK_BOOLEAN(paxio, available) {
  std::string name;
  float value;
  if (!Scratch::getInputValueAs(block, "CHANNEL", thread, sprite, name))
    return BlockResult::REPEAT;
  *outValue = pax_svc->hardware_read &&
              pax_svc->hardware_read(name.c_str(), &value) == 0;
  return BlockResult::CONTINUE;
}
SCRATCH_BLOCK(paxio, write) {
  std::string name;
  double value;
  if (!Scratch::getInputValueAs(block, "CHANNEL", thread, sprite, name) ||
      !Scratch::getInputValueAs(block, "VALUE", thread, sprite, value))
    return BlockResult::REPEAT;
  lastHardwareStatus = writeHardware(name.c_str(), (float)value);
  return BlockResult::CONTINUE;
}
SCRATCH_BLOCK_DOUBLE(paxio, status) {
  *outValue = lastHardwareStatus;
  return BlockResult::CONTINUE;
}
#define BOARD_READER(id)                                                       \
  SCRATCH_BLOCK(paxio, id) { return readHardware(#id, outValue); }
BOARD_READER(distance_cm)
BOARD_READER(light_lux)
BOARD_READER(temperature_c)
BOARD_READER(humidity_pct)
BOARD_READER(accel_x)
BOARD_READER(accel_y)
BOARD_READER(accel_z)
BOARD_READER(gyro_x)
BOARD_READER(gyro_y)
BOARD_READER(gyro_z)
BOARD_READER(motion)
BOARD_READER(hall)
BOARD_READER(touch_pad)
BOARD_READER(gas_voltage)
BOARD_READER(buttons_voltage)
BOARD_READER(microphone_level)
BOARD_READER(camera_ready)
SCRATCH_BLOCK_BOOLEAN(paxio, button) {
  auto name = Scratch::getFieldValue(*block, "BUTTON");
  *outValue = std::find(Input::inputButtons.begin(), Input::inputButtons.end(),
                        name) != Input::inputButtons.end();
  return BlockResult::CONTINUE;
}

static int writeHardware(const char *name, float value) {
  int result =
      pax_svc->hardware_write ? pax_svc->hardware_write(name, value) : -1;
  if (result != 0 && hardwareErrorReports++ < 8)
    pax_svc->log_printf("Scratch: hardware write %s failed (%d)\n", name,
                        result);
  if (result == 0 && hardwareCommandReports++ < 24)
    pax_svc->log_printf("Scratch: hardware command queued %s=%g\n", name, (double)value);
  return result;
}
#define BOARD_WRITER(id, channel)                                              \
  SCRATCH_BLOCK(paxio, id) {                                                   \
    double value;                                                              \
    if (!Scratch::getInputValueAs(block, "VALUE", thread, sprite, value))      \
      return BlockResult::REPEAT;                                              \
    lastHardwareStatus = writeHardware(channel, (float)value);                 \
    return BlockResult::CONTINUE;                                              \
  }
BOARD_WRITER(relay, "relay")
BOARD_WRITER(servo, "servo")
BOARD_WRITER(microphone, "microphone_enabled")
SCRATCH_BLOCK(paxio, rgbw) {
  double r, g, b, w;
  if (!Scratch::getInputValueAs(block, "R", thread, sprite, r) ||
      !Scratch::getInputValueAs(block, "G", thread, sprite, g) ||
      !Scratch::getInputValueAs(block, "B", thread, sprite, b) ||
      !Scratch::getInputValueAs(block, "W", thread, sprite, w))
    return BlockResult::REPEAT;
  if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) ||
      !std::isfinite(w) || r < 0 || g < 0 || b < 0 || w < 0 || r > 100 ||
      g > 100 || b > 100 || w > 100) {
    lastHardwareStatus = -2;
    return BlockResult::CONTINUE;
  }
  int results[] = {writeHardware("led_red", r), writeHardware("led_green", g),
                   writeHardware("led_blue", b), writeHardware("led_white", w),
                   writeHardware("led_brightness", 100)};
  lastHardwareStatus = 0;
  for (int result : results)
    if (result)
      lastHardwareStatus = result;
  return BlockResult::CONTINUE;
}
SCRATCH_BLOCK(paxio, camera) {
  lastHardwareStatus = writeHardware("camera_capture", 1);
  return BlockResult::CONTINUE;
}
