#include "pax_launcher_regions.h"
#include <cassert>
#include <cstdio>
using namespace esphome::pax_launcher;
int main() {
  assert(owns_pointer("http://host/launcher_ui.pax?cache=1"));
  assert(owns_pointer("/sd/roms/pax/launcher_ui.pax"));
  assert(!owns_pointer("/sd/roms/pax/redalert.pax"));
  assert(!owns_pointer("/launcher_ui.pax/redalert.pax"));
  assert(!owns_pointer("http://host/redalert.pax?ref=launcher_ui.pax"));
  DirtyRegion region;
  region.add(-1, -1, 3, 3, 4, 3);
  assert(region.x == 0 && region.y == 0 && region.w == 2 && region.h == 2);
  region.add(3, 2, 10, 10, 4, 3);
  assert(region.w == 4 && region.h == 3);
  region.clear();
  region.add(1, 0, 2, 2, 4, 3);
  const uint16_t pixels[] = {0,1,2,3,4,5,6,7,8,9,10,11};
  uint16_t output[4];
  region.rotated_pixels(pixels, 4, output);
  assert(output[0] == 6 && output[1] == 5 && output[2] == 2 && output[3] == 1);
  region.add(99, 99, 1, 1, 4, 3);
  assert(region.x == 1 && region.w == 2);
  puts("PASS: pointer ownership, dirty-region union/clipping and 180-degree pixels");
}
