#pragma once
struct PaxAudioGain { int left, right; };
// World pixels, not screen pixels: panning remains stable as the camera moves.
inline PaxAudioGain pax_audio_gain(int x,int y,int left,int top,int width,int height)
{
	if (width<=0 || height<=0) return {0,0};
	const int dx=x<left ? left-x : x>left+width ? x-left-width : 0;
	const int dy=y<top ? top-y : y>top+height ? y-top-height : 0;
	const int distance=dx>dy?dx:dy;
	if (distance>=128) return {0,0};
	const int gain=255*(128-distance)/128;
	int pan=(x-left)*256/width-128;
	if (pan<-128) pan=-128; if (pan>128) pan=128;
	return {gain*(pan>0?128-pan:128)/128,gain*(pan<0?128+pan:128)/128};
}
