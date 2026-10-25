// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#define PAX_BT_DISCOVERY_MSG_ID UINT32_C(0x50424431)
#define PAX_BT_DISCOVERY_COMMAND_ID UINT32_C(0x50424443)
#define PAX_BT_DISCOVERY_VERSION 1
#define PAX_BT_SCAN 1
#define PAX_BT_CONNECT 2
#define PAX_BT_DEVICE 1
#define PAX_BT_SCAN_DONE 2
#define PAX_BT_HEARTBEAT 3
#define PAX_BT_PAIRING_CODE 4
// Independent protocol: existing input packets and numeric PAX ABI stay intact.
typedef struct __attribute__((packed)) {
 uint8_t version, operation;
 uint16_t generation;
 uint8_t address[6];
 uint8_t ble, reserved;
} pax_bt_discovery_command_t;
typedef struct __attribute__((packed)) {
 uint8_t version, event;
 uint16_t generation;
 uint8_t address[6];
 uint8_t ble, connected;
 int8_t rssi;
 uint8_t hid; // only supported input devices may be connected
 uint8_t capabilities; // 1=BLE, 2=Classic; reported by the companion radio
 char name[48];
} pax_bt_discovery_event_t;
static inline int pax_bt_discovery_command_valid(const void *bytes,size_t size) {
 if(!bytes || size!=sizeof(pax_bt_discovery_command_t))return 0;
 const pax_bt_discovery_command_t *p=(const pax_bt_discovery_command_t *)bytes;
 return p->version==PAX_BT_DISCOVERY_VERSION && (p->operation==PAX_BT_SCAN || p->operation==PAX_BT_CONNECT) && p->ble<=1 && !p->reserved;
}
