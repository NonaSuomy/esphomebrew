// Sound backend for the Red Alert PAX (replaces common/soundio_openal.cpp).
//
// soundio_common.cpp decodes the game's samples into PCM chunks of up to 8 KiB
// and queues them per voice through the SoundImp_* calls below, from the game
// task. A mixer task drains the voices, resamples them to the output rate and
// hands stereo blocks to the loader's speaker, paced to real time with a small
// lead (see LEAD_US) and with silence between sounds so the speaker stays on.
//
// Each voice is a small queue with one producer (the game) and one consumer
// (the mixer). A per-voice spinlock guards the queue indices; the mixer only
// ever try-locks, so it never waits on the game.
#include "pax_port.h"

#include "common/soundio_imp.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum
{
    QUEUE_LEN = 3,          // chunks per voice (OpenAL used 2)
    CHUNK_MAX = 8192 + 128, // soundio_common's BUFFER_CHUNK_SIZE plus slack
    MIX_FRAMES = 512,       // per block: ~23 ms at 22,050 Hz; small enough for the speaker ring to accept smoothly
    MAX_VOICES = 8,
};

struct Chunk
{
    uint8_t data[CHUNK_MAX];
    int len;
    int bits, channels, rate; // the format this chunk was queued with
};

struct SampleTrackerTypeImp
{
    Chunk chunks[QUEUE_LEN];
    int lock;
    unsigned head;        // next chunk to play (mixer)
    unsigned tail;        // next free chunk (game)
    uint32_t pos;         // position in the head chunk, frames in 16.16
    volatile bool playing;
    volatile bool armed;  // playback requested; remains set across stream underruns
    volatile int volume;  // 0..65536
    int bits, channels, rate;
};

static SampleTrackerTypeImp* s_voices[MAX_VOICES];
static int s_out_rate = 22050;
static volatile bool s_paused = false;
static volatile bool s_quit = false;
static volatile bool s_mixer_done = true;
static void* s_mixer = nullptr;

static void lock(SampleTrackerTypeImp* st)
{
    while (__atomic_exchange_n(&st->lock, 1, __ATOMIC_ACQUIRE)) {
        // The mixer may be preempted while owning this PSRAM-backed lock.
        // Hammering it with atomic writes from core 0 stalls both cores'
        // external-memory traffic and prevents timely audio/video progress.
        // Block a real tick on either supported firmware tick rate, and only
        // retry the atomic acquisition after the owner has released it.
        do {
            pax_svc->delay_ms(10);
        } while (__atomic_load_n(&st->lock, __ATOMIC_RELAXED));
    }
}

static bool try_lock(SampleTrackerTypeImp* st)
{
    return !__atomic_exchange_n(&st->lock, 1, __ATOMIC_ACQUIRE);
}

static void unlock(SampleTrackerTypeImp* st)
{
    __atomic_store_n(&st->lock, 0, __ATOMIC_RELEASE);
}

static inline int sample_at(const Chunk& c, int frame, int channel)
{
    const int index = frame * c.channels + (c.channels > 1 ? channel : 0);
    if (c.bits == 16) {
        return reinterpret_cast<const int16_t*>(c.data)[index];
    }
    return (static_cast<int>(c.data[index]) - 128) << 8;
}

// Add up to `frames` frames of this voice into acc (stereo). Called with the lock held.
static void mix_voice(SampleTrackerTypeImp* st, int32_t* acc, int frames)
{
    const int volume = st->volume;
    int out = 0;
    while (out < frames) {
        if (st->head == st->tail) {
            st->playing = false; // ran dry, like an OpenAL source stopping
            return;
        }
        const Chunk& c = st->chunks[st->head % QUEUE_LEN];
        const int bytes_per_frame = (c.bits / 8) * c.channels;
        const int count = bytes_per_frame > 0 ? c.len / bytes_per_frame : 0;
        const uint32_t step = (static_cast<uint32_t>(c.rate) << 16) / static_cast<uint32_t>(s_out_rate);
        while (out < frames && static_cast<int>(st->pos >> 16) < count) {
            const int frame = static_cast<int>(st->pos >> 16);
            acc[out * 2] += (sample_at(c, frame, 0) * volume) >> 16;
            acc[out * 2 + 1] += (sample_at(c, frame, 1) * volume) >> 16;
            st->pos += step;
            out++;
        }
        if (static_cast<int>(st->pos >> 16) >= count) {
            st->pos -= static_cast<uint32_t>(count) << 16;
            st->head++;
        }
    }
}

