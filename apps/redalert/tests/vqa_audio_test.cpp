#include <cassert>
#include <cstdio>
#include "../pax_sound.cpp"
#include "../pax_vqaaudio.cpp"

extern "C" {
const app_services_t* pax_svc = nullptr;
long long pax_time_us(void) { return 0; }
}
bool VQAMovieDone = false;

int main()
{
    VQAHandle handle = {};
    VQAData data = {};
    SampleTrackerTypeImp voice = {};
    uint8_t pcm[24] = {};
    int16_t loaded[3] = {};
    handle.VQABuf = &data;
    handle.Config.HMIBufSize = 8;
    handle.Config.AudioBufSize = sizeof(pcm);
    data.Audio.Buffer = pcm;
    data.Audio.IsLoaded = loaded;
    data.Audio.NumAudBlocks = 3;
    data.Audio.BuffBytes = sizeof(pcm);
    data.Audio.BitsPerSample = 16;
    data.Audio.Channels = 1;
    data.Audio.SampleRate = 22050;
    AudioVQAHandle = &handle;
    s_voice = &voice;
    s_current_queued = false;
    assert(!Queue_Audio());
    assert(voice.tail == 0); // An unfilled block must never enter the mixer.
    loaded[0] = 1;
    assert(!Queue_Audio());
    assert(voice.tail == 1);
    assert(!Queue_Audio());
    assert(voice.tail == 1); // Waiting for next block must not repeat PCM.
    loaded[1] = 1;
    assert(Queue_Audio());
    assert(voice.tail == 1 && loaded[0] == 0);
    assert(data.Audio.PlayPosition == 8);
    loaded[2] = 1;
    assert(Queue_Audio());
    assert(voice.tail == 2 && loaded[1] == 0);
    puts("Red Alert VQA audio readiness tests passed");
}
