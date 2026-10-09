/* Host regression: a 9 FPS producer must feed a 22050 Hz output task, with
 * correct stereo cursor units, ring wrapping and recovery from map stalls.
 * Build with the cached upstream source and loader SDK include directories. */
#include "../pax_sound.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

dma_t dma;
int paintedtime;
const app_services_t *_pax_svc;
static int64_t clock_us, next_mix;
static unsigned submitted, silence, deletes;
static int stall, fail_create;
static jmp_buf finished;
static void (*created_task)(void *);

/* Music has its own decoder/ring regression. Keep the SFX clock test isolated. */
void pax_music_mix(int16_t *pcm, unsigned frames) { (void)pcm; (void)frames; }
void pax_music_shutdown(void) {}

static int64_t fake_time(void) { return clock_us; }
static short sample_at(unsigned frame) { return (short)(100 + frame % 1000); }

static void paint(void) {
    unsigned first = (unsigned)paintedtime;
    unsigned current = (unsigned)pax_sound_time();
    if (first < current) first = current;
    unsigned end = current + SOUND_RATE * 3 / 10;
    for (unsigned i = first; i < end; ++i) {
        unsigned at = (i & (RING_FRAMES - 1)) * 2;
        dma_buf[at] = sample_at(i);
        dma_buf[at + 1] = -sample_at(i);
    }
    paintedtime = (int)end;
    SNDDMA_Submit();
}

static void fake_delay(int ms) {
    if (__atomic_load_n(&task_done, __ATOMIC_ACQUIRE)) longjmp(finished, 1);
    clock_us += (int64_t)ms * 1000;
    if (clock_us >= next_mix) {
        /* Stop producing for 1.5 s, longer than a whole ring, then recover. */
        if (!stall || clock_us < 2000000 || clock_us >= 3500000) paint();
        next_mix = clock_us + 111000;
    }
}

static void fake_audio_init(int rate) { assert(rate == SOUND_RATE); }
static void fake_submit(short *pcm, int frames) {
    assert(frames == BLOCK_FRAMES);
    assert((unsigned)pax_sound_time() == submitted);
    assert(SNDDMA_GetDMAPos() == (int)((submitted & (RING_FRAMES - 1)) * 2));
    for (unsigned i = 0; i < (unsigned)frames; ++i) {
        if (pcm[i * 2] == 0) {
            assert(pcm[i * 2 + 1] == 0);
            assert(stall && clock_us >= 2000000 && clock_us < 3700000);
            ++silence;
        } else {
            assert(pcm[i * 2] == sample_at(submitted + i));
            assert(pcm[i * 2 + 1] == -sample_at(submitted + i));
        }
    }
    submitted += frames;
    if (submitted >= BLOCK_FRAMES * 400) __atomic_store_n(&sound_on, 0, __ATOMIC_RELEASE);
}

static int fake_create(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, int priority, void *out, int core) {
    (void)name; (void)stack; (void)arg;
    assert(priority == 6 && core == 1);
    if (fail_create) return -1;
    created_task = fn;
    *(void **)out = &created_task;
    return 0;
}
static void fake_delete(void *handle) { assert(handle == &created_task); ++deletes; }

int main(void) {
    static const app_services_t svc = {
        .audio_init = fake_audio_init, .audio_submit = fake_submit,
        .task_create = fake_create, .task_delete = fake_delete,
        .get_time_us = fake_time, .delay_ms = fake_delay
    };
    _pax_svc = &svc;
    for (stall = 0; stall <= 1; ++stall) {
        clock_us = next_mix = 0;
        submitted = silence = 0;
        paintedtime = 0;
        assert(SNDDMA_Init());
        paint();
        next_mix = 111000;
        if (setjmp(finished) == 0) created_task(NULL);
        assert(pax_sound_time() == BLOCK_FRAMES * 400);
        /* All but the final block are paced before the stop request. */
        unsigned rate = (unsigned)((int64_t)(submitted - BLOCK_FRAMES) * 1000000 / clock_us);
        assert(rate >= 22040 && rate <= 22060);
        assert(stall ? silence > 0 : silence == 0);
        SNDDMA_Shutdown();
        SNDDMA_Shutdown();
        assert(deletes == (unsigned)stall + 1);
    }
    fail_create = 1;
    assert(!SNDDMA_Init());
    assert(!sound_on && !dma.buffer && !sound_task_handle);
    puts("PASS: 9 FPS mixing, 22050 Hz playback, stereo/wrap/stall recovery and shutdown");
    return 0;
}
