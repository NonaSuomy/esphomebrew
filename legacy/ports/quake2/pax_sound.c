#include "psram_app.h"
#include "qcommon/qcommon.h"
#include "client/snd_loc.h"
#include "pax_music.h"
#include <stdint.h>
#include <string.h>

extern const app_services_t *_pax_svc;

enum { SOUND_RATE = 22050, BLOCK_FRAMES = 512, RING_FRAMES = 16384 };
static short dma_buf[RING_FRAMES * 2];
static short output_buf[BLOCK_FRAMES * 2];
static uint32_t read_frames;
static uint32_t mixed_frames;
static int sound_on;
static int task_done;
static void *sound_task_handle;

/* The game only paints ahead into the ring. A separate task drains it at
 * the audio rate, rather than emitting 23 ms of PCM once per video frame.
 * Publish the completed paint cursor with release/acquire ordering so the
 * second core never reads a partly painted block. */
static void sound_task(void *arg) {
    (void)arg;
    unsigned blocks = 0, starved = 0;
    int peak = 0;
    int64_t started = _pax_svc->get_time_us();
    int64_t deadline = started;
    int remainder = 0;
    while (__atomic_load_n(&sound_on, __ATOMIC_ACQUIRE)) {
        const uint32_t first = __atomic_load_n(&read_frames, __ATOMIC_RELAXED);
        const uint32_t end = __atomic_load_n(&mixed_frames, __ATOMIC_ACQUIRE);
        unsigned available = end > first ? end - first : 0;
        if (available > BLOCK_FRAMES) available = BLOCK_FRAMES;
        for (unsigned i = 0; i < available; ++i) {
            const unsigned at = ((first + i) & (RING_FRAMES - 1)) * 2;
            output_buf[i * 2] = dma_buf[at];
            output_buf[i * 2 + 1] = dma_buf[at + 1];
        }
        /* On a map-loading stall, emit silence, not stale samples from the
         * previous ring lap. The absolute cursor lets mixing catch up after
         * a stall longer than one whole ring (ordinary DMA wrap inference
         * cannot distinguish that from a short frame). */
        memset(output_buf + available * 2, 0,
               (BLOCK_FRAMES - available) * 2 * sizeof(short));
        starved += BLOCK_FRAMES - available;
        pax_music_mix(output_buf, BLOCK_FRAMES);
        for (unsigned i = 0; i < BLOCK_FRAMES * 2; ++i) {
            int magnitude = output_buf[i] < 0 ? -output_buf[i] : output_buf[i];
            if (magnitude > peak) peak = magnitude;
        }
        _pax_svc->audio_submit(output_buf, BLOCK_FRAMES);
        __atomic_store_n(&read_frames, first + BLOCK_FRAMES, __ATOMIC_RELEASE);
        ++blocks;

        /* Speaker writes already apply back-pressure when its queue is
         * full. Pace fast writes too, keeping the fractional microseconds
         * rather than rounding 512/22050 down on every block. */
        remainder += (BLOCK_FRAMES * 1000000) % SOUND_RATE;
        deadline += (BLOCK_FRAMES * 1000000) / SOUND_RATE;
        if (remainder >= SOUND_RATE) {
            deadline++;
            remainder -= SOUND_RATE;
        }
        int64_t now = _pax_svc->get_time_us();
        if (deadline < now - 25000) deadline = now;
        while (__atomic_load_n(&sound_on, __ATOMIC_ACQUIRE) && now < deadline) {
            int ms = (int)((deadline - now + 999) / 1000);
            _pax_svc->delay_ms(ms > 10 ? 10 : ms);
            now = _pax_svc->get_time_us();
        }
        if (blocks % 220 == 0 && _pax_svc->log_printf) {
            const int64_t elapsed = now - started;
            const unsigned rate = elapsed > 0 ? (unsigned)(220LL * BLOCK_FRAMES * 1000000 / elapsed) : 0;
            _pax_svc->log_printf("QUAKE2 AUDIO STREAM blocks=%u rate=%u Hz underrun=%u/%u peak=%d\n",
                                  blocks, rate, starved, 220 * BLOCK_FRAMES, peak);
            started = now;
            starved = 0;
            peak = 0;
        }
    }
    __atomic_store_n(&task_done, 1, __ATOMIC_RELEASE);
    /* The owner deletes the task after observing this flag, before the
     * loader can unmap the app's code and static PCM buffers. */
    for (;;) _pax_svc->delay_ms(1000);
}

qboolean SNDDMA_Init(void) {
    if (!_pax_svc->audio_init || !_pax_svc->audio_submit ||
        !_pax_svc->task_create || !_pax_svc->task_delete) return false;
    _pax_svc->audio_init(SOUND_RATE);
    memset(dma_buf, 0, sizeof dma_buf);
    __atomic_store_n(&read_frames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&mixed_frames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&task_done, 0, __ATOMIC_RELAXED);
    dma.buffer = (unsigned char *)dma_buf;
    dma.samplebits = 16;
    dma.speed = SOUND_RATE;
    dma.channels = 2;
    dma.samples = RING_FRAMES * 2; /* mono samples, NOT stereo frames */
    dma.submission_chunk = 1;
    dma.samplepos = 0;
    sound_task_handle = NULL;
    __atomic_store_n(&sound_on, 1, __ATOMIC_RELEASE);
    if (_pax_svc->task_create(sound_task, "q2_audio", 8192, NULL, 6,
                               &sound_task_handle, 1) != 0) {
        __atomic_store_n(&sound_on, 0, __ATOMIC_RELEASE);
        dma.buffer = NULL;
        if (_pax_svc->log_printf) _pax_svc->log_printf("QUAKE2 AUDIO task creation failed\n");
        return false;
    }
    if (_pax_svc->log_printf)
        _pax_svc->log_printf("QUAKE2 AUDIO INIT 22050 Hz stereo, independent playback task\n");
    return true;
}

int pax_sound_time(void) {
    return (int)__atomic_load_n(&read_frames, __ATOMIC_ACQUIRE);
}

int SNDDMA_GetDMAPos(void) {
    dma.samplepos = (pax_sound_time() & (RING_FRAMES - 1)) * 2;
    return dma.samplepos;
}

void SNDDMA_Shutdown(void) {
    __atomic_store_n(&sound_on, 0, __ATOMIC_RELEASE);
    if (sound_task_handle) {
        /* A speaker write is bounded by the loader's 750 ms timeout. */
        for (int ms = 0; ms < 1000 && !__atomic_load_n(&task_done, __ATOMIC_ACQUIRE); ms += 10)
            _pax_svc->delay_ms(10);
        _pax_svc->task_delete(sound_task_handle);
        sound_task_handle = NULL;
    }
}

void SNDDMA_BeginPainting(void) {}

void SNDDMA_Submit(void) {
    __atomic_store_n(&mixed_frames, (uint32_t)paintedtime, __ATOMIC_RELEASE);
}

void pax_sound_shutdown(void) {
    SNDDMA_Shutdown();
    pax_music_shutdown();
}
