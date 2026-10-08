/* Quake II right-stick look through the existing relative-mouse ABI.
 * USB stick axes are -100..100, with negative Y physically up. */
#pragma once
#include <stdint.h>

typedef struct {
  int64_t last_us;
  float fraction_x, fraction_y;
} pax_stick_mouse_t;

static inline int pax_stick_mouse_is_quake2(const char *source) {
  if (!source) return 0;
  for (; *source; ++source) {
    const char *name = "quake2";
    unsigned i = 0;
    while (name[i] && source[i] && (source[i] | 32) == name[i]) ++i;
    if (!name[i]) return 1;
  }
  return 0;
}

static inline float pax_stick_mouse_axis(float value) {
  if (value != value) return 0; /* Sensor not ready / NaN. */
  if (value > 100) value = 100;
  if (value < -100) value = -100;
  if (value > 20) return (value - 20) / 80;
  if (value < -20) return (value + 20) / 80;
  return 0;
}

static inline void pax_stick_mouse_step(pax_stick_mouse_t *state,
                                        float x, float y, int64_t now_us,
                                        int *dx, int *dy) {
  int64_t elapsed = state->last_us ? now_us - state->last_us : 0;
  state->last_us = now_us;
  if (elapsed < 0) elapsed = 0;
  if (elapsed > 200000) elapsed = 200000; /* No spin after a map-loading stall. */
  x = pax_stick_mouse_axis(x);
  y = pax_stick_mouse_axis(y);
  /* At Quake's default sensitivity=3 and m_yaw/m_pitch=.022 this is about
   * 180 degrees/second. Time-based, not dependent on frame/poll frequency. */
  float distance = 2700.0f * (float)elapsed / 1000000.0f;
  state->fraction_x = x ? state->fraction_x + x * distance : 0;
  state->fraction_y = y ? state->fraction_y + y * distance : 0;
  *dx = (int)state->fraction_x;
  *dy = (int)state->fraction_y;
  state->fraction_x -= *dx;
  state->fraction_y -= *dy;
}
