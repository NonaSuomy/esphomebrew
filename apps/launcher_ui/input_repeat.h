#pragma once
#include <stdbool.h>
#include <stdint.h>

/* A tap acts immediately. Held navigation starts repeating after a short
 * delay; a slow frame emits one step rather than a burst of missed steps. */
typedef struct {
  bool held, repeating;
  uint32_t last_ms;
} PaxInputRepeat;

static bool pax_input_repeat_step(PaxInputRepeat *key, bool down, uint32_t now_ms)
{
  if (!down) {
    *key = (PaxInputRepeat){0};
    return false;
  }
  if (!key->held) {
    *key = (PaxInputRepeat){.held = true, .last_ms = now_ms};
    return true;
  }
  const uint32_t interval = key->repeating ? 80U : 400U;
  if ((uint32_t)(now_ms - key->last_ms) < interval) return false;
  key->last_ms = now_ms;
  key->repeating = true;
  return true;
}
