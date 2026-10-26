// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstring>
#include "psram_app.h"

namespace esphome::pax_loader::ble_remote {
// Called only on the ESPHome main loop. The backend owns pairing and reports.
template<class Device>
int dispatch(Device *device, uint32_t command, int32_t a, int32_t b, const char *text) {
  if (device == nullptr) return -1;
  if (command == PAX_BLE_HOST_COUNT) return device->host_slots();
  if (command == PAX_BLE_ACTIVE_HOST) return device->active_host_slot();
  // The P4 backend loads its saved slots after hosted Wi-Fi starts. Mutating
  // slots before that point could replace saved NVS data with empty defaults.
  if (!device->test_ready_)
    return command == PAX_BLE_STATUS || command == PAX_BLE_RELEASE ? 0 : -6;
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
    device->set_slot_broadcast(device->active_host_slot(), a != 0);
    return 0;
  }
  if (command == PAX_BLE_HOST_STATE || command == PAX_BLE_SELECT_HOST || command == PAX_BLE_FORGET_HOST) {
    if (a < 0 || a >= device->host_slots()) return -2;
    if (command == PAX_BLE_HOST_STATE) {
      const bool active = a == device->active_host_slot();
      return (device->get_host_slot(a).occupied ? PAX_BLE_HOST_SAVED : 0) |
             (active ? PAX_BLE_HOST_ACTIVE : 0) |
             (active && device->is_connected() ? PAX_BLE_HOST_CONNECTED : 0) |
             (active && device->is_advertising() ? PAX_BLE_HOST_ADVERTISING : 0) |
             (device->slot_broadcasts(a) ? PAX_BLE_HOST_ENABLED : 0);
    }
    // A host change must never leave keys/buttons held on the previous device.
    device->release_held();device->send_mouse_click_release();device->send_gamepad(0,8);
    if (command == PAX_BLE_FORGET_HOST) {
      // Stay quiet until the user explicitly pairs again; otherwise the former
      // host can immediately reclaim the newly empty slot.
      device->set_slot_broadcast(a,false);device->forget_host(a);return 0;
    }
  }
  if (command == PAX_BLE_PAIR_NEW || command == PAX_BLE_SELECT_HOST) {
    if (command == PAX_BLE_PAIR_NEW) {
      for (a=0;a<device->host_slots();a++)if(!device->get_host_slot(a).occupied)break;
      if (a == device->host_slots()) return -5; // no empty slot; never overwrite a bond
      device->release_held();device->send_mouse_click_release();device->send_gamepad(0,8);
    }
    if (a != device->active_host_slot()) {
      // An inactive slot can be enabled now. switch_host waits for the old link
      // to close before advertising with the new slot's address.
      device->set_slot_broadcast(a,true);device->switch_host(a);
    } else if (!device->slot_broadcasts(a))device->set_slot_broadcast(a,true);
    else device->switch_host(a,true); // selecting a connected host keeps its link
    return a;
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
