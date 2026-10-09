#pragma once
#include <stdint.h>
#include <stddef.h>

// Continuous, integer-only 22050 -> 48000 stereo conversion. Warcraft
// mixes at the extracted files' native rate; the device speaker is 48 kHz.
// Feeding that rate directly avoids the expensive firmware FIR resampler.
class PaxResample48 {
	uint32_t phase{};
	int16_t previous[2]{};
	bool started{};
	static int16_t interpolate(int16_t a,int16_t b,uint32_t phase) {
		const int difference=int(b)-int(a);
		const uint32_t magnitude=difference<0 ? -difference:difference;
		const int delta=int(magnitude*phase/48000u);
		return int16_t(int(a)+(difference<0 ? -delta:delta));
	}
public:
	size_t convert(const int16_t *input,size_t frames,int16_t *output,size_t capacity) {
		if (!frames || !input || !output || frames>(SIZE_MAX-22049)/48000 ||
			capacity<(frames*48000+22049-phase)/22050) return 0;
		size_t count=0;
		for (size_t i=0;i<frames;++i) {
			const int16_t *current=input+i*2;
			if (!started) { previous[0]=current[0]; previous[1]=current[1]; started=true; }
			while (phase<48000) {
				output[count*2]=interpolate(previous[0],current[0],phase);
				output[count*2+1]=interpolate(previous[1],current[1],phase);
				++count; phase+=22050;
			}
			phase-=48000; previous[0]=current[0]; previous[1]=current[1];
		}
		return count;
	}
};
