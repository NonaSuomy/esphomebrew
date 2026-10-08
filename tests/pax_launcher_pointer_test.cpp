#include "pax_launcher_pointer.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
  esphome::pax_launcher::Pointer pointer;
  pointer.button(true, true);
  assert(!pointer.pressed(true));  // Held at boot: release before activation.
  pointer.button(false, true);
  pointer.button(true, true);
  assert(pointer.pressed(true));
  pointer.move(25, -10, true);
  assert(pointer.x == 537 && pointer.y == 290);
  pointer.move(100000, -100000, true);
  assert(pointer.x == 1023 && pointer.y == 0);
  pointer.move(-100000, 100000, true);
  assert(pointer.x == 0 && pointer.y == 599);
  pointer.move(std::numeric_limits<float>::quiet_NaN(), 1, true);
  assert(pointer.x == 0 && pointer.y == 599);
  pointer.move(30, -30, false);
  assert(pointer.x == 0 && pointer.y == 599);
  assert(!pointer.pressed(false));
  assert(!pointer.pressed(true));  // No held-click carryover when a PAX exits.
  pointer.button(false, false);
  pointer.button(true, true);
  assert(pointer.pressed(true));
  pointer.button(false, true);
  assert(!pointer.pressed(true));
  puts("PASS: launcher pointer bounds, finite input, boot and PAX release guards");
}
