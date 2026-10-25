#include "psram_app.h"
#include "qcommon/qcommon.h"
#include "pax_music.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const app_services_t *_pax_svc;
enum { MUSIC_RATE = 22050, MUSIC_RING = 16384, MUSIC_INPUT = 8192, MUSIC_PCM = 2304 };
static int16_t music_ring[MUSIC_RING * 2];
static uint8_t input[MUSIC_INPUT];
static int16_t pcm[MUSIC_PCM];
static uint32_t ring_read, ring_write, command;
static unsigned command_serial;
static int running, worker_done, ready, mixing, paused, gain_q8, finished;
static uint32_t mixed_total, underrun_total;
static void *task_handle;
static char music_dir[MAX_OSPATH];
static cvar_t *cd_nocd, *cd_volume;
static int requested_track, requested_loop;

/* Command publication and PCM ring publication are independent. Only the
 * music worker owns files/decoder state; the game never blocks on SD reads. */
static void request(int track, int loop) {
    requested_track = track;
    requested_loop = loop;
    __atomic_store_n(&command, (++command_serial << 16) | ((loop != 0) << 8) | (unsigned)track,
                     __ATOMIC_RELEASE);
}

void pax_music_mix(int16_t *stereo, unsigned frames) {
    __atomic_store_n(&mixing, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&ready, __ATOMIC_SEQ_CST) || __atomic_load_n(&paused, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&mixing, 0, __ATOMIC_SEQ_CST);
        return;
    }
    unsigned first = __atomic_load_n(&ring_read, __ATOMIC_RELAXED);
    unsigned end = __atomic_load_n(&ring_write, __ATOMIC_ACQUIRE);
    unsigned available = end - first;
    if (available > frames) available = frames;
    int gain = __atomic_load_n(&gain_q8, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < available; ++i) {
        unsigned at = ((first + i) & (MUSIC_RING - 1)) * 2;
        for (unsigned ch = 0; ch < 2; ++ch) {
            int value = stereo[i * 2 + ch] + music_ring[at + ch] * gain / 256;
            stereo[i * 2 + ch] = value > 32767 ? 32767 : value < -32768 ? -32768 : value;
        }
    }
    /* Only consume published samples; an underrun must never replay stale
     * ring contents or let the reader overtake the producer. */
    __atomic_store_n(&ring_read, first + available, __ATOMIC_RELEASE);
    __atomic_fetch_add(&mixed_total, available, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&finished, __ATOMIC_ACQUIRE))
        __atomic_fetch_add(&underrun_total, frames - available, __ATOMIC_RELAXED);
    else if (first + available == end)
        __atomic_store_n(&ready, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&mixing, 0, __ATOMIC_SEQ_CST);
}

static void reset_ring(void) {
    __atomic_store_n(&ready, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&mixing, __ATOMIC_SEQ_CST)) _pax_svc->delay_ms(1);
    __atomic_store_n(&ring_read, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ring_write, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&mixed_total, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&underrun_total, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&finished, 0, __ATOMIC_RELEASE);
}

/* Skip ID3v2 tags rather than feeding metadata to the frame synchronizer. */
static int rewind_mp3(void *file, void *decoder) {
    uint8_t h[10];
    if (_pax_svc->file_seek(file, 0, SEEK_SET) != 0) return 0;
    size_t got = _pax_svc->file_read(h, 1, sizeof h, file);
    long offset = 0;
    if (got == sizeof h && !memcmp(h, "ID3", 3) && !((h[6] | h[7] | h[8] | h[9]) & 0x80)) {
        offset = 10L + ((long)h[6] << 21) + ((long)h[7] << 14) + ((long)h[8] << 7) + h[9];
        if (h[5] & 0x10) offset += 10;
    }
    pax_mp3_reset(decoder);
    return _pax_svc->file_seek(file, offset, SEEK_SET) == 0;
}

static void *open_track(unsigned track) {
    char path[MAX_OSPATH + 40];
    snprintf(path, sizeof path, "%s/music/track%02u.mp3", music_dir, track);
    void *file = _pax_svc->file_open(path, "rb");
    if (!file) {
        snprintf(path, sizeof path, "%s/music/track%u.mp3", music_dir, track);
        file = _pax_svc->file_open(path, "rb");
    }
    if (!file) {
        snprintf(path, sizeof path, "/sd/roms/quake2/music/track%02u.mp3", track);
        file = _pax_svc->file_open(path, "rb");
    }
    if (_pax_svc->log_printf)
        _pax_svc->log_printf("QUAKE2 MUSIC %s track=%02u path=%s\n", file ? "opened" : "missing", track, path);
    return file;
}

