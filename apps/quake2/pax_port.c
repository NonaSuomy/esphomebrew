#include "psram_app.h"
#include "quake2.h"
#include "client/keys.h"
#include "ref_soft/r_local.h"
#include "pax_video_memory.h"
#include <string.h>

extern const app_services_t *_pax_svc;
extern volatile int pax_exit_requested;

static pixel_t *screen;
static int width, height;
static uint16_t *rgb;
static uint16_t palette[256];
static unsigned long frame_count;
static int initial_frame_sent;
#ifdef QUAKE2_TEST_NEWGAME
static int perf_servercount;
static unsigned long perf_start_frames;
static int64_t perf_start, perf_present_us, perf_start_present_us;
#endif

void QG_SetPalette(unsigned char p[768]) {
    for (int i = 0; i < 256; ++i)
        palette[i] = ((p[i*3] & 0xf8) << 8) | ((p[i*3+1] & 0xfc) << 3) | (p[i*3+2] >> 3);
}
void QG_DrawFrame(void *pixels) {
    if (!rgb || !pixels) return;
    ++frame_count;
    if ((frame_count == 1 || (frame_count % 30) == 0) && _pax_svc->log_printf)
        _pax_svc->log_printf("QUAKE2 FRAME %lu\n", frame_count);
#ifdef QUAKE2_TEST_NEWGAME
    int64_t present_start = _pax_svc->get_time_us();
#endif
    const unsigned char *src = pixels;
    for (int i = 0; i < width * height; ++i) rgb[i] = palette[src[i]];
    if (_pax_svc->display_write_frame_custom)
        _pax_svc->display_write_frame_custom(rgb, (uint16_t)width, (uint16_t)height, 2.0f, false);
#ifdef QUAKE2_TEST_NEWGAME
    perf_present_us += _pax_svc->get_time_us() - present_start;
#endif
}
#ifdef QUAKE2_TEST_NEWGAME
/* Called from the client-facing main loop, since these old Quake headers
 * have no include guards and cannot coexist with the renderer headers. */
void pax_video_perf_sample(int playable, int servercount) {
    int64_t now = _pax_svc->get_time_us();
    if (playable) {
        if (!perf_start || perf_servercount != servercount) {
            perf_start = now;
            perf_servercount = servercount;
            perf_start_frames = frame_count;
            perf_start_present_us = perf_present_us;
        } else {
            unsigned long frames = frame_count - perf_start_frames;
            if (frames && now - perf_start >= 5000000) {
                _pax_svc->log_printf("QUAKE2 PERF frames=%lu elapsed_us=%lu fps=%.2f present_ms=%.2f\n",
                                     frames, (unsigned long)(now - perf_start),
                                     frames * 1000000.0 / (now - perf_start),
                                     (perf_present_us - perf_start_present_us) / (frames * 1000.0));
                perf_start = now;
                perf_start_frames = frame_count;
                perf_start_present_us = perf_present_us;
            }
        }
    } else {
        perf_start = 0;
    }
}
#endif
int QG_GetKey(int *down, int *key);
void QG_GetMouseMove(int *x, int *y);
int QG_Milliseconds(void) { return (int)(_pax_svc->get_time_us() / 1000); }
void QG_GetMouseDiff(int *x, int *y) { QG_GetMouseMove(x, y); }
void QG_CaptureMouse(void) {}
void QG_ReleaseMouse(void) {}
void QG_Mkdir(const char *path) { Sys_Mkdir((char *)path); }

rserr_t SWimp_SetMode(int *w, int *h, int mode, qboolean fullscreen) {
    (void)mode; (void)fullscreen;
    width = 320; height = 240;
    if (!pax_video_memory_init(_pax_svc, QUAKE2_SRAM_ZBUFFER))
        return rserr_invalid_mode;
    screen = (pixel_t *)pax_video_color_buffer();
    rgb = _pax_svc->display_get_framebuffer
              ? _pax_svc->display_get_framebuffer() : NULL;
    if (!screen || !rgb) return rserr_invalid_mode;
    memset(screen, 0, (size_t)width * height);
    vid.width = width; vid.height = height; vid.rowbytes = width; vid.buffer = screen;
    *w = width; *h = height;
    ri.Vid_NewWindow(width, height);
    return rserr_ok;
}
void SWimp_Shutdown(void) {
    pax_video_memory_shutdown();
    screen = NULL;
    rgb = NULL;
}
int SWimp_Init(void *a, void *b) { (void)a; (void)b; return 1; }
qboolean SWimp_InitGraphics(qboolean fullscreen) { (void)fullscreen; return rserr_ok; }
void SWimp_SetPalette(const unsigned char *p) {
    unsigned char rgbpal[768];
    for (int i=0;i<256;i++) { rgbpal[i*3]=p[i*4]; rgbpal[i*3+1]=p[i*4+1]; rgbpal[i*3+2]=p[i*4+2]; }
    QG_SetPalette(rgbpal);
    /* The Quake II precache path can take longer than the launcher watchdog.
     * Present the initialized game surface as soon as the video palette is
     * available so the PAX remains active while the first map is prepared. */
    if (!initial_frame_sent && vid.buffer) {
        initial_frame_sent = 1;
        QG_DrawFrame(vid.buffer);
    }
}
void SWimp_BeginFrame(float sep) { (void)sep; }
void SWimp_EndFrame(void) { QG_DrawFrame(vid.buffer); }
void SWimp_AppActivate(qboolean active) { (void)active; }