// Keep enough queued audio to ride out occasional frame/render stalls without
// filling the speaker chain so far ahead that video and speech become visibly
// out of sync. This is about seven mixer blocks at 22,050 Hz.
static const long long LEAD_US = 160000;
static const int TICK_MS = 10; // one FreeRTOS tick on this firmware (100 Hz)

static void mixer_task(void*)
{
    static int32_t acc[MIX_FRAMES * 2];
    static int16_t out[MIX_FRAMES * 2];
    long long anchor_us = 0;   // when the current run of audio started
    long long frames_out = 0;  // frames submitted since anchor_us
    // When the silence began (0: something is playing). Start out idle: the
    // chain comes up with the first real sound (the intro movie), as it did
    // before silence was fed; bringing it up while the game loads made it
    // stop and restart right as the intro began.
    long long idle_since = -10000000;
    bool chain_idle = false;   // we let the speaker chain stop (a long silence)
    while (!s_quit) {
        bool any = false;
        memset(acc, 0, sizeof(acc));
        if (!s_paused) {
            for (SampleTrackerTypeImp* st : s_voices) {
                if (st == nullptr || !st->playing || !try_lock(st)) {
                    continue;
                }
                if (st->playing) {
                    mix_voice(st, acc, MIX_FRAMES);
                    any = true;
                }
                unlock(st);
            }
        }
        // Nothing playing: keep sending silence. ESPHome's speaker chain
        // stops itself soon after its input runs dry, and restarting it drops
        // the next sound and stutters the game. Only a long silence (a menu)
        // lets it idle.
        if (any) {
            idle_since = 0;
        } else if (idle_since == 0) {
            idle_since = pax_time_us();
        } else if (pax_time_us() - idle_since > 10000000) {
            if (frames_out != 0) {
                chain_idle = true; // we fed it, then let it stop
            }
            frames_out = 0; // the next sound starts a new run
            pax_svc->delay_ms(10);
            continue;
        }
        for (int i = 0; i < MIX_FRAMES * 2; i++) {
            const int32_t v = acc[i];
            out[i] = static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        // Stay at most LEAD_US ahead of real time since the run started. This
        // alone paces the mixer, speaker or not.
        const long long now = pax_time_us();
        const long long played_us = frames_out * 1000000LL / s_out_rate;
        if (frames_out == 0 || now - anchor_us > played_us + 250000) {
            // A new run (after a long silence the speaker chain has stopped
            // itself), or we fell far behind: start over from now.
            if (chain_idle) {
                pax_svc->audio_init(s_out_rate);
                chain_idle = false;
            }
            anchor_us = now;
            frames_out = 0;
        } else if (played_us - (now - anchor_us) > LEAD_US) {
            // At least one FreeRTOS tick (10 ms): delay_ms() of less rounds
            // down to a bare yield, and the old high-priority mixer spun and
            // starved the presenter on core 1 (the intro ran at 2-8 fps).
            const int ms = static_cast<int>((played_us - (now - anchor_us) - LEAD_US) / 1000) + 1;
            pax_svc->delay_ms(ms < TICK_MS ? TICK_MS : ms);
        }
        frames_out += MIX_FRAMES;
        pax_svc->audio_submit(out, MIX_FRAMES);
        // Submit is void: elapsed time cannot tell us whether it accepted
        // audio. Restarting on a scheduling delay resets a healthy I2S chain
        // and can race its descriptor setup. Only initialize at startup or
        // after a deliberate long idle, not during active playback.
    }
    s_mixer_done = true;
    for (;;) {
        pax_svc->delay_ms(1000); // the loader deletes this task
    }
}

extern "C" void pax_sound_shutdown(void)
{
    if (s_mixer == nullptr) {
        return;
    }
    s_quit = true;
    for (int i = 0; i < 100 && !s_mixer_done; i++) {
        pax_svc->delay_ms(10);
    }
    pax_svc->task_delete(s_mixer);
    s_mixer = nullptr;
}

bool SoundImp_Init(int bits_per_sample, bool stereo, int rate, bool reverse_channels)
{
    (void)bits_per_sample;
    (void)stereo;
    (void)reverse_channels;
    if (s_mixer != nullptr) {
        return true;
    }
    s_out_rate = rate > 0 ? rate : 22050;
    pax_svc->audio_init(s_out_rate);
    s_quit = false;
    s_mixer_done = false;
    // Audio deadlines come before presentation. Contended game-side queue
    // locks block instead of monopolizing PSRAM with atomic busy-spins.
    // The presenter is priority 6: share its core's time slices so a lengthy
    // conversion cannot exclude audio, without starving video with a higher
    // mixer priority. Core 0's priority-3 game leaves the resampler runnable.
    if (pax_svc->task_create(mixer_task, "ra_mixer", 16 * 1024, nullptr, 6, &s_mixer, 1) != 0) {
        s_mixer = nullptr;
        s_mixer_done = true;
        pax_svc->log_printf("RA: could not start the sound mixer\n");
        return false;
    }
    pax_svc->log_printf("RA: sound %d Hz\n", s_out_rate);
    return true;
}

void SoundImp_Shutdown()
{
    pax_sound_shutdown();
    for (SampleTrackerTypeImp*& st : s_voices) {
        free(st);
        st = nullptr;
    }
}

void SoundImp_PauseSound()
{
    s_paused = true;
}

bool SoundImp_ResumeSound()
{
    s_paused = false;
    return true;
}

SampleTrackerTypeImp* SoundImp_Init_Sample(int bits_per_sample, bool stereo, int rate)
{
    for (SampleTrackerTypeImp*& slot : s_voices) {
        if (slot == nullptr) {
            SampleTrackerTypeImp* st = static_cast<SampleTrackerTypeImp*>(calloc(1, sizeof(SampleTrackerTypeImp)));
            if (st != nullptr) {
                st->volume = 65536;
                SoundImp_Set_Sample_Attributes(st, bits_per_sample, stereo, rate);
                slot = st;
            }
            return st;
        }
    }
    return nullptr;
}

// Freed with the mixer stopped, in SoundImp_Shutdown.
void SoundImp_Shutdown_Sample(SampleTrackerTypeImp* st)
{
    SoundImp_Stop_Sample(st);
}

void SoundImp_Set_Sample_Attributes(SampleTrackerTypeImp* st, int bits_per_sample, bool stereo, int rate)
{
    st->bits = bits_per_sample == 8 ? 8 : 16;
    st->channels = stereo ? 2 : 1;
    st->rate = rate > 0 ? rate : s_out_rate;
}

void SoundImp_Set_Sample_Volume(SampleTrackerTypeImp* st, unsigned int volume)
{
    st->volume = volume > 65536 ? 65536 : static_cast<int>(volume);
}

int SoundImp_Get_Sample_Free_Buffer_Count(SampleTrackerTypeImp* st)
{
    lock(st);
    const int free_chunks = QUEUE_LEN - static_cast<int>(st->tail - st->head);
    unlock(st);
    return free_chunks;
}

void SoundImp_Buffer_Sample_Data(SampleTrackerTypeImp* st, const void* data, size_t datalen)
{
    if (SoundImp_Get_Sample_Free_Buffer_Count(st) <= 0) {
        return;
    }
    // The slot at tail is not the mixer's until tail moves past it.
    Chunk& c = st->chunks[st->tail % QUEUE_LEN];
    c.len = static_cast<int>(datalen < CHUNK_MAX ? datalen : CHUNK_MAX);
    memcpy(c.data, data, c.len);
    c.bits = st->bits;
    c.channels = st->channels;
    c.rate = st->rate;
    lock(st);
    st->tail++;
    // The mixer can drain a stream while the game refills it. Keep playback
    // armed after the initial explicit Start, so newly queued music resumes;
    // don't start voices that are still being initially pre-buffered.
    if (st->armed) {
        st->playing = true;
    }
    unlock(st);
}

void SoundImp_Start_Sample(SampleTrackerTypeImp* st)
{
    lock(st);
    st->armed = true;
    st->playing = st->head != st->tail;
    unlock(st);
}

void SoundImp_Stop_Sample(SampleTrackerTypeImp* st)
{
    lock(st);
    st->armed = false;
    st->playing = false;
    st->head = st->tail;
    st->pos = 0;
    unlock(st);
}

bool SoundImp_Sample_Status(SampleTrackerTypeImp* st)
{
    return st->playing;
}
