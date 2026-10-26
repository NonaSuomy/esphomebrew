// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstring>
#include "psram_app.h"

namespace esphome::pax_loader::ble_remote {
// Called only on the ESPHome main loop. The backend owns pairing and reports.
template<class Device>
int dispatch(Device *device, uint32_t command, int32_t a, int32_t b, const char *text) {
  if (device == nullptr) return -1;
  if (command == PAX_BLE_STATUS)
    return PAX_BLE_AVAILABLE | (device->is_connected() ? PAX_BLE_CONNECTED : 0) |
           (device->is_paired() ? PAX_BLE_PAIRED : 0) |
           (device->is_advertising() ? PAX_BLE_ADVERTISING : 0);
  if (command == PAX_BLE_RELEASE) {
    device->release_held();
    device->send_mouse_click_release();
    device->send_gamepad(0, 8);
    return 0;
  }
  if (command == PAX_BLE_ADVERTISE) {
    if (a != 0 && a != 1) return -2;
    device->set_slot_broadcast(0, a != 0);
    return 0;
  }
  if (!device->is_connected()) return -4;
  switch (command) {
    case PAX_BLE_TEXT:
      if (text == nullptr || strnlen(text, 129) > 128) return -2;
      device->send_string(text); break;
    case PAX_BLE_KEY:
      if (a < 0 || a > 255 || b < 1 || b > 255) return -2;
      device->send_key_combo(a, b); break;
    case PAX_BLE_MOUSE_MOVE:
      if (a < -127 || a > 127 || b < -127 || b > 127) return -2;
      device->send_mouse_move(a, b); break;
    case PAX_BLE_MOUSE_BUTTONS:
      if (a < 0 || a > 7) return -2;
      if (a) device->send_mouse_click_start(a); else device->send_mouse_click_release();
      break;
    case PAX_BLE_MOUSE_SCROLL:
      if (a < -127 || a > 127) return -2;
      device->send_mouse_scroll(a); break;
    case PAX_BLE_GAMEPAD:
      if (a < 0 || a > 65535 || b < 0 || b > 8) return -2;
      device->send_gamepad(a, b); break;
    default: return -2;
  }
  return 0;
}
}
