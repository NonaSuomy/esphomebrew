/*
 * SDL Audio shim for Duke3D PSRAM App.
 *
 * Duke3D's audiolib (dsl.c) calls:
 *   SDL_OpenAudio(&desired, &obtained) — with audio_cb callback
 *   SDL_PauseAudio(0) — to start playback
 *   SDL_BuildAudioCVT / SDL_ConvertAudio — for format conversion
 *
 * We route audio through the pax service table:
 *   audio_init(sample_rate) → audio_submit(stereo_buf, frame_count)
 *
 * The audio callback fills a mono S16 buffer, we convert to stereo
 * and submit via the service table on a dedicated task (core 1).
 */
#include "psram_app.h"
#include "SDL_audio.h"
#include <string.h>

extern const app_services_t *_pax_svc;
extern volatile int pax_exit_requested;
extern void pax_duke_music_mix(int16_t *mono, int frames);
extern void pax_duke_music_shutdown(void);
extern void duke_resample_reset(void);
extern size_t duke_resample48(const int16_t *,size_t,int16_t *,size_t);

static SDL_AudioSpec s_audio_spec;
static uint8_t *s_sdl_buffer = NULL;     /* mono mix buffer for the callback */
static volatile int s_audio_paused = 1;
static volatile int s_audio_locked = 0;
static volatile int s_audio_task_done = 0;
static void *s_audio_task_handle = NULL;
static int16_t *s_stereo_buf = NULL;
static int16_t *s_device_buf = NULL;

/* Audio task: calls the game's audio callback repeatedly,
 * converts mono→stereo, submits to hardware */
static void audio_feed_task(void *arg)
{
    (void)arg;
    const int mono_bytes = SAMPLECOUNT * SAMPLESIZE;

    /* Allocate stereo output buffer in DMA-capable internal RAM */
    s_stereo_buf = _pax_svc->mem_caps_alloc(
        SAMPLECOUNT * 2 * sizeof(int16_t),
        PAX_MEM_CAP_INTERNAL | PAX_MEM_CAP_DMA);
    if (!s_stereo_buf) {
        _pax_svc->log_printf("Duke3D audio: stereo buf alloc failed\n");
        s_audio_task_done = 1;
        while (1) _pax_svc->delay_ms(100);
    }

    s_device_buf = _pax_svc->mem_alloc(1120*2*sizeof(int16_t));
    if (!s_device_buf) {
        _pax_svc->log_printf("Duke3D audio: 48 kHz buf alloc failed\n");
        s_audio_task_done = 1;
        while (1) _pax_svc->delay_ms(100);
    }
    duke_resample_reset();
    int64_t stats_start=_pax_svc->get_time_us();
    int64_t mix_us=0, max_mix_us=0, frames_sent=0;
    unsigned late=0;
    while (!pax_exit_requested) {
        if (!s_audio_paused && !s_audio_locked && s_sdl_buffer) {
            int64_t begin=_pax_svc->get_time_us();
            memset(s_sdl_buffer, 0, mono_bytes);
            if (s_audio_spec.callback)
                s_audio_spec.callback(s_audio_spec.userdata, s_sdl_buffer, mono_bytes);

            /* Mix SD-card MP3 music into the same stream as Duke's sound
             * effects. The launcher then applies the shared blocking audio
             * submit path to the combined buffer. */
            pax_duke_music_mix((int16_t *)s_sdl_buffer, SAMPLECOUNT);

            /* Mono S16 → stereo interleaved */
            const int16_t *mono = (const int16_t *)s_sdl_buffer;
            for (int i = 0; i < SAMPLECOUNT; i++) {
                s_stereo_buf[i * 2]     = mono[i];
                s_stereo_buf[i * 2 + 1] = mono[i];
            }

            size_t count=duke_resample48(s_stereo_buf,SAMPLECOUNT,s_device_buf,1120);
            int64_t elapsed=_pax_svc->get_time_us()-begin;
            mix_us+=elapsed;if(elapsed>max_mix_us)max_mix_us=elapsed;
            if(elapsed>1000000LL*SAMPLECOUNT/SAMPLERATE)++late;
            if(count)_pax_svc->audio_submit(s_device_buf,count);
            frames_sent+=SAMPLECOUNT;
            int64_t now=_pax_svc->get_time_us();
            if(now-stats_start>=3000000) {
                _pax_svc->log_printf("DUKE AUDIO: rate=%d target=%d mix=%d%% max=%d ms late=%u\n",(int)(frames_sent*1000000/(now-stats_start)),SAMPLERATE,(int)(mix_us*100/(now-stats_start)),(int)(max_mix_us/1000),late);
                stats_start=now;frames_sent=mix_us=max_mix_us=0;late=0;
            }
        } else {
            _pax_svc->delay_ms(5);
        }
    }

    s_audio_task_done = 1;
    while (1) _pax_svc->delay_ms(100);
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    /* Allocate the mix buffer */
    if (!s_sdl_buffer) {
        s_sdl_buffer = _pax_svc->mem_caps_alloc(
            SAMPLECOUNT * SAMPLESIZE * 2, PAX_MEM_CAP_INTERNAL);
        if (!s_sdl_buffer)
            s_sdl_buffer = _pax_svc->mem_alloc(SAMPLECOUNT * SAMPLESIZE * 2);
    }

    /* Fill obtained spec */
    if (obtained) {
        memset(obtained, 0, sizeof(SDL_AudioSpec));
        obtained->freq     = SAMPLERATE;
        obtained->format   = 16; /* AUDIO_S16SYS */
        obtained->channels = 1;
        obtained->samples  = SAMPLECOUNT;
        obtained->callback = desired->callback;
        obtained->userdata = desired->userdata;
    }

    memset(&s_audio_spec, 0, sizeof(s_audio_spec));
    s_audio_spec.freq     = SAMPLERATE;
    s_audio_spec.format   = 16;
    s_audio_spec.channels = 1;
    s_audio_spec.samples  = SAMPLECOUNT;
    s_audio_spec.callback = desired->callback;
    s_audio_spec.userdata = desired->userdata;

    /* Initialize hardware audio at our sample rate */
    _pax_svc->audio_init(48000);

    /* Start audio feed task on core 1 */
    _pax_svc->task_create(audio_feed_task, "duke_aud", 8192, NULL, 8,
                           &s_audio_task_handle, 1);

    _pax_svc->log_printf("Duke3D SDL_OpenAudio: %d Hz, %d samples\n",
                          SAMPLERATE, SAMPLECOUNT);
    return 0;
}

