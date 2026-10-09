#pragma once
#include <stdint.h>

// Fixed-rate game ticks, independent of expensive display frames. Integer
// phase avoids software floating point and rounding drift. A long stall may
// catch up at most eight ticks; never replay seconds of stale input/logic.
class PaxGameClock {
	uint64_t last{};
	uint32_t phase{};
public:
	void reset(uint64_t now) { last=now; phase=0; }
	unsigned take(uint64_t now, unsigned rate, bool paused) {
		uint64_t elapsed=now-last; last=now;
		if (paused || !rate) { phase=0; return 0; }
		if (elapsed>250000) elapsed=250000;
		const uint64_t total=phase+elapsed*rate;
		phase=total%1000000;
		const unsigned ticks=total/1000000;
		return ticks>8 ? 8 : ticks;
	}
	unsigned wait_us(uint64_t now, unsigned rate) const {
		if (!rate) return 10000;
		const uint64_t elapsed=now-last;
		if (elapsed>250000) return 0;
		const uint64_t total=phase+elapsed*rate;
		return total>=1000000 ? 0 : (1000000-total+rate-1)/rate;
	}
};
