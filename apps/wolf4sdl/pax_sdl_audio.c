/* SDL_mixer audio backend for the PAX service ABI. Wolf4SDL's original SDL
 * backend owns an audio callback task. Keep that same cadence here: rendering
 * is variable-cost, so generating one block from SDL_Flip makes sound stutter
 * or change pitch whenever a frame takes longer than usual. */
#include "psram_app.h"
#include "SDL_audio.h"
#include <string.h>

extern const app_services_t *_pax_svc;

static SDL_AudioSpec s_spec;
static uint8_t *s_mix;
static int16_t *s_stereo;
static volatile int s_paused = 1;
static volatile int s_locked;
static volatile int s_audio_mixing;
static volatile int s_audio_running;
static volatile int s_audio_task_done;
static void *s_audio_task_handle;
static int s_audio_task_failed;

static void mix_and_submit(void)
{
    if (!s_mix || !s_stereo || !s_spec.callback) return;

    s_audio_mixing = 1;
    memset(s_mix, 0, SAMPLECOUNT * 2u);
    s_spec.callback(s_spec.userdata, s_mix, SAMPLECOUNT * 2);
    s_audio_mixing = 0;
    const int16_t *mono = (const int16_t *)s_mix;
    for (int i = 0; i < SAMPLECOUNT; ++i) {
        s_stereo[i * 2] = mono[i];
        s_stereo[i * 2 + 1] = mono[i];
    }
    _pax_svc->audio_submit((short *)s_stereo, SAMPLECOUNT);
}

static void audio_feed_task(void *arg)
{
    (void)arg;
    while (s_audio_running) {
        if (s_paused || s_locked) {
            _pax_svc->delay_ms(1);
            continue;
        }
        mix_and_submit();
    }
    s_audio_task_done = 1;
    /* The owning app thread deletes this task after it has stopped. */
    while (1) _pax_svc->delay_ms(100);
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    if (!desired || !_pax_svc) return -1;
    if (!s_mix) s_mix = (uint8_t *)_pax_svc->mem_caps_alloc(
        SAMPLECOUNT * 2u, PAX_MEM_CAP_INTERNAL | PAX_MEM_CAP_DMA);
    if (!s_stereo) s_stereo = (int16_t *)_pax_svc->mem_caps_alloc(
        SAMPLECOUNT * 2u * sizeof(int16_t), PAX_MEM_CAP_INTERNAL | PAX_MEM_CAP_DMA);
    if (!s_mix || !s_stereo) return -1;

    memset(&s_spec, 0, sizeof(s_spec));
    s_spec.freq = desired->freq > 0 ? desired->freq : SAMPLERATE;
    s_spec.format = AUDIO_S16SYS;
    s_spec.channels = 1;
    s_spec.samples = SAMPLECOUNT;
    s_spec.size = SAMPLECOUNT * 2u;
    s_spec.callback = desired->callback;
    s_spec.userdata = desired->userdata;
    if (obtained) *obtained = s_spec;

    _pax_svc->audio_init(s_spec.freq);
    s_paused = 1;
    s_locked = 0;
    s_audio_mixing = 0;
    s_audio_running = 0;
    s_audio_task_done = 0;
    s_audio_task_failed = 0;
    s_audio_task_handle = NULL;
    _pax_svc->log_printf("Wolf4SDL PAX: audio %d Hz / %d frames\n",
                          s_spec.freq, SAMPLECOUNT);
    return 0;
}

void SDL_PauseAudio(int pause_on)
{
    s_paused = pause_on ? 1 : 0;
    if (pause_on || s_audio_task_handle || s_audio_task_failed ||
        !_pax_svc || !_pax_svc->task_create)
        return;

    s_audio_running = 1;
    s_audio_task_done = 0;
    if (_pax_svc->task_create(audio_feed_task, "wolf_aud", 8192, NULL, 8,
                               &s_audio_task_handle, 1) != 0) {
        s_audio_running = 0;
        s_audio_task_failed = 1;
        _pax_svc->log_printf(
            "Wolf4SDL PAX: audio task unavailable; using render-thread fallback\n");
    } else {
        _pax_svc->log_printf("Wolf4SDL PAX: audio task started\n");
    }
}

void SDL_CloseAudio(void)
{
    s_paused = 1;
    s_audio_running = 0;
    if (s_audio_task_handle) {
        for (int i = 0; i < 100 && !s_audio_task_done; ++i)
            _pax_svc->delay_ms(10);
        _pax_svc->task_delete(s_audio_task_handle);
        s_audio_task_handle = NULL;
    }
    if (s_mix) { _pax_svc->mem_free(s_mix); s_mix = NULL; }
    if (s_stereo) { _pax_svc->mem_free(s_stereo); s_stereo = NULL; }
    s_audio_task_failed = 0;
}

void SDL_LockAudio(void)
{
    s_locked = 1;
    /* SDL_mixer mutates its channel/music lists under this lock. Wait for
     * an already-running callback to finish before touching those lists. */
    while (s_audio_mixing && _pax_svc && _pax_svc->delay_ms)
        _pax_svc->delay_ms(1);
}
void SDL_UnlockAudio(void) { s_locked = 0; }

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, Uint16 src_format,
                      Uint8 src_channels, int src_rate,
                      Uint16 dst_format, Uint8 dst_channels, int dst_rate)
{
    (void)src_format; (void)src_channels; (void)src_rate;
    (void)dst_format; (void)dst_channels; (void)dst_rate;
    if (!cvt) return -1;
    cvt->needed = 0;
    cvt->len_mult = 1;
    cvt->len_ratio = 1.0;
    return 0;
}

int SDL_ConvertAudio(SDL_AudioCVT *cvt)
{
    if (cvt) cvt->len_cvt = cvt->len;
    return 0;
}

void pax_sdl_pump_audio(void)
{
    /* Only used if the launcher cannot create the normal audio task. */
    if (s_audio_task_handle || !s_audio_task_failed || s_paused || s_locked)
        return;
    mix_and_submit();
}

void pax_sdl_audio_shutdown(void) { SDL_CloseAudio(); }
