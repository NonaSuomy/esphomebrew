#pragma once
#include <stdint.h>
/* Wall times in microseconds. Render/presentation measurements overlap;
 * action-to-flush ends at the host flush call, not at physical LCD scanout. */
typedef struct {
  uint64_t total;
  uint32_t samples, maximum, over_16ms;
} PaxTiming;
static void timing_record(PaxTiming *timing, uint64_t elapsed)
{
  uint32_t sample=elapsed>UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
  timing->samples++;timing->total+=sample;
  if(sample>timing->maximum)timing->maximum=sample;
  if(sample>16000)timing->over_16ms++;
}