static void music_task(void *arg) {
    (void)arg;
    void *file = NULL, *decoder = NULL;
    unsigned seen = 0, track = 0, loop = 0, pos = 0, len = 0;
    unsigned failures = 0, logged_format = 0, decoded = 0;
    int eof = 0;
    int64_t decode_us = 0;
    while (__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
        unsigned next = __atomic_load_n(&command, __ATOMIC_ACQUIRE);
        if (next != seen) {
            reset_ring();
            if (file) _pax_svc->file_close(file);
            file = NULL;
            seen = next;
            track = next & 255;
            loop = (next >> 8) & 1;
            pos = len = failures = logged_format = decoded = 0;
            eof = 0;
            decode_us = 0;
            if (track) file = open_track(track);
            if (file && !decoder) decoder = pax_mp3_create();
            if (file && (!decoder || !rewind_mp3(file, decoder))) {
                _pax_svc->log_printf("QUAKE2 MUSIC decoder allocation/seek failed track=%02u\n", track);
                _pax_svc->file_close(file);
                file = NULL;
            }
        }
        if (!file || __atomic_load_n(&paused, __ATOMIC_ACQUIRE)) {
            _pax_svc->delay_ms(10);
            continue;
        }
        unsigned written = __atomic_load_n(&ring_write, __ATOMIC_RELAXED);
        unsigned read = __atomic_load_n(&ring_read, __ATOMIC_ACQUIRE);
        if (written - read > MUSIC_RING - 1152) {
            _pax_svc->delay_ms(5);
            continue;
        }
        /* Large compressed reads keep SD transactions out of the 23 ms
         * playback deadline. Decode uses a bounded contiguous 8 KiB window. */
        if (len - pos < 2048 && !eof) {
            unsigned remaining = len - pos;
            memmove(input, input + pos, remaining);
            pos = 0;
            len = remaining;
            size_t got = _pax_svc->file_read(input + len, 1, MUSIC_INPUT - len, file);
            len += got;
            eof = got == 0;
        }
        if (len - pos < 4) {
            if (loop && rewind_mp3(file, decoder)) {
                pos = len = failures = 0;
                eof = 0;
                continue;
            }
            _pax_svc->file_close(file);
            file = NULL;
            /* Drain the already queued tail for non-looping tracks. */
            __atomic_store_n(&finished, 1, __ATOMIC_RELEASE);
            if (written != read) __atomic_store_n(&ready, 1, __ATOMIC_SEQ_CST);
            continue;
        }
        int consumed = 0, samples = 0, rate = 0, channels = 0;
        int64_t start = _pax_svc->get_time_us();
        int result = pax_mp3_decode(decoder, input + pos, len - pos, pcm, MUSIC_PCM,
                                    &consumed, &samples, &rate, &channels);
        decode_us += _pax_svc->get_time_us() - start;
        if (consumed < 0 || (unsigned)consumed > len - pos) consumed = 0;
        pos += consumed ? (unsigned)consumed : 1;
        if (result != 0 || samples <= 0 || samples > MUSIC_PCM || (channels != 1 && channels != 2)) {
            if (++failures >= 4096) {
                _pax_svc->log_printf("QUAKE2 MUSIC decode failed track=%02u\n", track);
                _pax_svc->file_close(file);
                file = NULL;
            }
            continue;
        }
        failures = 0;
        if (rate != MUSIC_RATE && rate != MUSIC_RATE * 2) {
            _pax_svc->log_printf("QUAKE2 MUSIC unsupported rate=%d track=%02u\n", rate, track);
            _pax_svc->file_close(file);
            file = NULL;
            continue;
        }
        unsigned step = rate / MUSIC_RATE;
        unsigned source_frames = samples / channels;
        unsigned frames = source_frames / step;
        for (unsigned i = 0; i < frames; ++i) {
            unsigned at = ((written + i) & (MUSIC_RING - 1)) * 2;
            for (unsigned ch = 0; ch < 2; ++ch) {
                unsigned source_ch = channels == 1 ? 0 : ch;
                int value = pcm[(i * step) * channels + source_ch];
                /* Cheap 2:1 anti-alias averaging for the supplied 44.1 kHz
                 * tracks, instead of throwing away alternate samples. */
                if (step == 2) value = (value + pcm[(i * step + 1) * channels + source_ch]) / 2;
                music_ring[at + ch] = value;
            }
        }
        __atomic_store_n(&ring_write, written + frames, __ATOMIC_RELEASE);
        if (written + frames - read >= 2048) __atomic_store_n(&ready, 1, __ATOMIC_SEQ_CST);
        decoded += frames;
        if (!logged_format++)
            _pax_svc->log_printf("QUAKE2 MUSIC decoded track=%02u %d Hz %d channels -> 22050 Hz stereo\n", track, rate, channels);
        if (decoded >= MUSIC_RATE * 5) {
            unsigned cpu = (unsigned)(decode_us * MUSIC_RATE * 10000 / ((int64_t)decoded * 1000000));
            _pax_svc->log_printf("QUAKE2 MUSIC STREAM track=%02u decode_cpu=%u.%02u%% mixed=%u underrun=%u lead=%u\n",
                                  track, cpu / 100, cpu % 100,
                                  __atomic_load_n(&mixed_total, __ATOMIC_RELAXED),
                                  __atomic_load_n(&underrun_total, __ATOMIC_RELAXED),
                                  written + frames - read);
            decoded = 0;
            decode_us = 0;
        }
    }
    reset_ring();
    if (file) _pax_svc->file_close(file);
    pax_mp3_destroy(decoder);
    __atomic_store_n(&worker_done, 1, __ATOMIC_RELEASE);
    for (;;) _pax_svc->delay_ms(1000);
}

