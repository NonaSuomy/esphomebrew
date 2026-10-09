/* PAX OSD bridge for nofrendo. */
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "nofrendo.h"
#include "osd.h"
#include "bitmap.h"
#include "vid_drv.h"
#include "event.h"
#include "nesinput.h"
#include "pax_nes_options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const app_services_t *_pax_svc;
extern volatile int pax_nes_exit;
extern volatile int pax_exit_requested;
extern volatile int nofrendo_quit_flag;
extern void nes_poweroff(void);

#define W 256
#define H 224
static bitmap_t *screen;
static uint8_t *indexed;
static uint16_t *rgb565;
static uint16_t *cropped_rgb565;
static uint16_t palette[256];
static void (*sound_cb)(void *, int);
static unsigned pax_video_frames;
static unsigned pax_audio_frames;
static unsigned pax_audio_submit_calls;

void pax_nes_frame_pace(int64_t frame_start)
{
    const int64_t frame_us = 16667; /* 60.00 Hz */
    const int64_t elapsed = _pax_svc->get_time_us() - frame_start;

    int64_t remaining = frame_us - elapsed;
    if (remaining >= 1000)
        /* Floor the millisecond sleep. Rounding up every frame makes the
         * emulator run at roughly 58.8 Hz instead of the NES refresh rate. */
        _pax_svc->delay_ms((int)(remaining / 1000));

    /* Do not log from the frame pacer: the standalone launcher's log service
     * can block for 100 ms when USB JTAG has no reader (UART is separate). */
}

static int vinit(int w, int h) { (void)w; (void)h; return 0; }
static void vshutdown(void) {}
static int vmode(int w, int h) { (void)w; (void)h; return 0; }
static void vpalette(rgb_t *p) {
    for (int i = 0; i < 256; ++i)
        palette[i] = (uint16_t)(((p[i].r >> 3) << 11) | ((p[i].g >> 2) << 5) | (p[i].b >> 3));
}
static void vclear(uint8_t c) { if (indexed) memset(indexed, c, W * H); }
static bitmap_t *vlock(void) { if (!screen) screen = bmp_createhw(indexed, W, H, W); return screen; }
static void vfree(int n, rect_t *r) { (void)n; (void)r; }
static void pax_nes_display_frame(void)
{
    pax_nes_options_apply_palette();
    const uint16_t *output = rgb565;
    uint16_t output_width = W;
    if (pax_nes_options_crop_sides()) {
        if (!cropped_rgb565)
            cropped_rgb565 = _pax_svc->mem_caps_alloc(
                (size_t)(W - 16) * H * sizeof(uint16_t), PAX_MEM_CAP_SPIRAM | PAX_MEM_CAP_DMA);
        if (cropped_rgb565) {
            for (int y = 0; y < H; ++y)
                memcpy(cropped_rgb565 + y * (W - 16), rgb565 + y * W + 8,
                       (W - 16) * sizeof(uint16_t));
            output = cropped_rgb565;
            output_width = W - 16;
        }
    }
    _pax_svc->display_write_frame_custom(output, output_width, H,
                                           pax_nes_options_scale(), false);
}
static void vblit(bitmap_t *b, int n, rect_t *r) {
    (void)n; (void)r;
    if (!b || !b->line[0] || !_pax_svc) return;
    unsigned nonzero_pixels = 0;
    for (int i = 0; i < W * H; ++i) {
        rgb565[i] = palette[b->line[0][i]];
        nonzero_pixels += rgb565[i] != 0;
    }
    if (pax_video_frames < 4)
        _pax_svc->log_printf("NES PAX: submitting video frame=%u\n",
                              pax_video_frames + 1u);
    pax_nes_display_frame();
    ++pax_video_frames;
    if ((pax_video_frames % 60u) == 0u)
        _pax_svc->log_printf("NES PAX: video frames=%u nonzero_pixels=%u\n",
                              pax_video_frames, nonzero_pixels);
}
static viddriver_t driver = { "PAX", vinit, vshutdown, vmode, vpalette, vclear, vlock, vfree, vblit, false };

int osd_main(int argc, char **argv)
{
    (void)argc;
    return main_loop(argv[0], system_nes);
}

void osd_getvideoinfo(vidinfo_t *i) { i->default_width = W; i->default_height = H; i->driver = &driver; }
void osd_getsoundinfo(sndinfo_t *i) { i->sample_rate = 32000; i->bps = 16; }
int osd_installtimer(int f, void *fn, int fs, void *c, int cs) { (void)f;(void)fn;(void)fs;(void)c;(void)cs; return 0; }
void osd_setsound(void (*fn)(void *, int)) { sound_cb = fn; }
void osd_getmouse(int *x, int *y, int *b) { if(x)*x=0;if(y)*y=0;if(b)*b=0; }
void osd_togglefullscreen(int c) { (void)c; }
void osd_fullname(char *d, const char *s) { strcpy(d, s); }
char *osd_newextension(char *s, char *e) { (void)e; return s; }
int osd_makesnapname(char *s, int n) { (void)s;(void)n; return -1; }
void osd_freeinput(void) {}

