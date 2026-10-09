#ifndef PAX_TOUCH_SAMPLES_H
#define PAX_TOUCH_SAMPLES_H
#include <stdint.h>

struct PaxTouchSample { int x{}, y{}; uint32_t ticks{}; bool down{}; };
struct PaxMouseSample { int dx{}, dy{}, buttons{}; uint32_t ticks{}; };

// One sampler producer and one game-thread consumer. A short press and its
// release survive even when both happen between slow game frames.
template<class Sample> class PaxSampleQueue {
	Sample samples[64]{};
	unsigned read{}, write{};
public:
	bool push(const Sample &sample) {
		const unsigned w=__atomic_load_n(&write, __ATOMIC_RELAXED);
		if (w-__atomic_load_n(&read, __ATOMIC_ACQUIRE)==64) return false;
		samples[w%64]=sample;
		__atomic_store_n(&write,w+1,__ATOMIC_RELEASE);
		return true;
	}
	bool pop(Sample &sample) {
		const unsigned r=__atomic_load_n(&read,__ATOMIC_RELAXED);
		if (r==__atomic_load_n(&write,__ATOMIC_ACQUIRE)) return false;
		sample=samples[r%64];
		__atomic_store_n(&read,r+1,__ATOMIC_RELEASE);
		return true;
	}
	bool peek(Sample &sample) const {
		const unsigned r=__atomic_load_n(&read,__ATOMIC_RELAXED);
		if (r==__atomic_load_n(&write,__ATOMIC_ACQUIRE)) return false;
		sample=samples[r%64]; return true;
	}
};
using PaxTouchQueue = PaxSampleQueue<PaxTouchSample>;
using PaxMouseQueue = PaxSampleQueue<PaxMouseSample>;

static inline bool pax_pop_mouse(PaxMouseQueue &queue, PaxMouseSample &sample, int previous_buttons)
{
	if (!queue.pop(sample)) return false;
	// Keep the press position itself: merging later held movement into a
	// newly pressed sample would erase the beginning of a selection drag.
	if (sample.buttons!=previous_buttons) return true;
	PaxMouseSample next;
	// A slow render must not make the cursor replay dozens of stale motion
	// packets. Coalesce movement, but never cross a press/release boundary.
	for (unsigned i=1; i<64 && queue.peek(next) && next.buttons==sample.buttons; ++i) {
		queue.pop(next); sample.dx+=next.dx; sample.dy+=next.dy; sample.ticks=next.ticks;
	}
	return true;
}

// A cached touchscreen sample can briefly report RELEASED while the finger
// is still down. Keep the original release time, but wait 40 ms before
// publishing it so one gesture does not turn into several clicks.
class PaxTouchFilter {
	PaxTouchSample previous{}, release{};
	bool releasing{};
public:
	void update(PaxTouchSample sample, PaxTouchQueue &queue) {
		const int dx=sample.x-previous.x, dy=sample.y-previous.y;
		if (releasing && (sample.ticks-release.ticks>=40 ||
			(sample.down && dx*dx+dy*dy>=64))) {
			if (!queue.push(release)) return;
			previous=release; releasing=false;
		}
		if (sample.down) {
			releasing=false;
			if ((!previous.down || dx || dy) && queue.push(sample)) previous=sample;
		} else if (previous.down && !releasing) {
			release=previous; release.down=false; release.ticks=sample.ticks;
			releasing=true;
		}
	}
};
#endif
