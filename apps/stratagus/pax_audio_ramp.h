#pragma once

// Keep a voice replacement continuous even if the preceding clip was cut
// in its middle. State is owned only by the audio task, not the UI task.
class PaxVoiceRamp {
	int previous[2]{}, start[2]{}, remaining{};
public:
	void replace() {
		start[0]=previous[0]; start[1]=previous[1]; remaining=110;
	}
	int sample(int value, int channel) {
		if (remaining) value=(start[channel]*remaining+value*(110-remaining))/110;
		previous[channel]=value;
		if (channel==1 && remaining) --remaining;
		return value;
	}
};