void SDL_PauseAudio(int pause_on)
{
    s_audio_paused = pause_on ? 1 : 0;
}

void SDL_CloseAudio(void)
{
    s_audio_paused = 1;
    if (s_audio_task_handle) {
        /* Wait for audio task to notice exit */
        for (int i = 0; i < 100 && !s_audio_task_done; i++)
            _pax_svc->delay_ms(10);
        _pax_svc->task_delete(s_audio_task_handle);
        s_audio_task_handle = NULL;
    }
    if (s_stereo_buf) {
        _pax_svc->mem_free(s_stereo_buf);
        s_stereo_buf = NULL;
    }
    if (s_sdl_buffer) {
        _pax_svc->mem_free(s_sdl_buffer);
        s_sdl_buffer = NULL;
    }
    if (s_device_buf) { _pax_svc->mem_free(s_device_buf);s_device_buf=NULL; }
}

/* Called from pax_main.c during exit cleanup */
void pax_sound_shutdown(void)
{
    SDL_CloseAudio();
    pax_duke_music_shutdown();
}

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, Uint16 src_format, Uint8 src_channels,
                       int src_rate, Uint16 dst_format, Uint8 dst_channels, int dst_rate)
{
    if (cvt) cvt->len_mult = 1;
    return 0;
}

int SDL_ConvertAudio(SDL_AudioCVT *cvt)
{
    /* Duke3D's dsl.c calls this after audio_cb. In the original SDL port,
     * it converts mono→stereo. We handle that in the audio task, so this
     * is mostly a no-op. But dsl.c expects it to work on cvt->buf. */
    if (!cvt || !cvt->buf) return 0;
    /* Just update len_cvt — the data is already in the buffer */
    cvt->len_cvt = cvt->len;
    return 0;
}

void SDL_LockAudio(void)   { s_audio_locked = 1; }
void SDL_UnlockAudio(void) { s_audio_locked = 0; }
