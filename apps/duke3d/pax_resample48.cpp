#include "../stratagus/pax_resample48.h"
static PaxResample48 converter;
extern "C" void duke_resample_reset() { converter = PaxResample48{}; }
extern "C" size_t duke_resample48(const int16_t *input, size_t frames, int16_t *output, size_t capacity) {
  return converter.convert(input,frames,output,capacity);
}
