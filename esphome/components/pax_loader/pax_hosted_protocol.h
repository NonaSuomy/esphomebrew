/*
 * Stand-alone copy of the P4 Bluepad32/ESP-Hosted wire contract.
 *
 * This header is deliberately dependency-free so it can be copied into the
 * ESP-Hosted C6 slave project alongside the Bluepad32 source.
 */
#pragma once

#include <stdint.h>

#define BLUEPAD32_HOSTED_MSG_ID 0x42503332u
#define BLUEPAD32_HOSTED_MAGIC 0x4250u
#define BLUEPAD32_HOSTED_VERSION 1u
#define BLUEPAD32_HOSTED_KEYBOARD_MSG_ID 0x4250334bu /* BP3K */
#define BLUEPAD32_HOSTED_MOUSE_MSG_ID 0x4250334du    /* BP3M */
#define BLUEPAD32_HOSTED_KEYBOARD_KEYS 10u
#define BLUEPAD32_HOSTED_MAX_DEVICES 4u
#define BLUEPAD32_HOSTED_SLOT_INVALID 0xffu

#define BLUEPAD32_HOSTED_BTN_A (1u << 0)
#define BLUEPAD32_HOSTED_BTN_B (1u << 1)
#define BLUEPAD32_HOSTED_BTN_X (1u << 2)
#define BLUEPAD32_HOSTED_BTN_Y (1u << 3)
#define BLUEPAD32_HOSTED_BTN_L1 (1u << 4)
#define BLUEPAD32_HOSTED_BTN_R1 (1u << 5)
#define BLUEPAD32_HOSTED_BTN_L2 (1u << 6)
#define BLUEPAD32_HOSTED_BTN_R2 (1u << 7)
#define BLUEPAD32_HOSTED_BTN_SELECT (1u << 8)
#define BLUEPAD32_HOSTED_BTN_START (1u << 9)
#define BLUEPAD32_HOSTED_BTN_L3 (1u << 10)
#define BLUEPAD32_HOSTED_BTN_R3 (1u << 11)
#define BLUEPAD32_HOSTED_BTN_HOME (1u << 12)
#define BLUEPAD32_HOSTED_BTN_MISC (1u << 13)

#define BLUEPAD32_HOSTED_DPAD_UP 0x01u
#define BLUEPAD32_HOSTED_DPAD_DOWN 0x02u
#define BLUEPAD32_HOSTED_DPAD_LEFT 0x04u
#define BLUEPAD32_HOSTED_DPAD_RIGHT 0x08u

typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t version;
  uint8_t connected;
  uint32_t buttons;
  uint8_t dpad;
  int16_t axis_lx;
  int16_t axis_ly;
  int16_t axis_rx;
  int16_t axis_ry;
  uint16_t brake;
  uint16_t throttle;
  uint8_t battery;
  uint8_t reserved;
  uint8_t slot;
  uint16_t sequence;
} bluepad32_hosted_state_t;

/* BLE keyboard reports are sent separately so the fixed-size gamepad packet
 * remains independent. pressed_keys contain HID usage IDs. */
typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t version;
  uint8_t connected;
  uint8_t modifiers;
  uint8_t key_count;
  uint8_t pressed_keys[BLUEPAD32_HOSTED_KEYBOARD_KEYS];
  uint8_t battery;
  uint8_t reserved;
  uint16_t sequence;
} bluepad32_hosted_keyboard_t;

/* BLE mouse/touchpad reports carry relative motion and button state. */
typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t version;
  uint8_t connected;
  int32_t delta_x;
  int32_t delta_y;
  uint16_t buttons;
  int8_t scroll_wheel;
  uint8_t misc_buttons;
  uint8_t battery;
  uint8_t reserved;
  uint16_t sequence;
} bluepad32_hosted_mouse_t;

#if defined(__cplusplus)
static_assert(sizeof(bluepad32_hosted_state_t) == 26,
              "Bluepad32 hosted packet layout changed");
#else
_Static_assert(sizeof(bluepad32_hosted_state_t) == 26,
               "Bluepad32 hosted packet layout changed");
_Static_assert(sizeof(bluepad32_hosted_keyboard_t) == 20,
               "Bluepad32 hosted keyboard packet layout changed");
_Static_assert(sizeof(bluepad32_hosted_mouse_t) == 20,
               "Bluepad32 hosted mouse packet layout changed");
#endif