void osd_getinput(void)
{
    static int old;
    static int menu_was_down;
    pax_gamepad_state_t p;
    int now = 0;
    const int shared_menu = pax_nes_shared_menu_attach(_pax_svc, rgb565, W, H);
    _pax_svc->input_gamepad_read(&p);
    if (p.values[PAX_INPUT_SELECT]) now |= 1 << 0;
    if (p.values[PAX_INPUT_START])  now |= 1 << 3;
    if (p.values[PAX_INPUT_UP])     now |= 1 << 4;
    if (p.values[PAX_INPUT_RIGHT])  now |= 1 << 5;
    if (p.values[PAX_INPUT_DOWN])   now |= 1 << 6;
    if (p.values[PAX_INPUT_LEFT])   now |= 1 << 7;
    if (p.values[PAX_INPUT_A])      now |= 1 << 13;
    if (p.values[PAX_INPUT_B])      now |= 1 << 14;
    const int menu_down = p.values[PAX_INPUT_MENU];
    const int menu_pressed = menu_down && !menu_was_down;
    menu_was_down = menu_down;
    if ((p.values[PAX_INPUT_L] && p.values[PAX_INPUT_R]) || pax_exit_requested ||
        (_pax_svc->input_l3_read && _pax_svc->input_l3_read())) {
        /* osd_getinput() runs inside the emulation call stack.  Do not call
         * main_quit() here: it destroys the NES machine while the current
         * frame is still unwinding and corrupts the heap.  Request a clean
         * return, then let main_loop() perform teardown exactly once. */
        pax_exit_requested = 1;
        pax_nes_exit = 1;
        nofrendo_quit_flag = 1;
        nes_poweroff();
        /* Stop processing input events in the same frame as shutdown. */
        old = now;
        return;
    }
    if (!shared_menu && (menu_pressed || pax_nes_menu_swipe_down_pressed(_pax_svc))) {
        if (_pax_svc->log_printf)
            _pax_svc->log_printf("NES PAX: in-game menu trigger (menu=%d)\n", menu_down);
        if (pax_nes_menu_run(_pax_svc, rgb565, W, H)) {
            pax_exit_requested = 1;
            pax_nes_exit = 1;
            nofrendo_quit_flag = 1;
            nes_poweroff();
        }
        /* Discard buttons used by the modal menu rather than leaking them
         * into the next game's controller event. */
        old = now;
        return;
    }
    int changed = now ^ old;
    const int events[16] = { event_joypad1_select,0,0,event_joypad1_start,event_joypad1_up,event_joypad1_right,event_joypad1_down,event_joypad1_left,0,0,0,0,0,event_joypad1_a,event_joypad1_b,0 };
    for (int i = 0; i < 16; ++i) if ((changed & (1 << i)) && events[i]) {
        event_t fn = event_get(events[i]);
        if (fn) fn((now & (1 << i)) ? INP_STATE_MAKE : INP_STATE_BREAK);
    }
    old = now;
}

int osd_init(void) { indexed = _pax_svc->mem_alloc(W * H); rgb565 = _pax_svc->mem_caps_alloc(W * H * 2, PAX_MEM_CAP_SPIRAM | PAX_MEM_CAP_DMA); return indexed && rgb565 ? 0 : -1; }
void osd_shutdown(void) { pax_nes_shared_menu_detach(); bmp_destroy(&screen); if (indexed) _pax_svc->mem_free(indexed); if (rgb565) _pax_svc->mem_free(rgb565); if (cropped_rgb565) _pax_svc->mem_free(cropped_rgb565); indexed = NULL; rgb565 = NULL; cropped_rgb565 = NULL; sound_cb = NULL; }
void osd_main_dummy(void) {}

/* Called by the nofrendo APU bridge in pax_audio.c. */
void do_audio_frame(void) {
    static unsigned remainder;
    if (!sound_cb) return;
    /* 32000 / 60 = 533, 533, 534 samples; 512 underfeeds the speaker. */
    remainder += 32000;
    int samples = remainder / 60;
    remainder %= 60;
    short stereo[534 * 2];
    sound_cb(stereo, samples);
    int peak = 0;
    for (int i = 0; i < samples; ++i) {
        int value = stereo[i];
        if (value < 0) value = -value;
        if (value > peak) peak = value;
    }
    for (int i = samples - 1; i >= 0; --i) {
        stereo[i * 2] = stereo[i];
        stereo[i * 2 + 1] = stereo[i];
    }
    if (pax_audio_submit_calls < 8u)
        _pax_svc->log_printf("NES PAX: audio submit begin=%u peak=%d\n",
                              pax_audio_submit_calls + 1u, peak);
    _pax_svc->audio_submit(stereo, samples);
    if (pax_audio_submit_calls < 8u)
        _pax_svc->log_printf("NES PAX: audio submit end=%u\n",
                              pax_audio_submit_calls + 1u);
    ++pax_audio_submit_calls;
    ++pax_audio_frames;
    if ((pax_audio_frames % 60u) == 0u)
        _pax_svc->log_printf("NES PAX: audio frames=%u peak=%d\n",
                              pax_audio_frames, peak);
}