void CDAudio_Play(int track, qboolean looping) {
    if (track < 2 || track > 99) track = 0; /* track 1 was CD data */
    if (track == requested_track && (looping != 0) == requested_loop) return;
    request(track, looping != 0);
}
void CDAudio_Stop(void) { request(0, 0); }
void CDAudio_Pause(void) { __atomic_store_n(&paused, 1, __ATOMIC_RELEASE); }
void CDAudio_Resume(void) { __atomic_store_n(&paused, cd_nocd && cd_nocd->value != 0, __ATOMIC_RELEASE); }
void CDAudio_Update(void) {
    if (!cd_volume) return;
    float value = cd_volume->value;
    int gain = value < 0 ? 0 : value > 1 ? 256 : (int)(value * 256);
    __atomic_store_n(&gain_q8, gain, __ATOMIC_RELAXED);
    if (cd_nocd->modified) {
        cd_nocd->modified = false;
        __atomic_store_n(&paused, cd_nocd->value != 0, __ATOMIC_RELEASE);
    }
}
static void CD_f(void) {
    const char *cmd = Cmd_Argv(1);
    if (!strcmp(cmd, "play") || !strcmp(cmd, "loop")) CDAudio_Play(atoi(Cmd_Argv(2)), !strcmp(cmd, "loop"));
    else if (!strcmp(cmd, "stop")) CDAudio_Stop();
    else if (!strcmp(cmd, "pause") || !strcmp(cmd, "off")) CDAudio_Pause();
    else if (!strcmp(cmd, "resume") || !strcmp(cmd, "on")) CDAudio_Resume();
    else Com_Printf("MP3 music: cd play/loop <track>, cd stop/pause/resume; cd_volume 0..1\n");
}
int CDAudio_Init(void) {
    cd_nocd = Cvar_Get("cd_nocd", "0", CVAR_ARCHIVE);
    cd_volume = Cvar_Get("cd_volume", "0.25", CVAR_ARCHIVE);
#ifdef QUAKE2_TEST_NEWGAME
    /* FS_Gamedir redirects save/config writes during the isolated regression;
     * the read-only music assets still live beside the real baseq2 PAK. */
    snprintf(music_dir, sizeof music_dir, "/sd/roms/quake2/baseq2");
#else
    snprintf(music_dir, sizeof music_dir, "%s", FS_Gamedir());
#endif
    command = command_serial = 0;
    requested_track = requested_loop = 0;
    worker_done = ready = mixing = 0;
    task_handle = NULL;
    CDAudio_Update();
    paused = cd_nocd->value != 0;
    __atomic_store_n(&running, 1, __ATOMIC_RELEASE);
    if (_pax_svc->task_create(music_task, "q2_music", 40960, NULL, 4, &task_handle, 1) != 0) {
        running = 0;
        _pax_svc->log_printf("QUAKE2 MUSIC task creation failed\n");
        return -1;
    }
    Cmd_AddCommand("cd", CD_f);
    return 0;
}
void pax_music_shutdown(void) {
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    if (task_handle) {
        for (int ms = 0; ms < 1500 && !__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE); ms += 10)
            _pax_svc->delay_ms(10);
        _pax_svc->task_delete(task_handle);
        task_handle = NULL;
    }
}
void CDAudio_Shutdown(void) {
    pax_music_shutdown();
    Cmd_RemoveCommand("cd");
}
