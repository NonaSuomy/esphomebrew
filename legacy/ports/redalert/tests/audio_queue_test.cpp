// Compile with the repository and Vanilla-Conquer roots on the include path.
#include <cassert>
#include <cstdio>
#include "../pax_sound.cpp"

extern "C" {
const app_services_t* pax_svc = nullptr;
long long pax_time_us(void) { return 0; }
}

static SampleTrackerTypeImp* held_voice;
static int waits;
static void release_after_wait(int ms)
{
    assert(ms >= 10);
    ++waits;
    if (waits == 2) unlock(held_voice);
}

int main()
{
    SampleTrackerTypeImp st = {};
    app_services_t services = {};
    services.delay_ms = release_after_wait;
    pax_svc = &services;
    // Simulate a preempted mixer holding the lock. The producer must block
    // until it is released, then acquire it normally without busy-spinning.
    held_voice = &st;
    st.lock = 1;
    lock(&st);
    assert(waits == 2 && st.lock == 1);
    unlock(&st);
    st.volume = 65536;
    SoundImp_Set_Sample_Attributes(&st, 16, false, 22050);
    const int16_t pcm[] = {1000, -2000, 3000};
    int32_t mixed[8] = {};

    SoundImp_Buffer_Sample_Data(&st, pcm, sizeof(pcm));
    assert(!SoundImp_Sample_Status(&st)); // pre-buffering is not playback
    SoundImp_Start_Sample(&st);
    mix_voice(&st, mixed, 4);
    assert(mixed[0] == 1000 && mixed[1] == 1000);
    assert(mixed[2] == -2000 && mixed[3] == -2000);
    assert(mixed[4] == 3000 && mixed[5] == 3000);
    assert(mixed[6] == 0 && mixed[7] == 0);
    assert(!st.playing && st.armed);
    assert(SoundImp_Get_Sample_Free_Buffer_Count(&st) == QUEUE_LEN);

    SoundImp_Buffer_Sample_Data(&st, pcm, sizeof(pcm));
    assert(st.playing); // starvation/refill resumes without another Start
    SoundImp_Stop_Sample(&st);
    assert(!st.armed && !st.playing && st.pos == 0);
    SoundImp_Buffer_Sample_Data(&st, pcm, sizeof(pcm));
    assert(!st.playing); // Stop disarms automatic restart

    SoundImp_Stop_Sample(&st);
    for (int i = 0; i < QUEUE_LEN; ++i)
        SoundImp_Buffer_Sample_Data(&st, pcm, sizeof(pcm));
    assert(SoundImp_Get_Sample_Free_Buffer_Count(&st) == 0);
    SoundImp_Buffer_Sample_Data(&st, pcm, sizeof(pcm));
    assert(st.tail - st.head == QUEUE_LEN); // never overwrite a full ring
    puts("Red Alert audio queue tests passed");
}
