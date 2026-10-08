#include "input_recovery.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using esphome::usb_hidx::InputRecovery;

int main() {
  InputRecovery idle;
  for (uint32_t now : {0U, 1000U, 86400000U, 0xffffffffU})
    assert(!idle.ready(now));  // No inactivity watchdog: a sleeping HID is healthy.

  InputRecovery keyboard, media;
  keyboard.schedule(1000, true);
  assert(keyboard.pending && keyboard.clear_host_halt && !keyboard.clear_device_halt);
  assert(!keyboard.ready(1249) && keyboard.ready(1250));
  assert(keyboard.report_failure());
  assert(!media.pending);  // Recover one endpoint, not the entire hub.
  keyboard.submitted();
  assert(!keyboard.pending && !keyboard.clear_host_halt && keyboard.failures == 1);
  keyboard.schedule(2000);  // Another failed completion must back off further.
  assert(!keyboard.ready(2499) && keyboard.ready(2500));
  assert(keyboard.report_failure());
  keyboard.schedule(3000);
  assert(!keyboard.report_failure());
  assert(!keyboard.ready(3999) && keyboard.ready(4000));
  keyboard.healthy();
  assert(!keyboard.pending && keyboard.failures == 0);
  keyboard.schedule(10000);
  assert(keyboard.retry_at == 10250);  // Successful input resets the backoff.

  media.schedule(0, true, true);  // Device STALL needs CLEAR_FEATURE before host clear.
  assert(media.clear_device_halt);
  media.schedule(250);  // Failed control completion preserves both halt flags.
  assert(media.clear_device_halt && media.clear_host_halt);
  media.clear_device_halt = false;
  media.submitted();
  assert(!media.pending && !media.clear_host_halt && !media.clear_device_halt);

  InputRecovery repeated;
  for (unsigned i = 0; i < 100000; ++i) {
    repeated.schedule(1000000);
    assert(repeated.retry_at - 1000000 <= 30000);
  }
  assert(repeated.failures == UINT16_MAX && repeated.retry_at == 1030000);
  InputRecovery rollover;
  rollover.schedule(0xffffff80U);
  assert(!rollover.ready(0xffffffffU));
  assert(!rollover.ready(0x79U) && rollover.ready(0x7aU));
  puts("PASS: idle preservation, per-endpoint recovery, submit/completion state, stall flags, backoff cap and millis wrap");
}
