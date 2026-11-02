/* HuExpress PC Engine / TurboGrafx-16 PAX entry point. */
#define PAX_APP_SIDE 1
#include "runtime.h"
#define EMU_AUDIO_SLOTS 6
#define EMU_AUDIO_PREFILL 3
#include "audio_queue.h"
#include "pce.h"
#include "mix.h"
#include "zipmgr.h"
#include <setjmp.h>
#include <strings.h>

const app_services_t *_pax_svc;

extern char *rom_file_name;
extern char *syscard_filename;
extern uchar *SPM_raw;

static volatile int pce_exit_requested;
bool skipNextFrame = false;
void app_return_to_launcher(void) { pce_exit_requested = 1; }
void HCD_shutdown(void) {}
int rmdir(const char *path) { (void)path; return 0; }

#define PCE_OUTPUT_W       256
#define PCE_OUTPUT_H       240
#define PCE_AUDIO_RATE     22050
#define PCE_AUDIO_CAP      768
#define PCE_AUDIO_CHANNELS 6
#define PCE_INDEX_STRIDE   XBUF_WIDTH

/* HuExpress expects these buffers to be present while the CPU is running.
 * Keep the indexed render buffer in PSRAM and convert only the visible area
 * when a frame is complete. */
static unsigned char *pce_xbuf;
static unsigned char *pce_spm_raw;
static uint32 *pce_sprite_positions;
static uint16_t *pce_palette;
static uint16_t *pce_pixels;
static char *pce_audio_channels[PCE_AUDIO_CHANNELS];
static int16_t *pce_audio;
static unsigned pce_frame_count;
static unsigned pce_changes;
static unsigned pce_last_hash;
static int pce_audio_peak;
static unsigned pce_audio_phase;
static int64_t pce_next_deadline;
static int64_t pce_fps_start;
/* Vary the gap between presented frames so periodic sprite blinks cannot
 * consistently fall on skipped frames, including in audio-priority mode. */
static unsigned pce_video_mode = 1, pce_skip_remaining, pce_skip_pattern;
static bool pce_audio_enabled = true;
static unsigned pce_zoom = 1;
static unsigned pce_state_slot;
static uint32_t pce_rom_id;
bool pce_state_save(uint32_t rom, unsigned slot);
bool pce_state_load(uint32_t rom, unsigned slot);
bool pce_state_delete(uint32_t rom, unsigned slot);

static void pce_options_load(void)
{
    if (!_pax_svc->file_open || !_pax_svc->file_read || !_pax_svc->file_close) return;
    void *f = _pax_svc->file_open("/sd/roms/pax/pce-options.cfg", "rb");
    if (!f) return;
    char buf[64];
    size_t n = _pax_svc->file_read(buf, 1, sizeof(buf) - 1, f);
    _pax_svc->file_close(f);
    buf[n] = '\0';
    unsigned mode, audio, zoom;
    if (sscanf(buf, "%u %u %u", &mode, &audio, &zoom) == 3 &&
        mode < 3 && audio < 2 && zoom < 3) {
        pce_video_mode = mode; pce_audio_enabled = audio != 0; pce_zoom = zoom;
    }
}

static bool pce_options_save(void)
{
    if (!_pax_svc->file_open || !_pax_svc->file_write || !_pax_svc->file_close) return false;
    void *f = _pax_svc->file_open("/sd/roms/pax/pce-options.cfg", "wb");
    if (!f) return false;
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%u %u %u\n", pce_video_mode,
                       pce_audio_enabled ? 1u : 0u, pce_zoom);
    bool ok = len > 0 && _pax_svc->file_write(buf, 1, (size_t)len, f) == (size_t)len;
    return _pax_svc->file_close(f) == 0 && ok;
}

static void pce_menu_action(void *context, uint32_t action, int32_t direction)
{
    (void)context;
    if (!_pax_svc || !_pax_svc->game_menu_set_page) return;
    if (action == 8) { _pax_svc->game_menu_set_page(NULL); return; }
    int delta = direction < 0 ? -1 : 1;
    if (action == 1) pce_video_mode = (unsigned)((pce_video_mode + 3 + delta) % 3);
    if (action == 2) pce_audio_enabled = !pce_audio_enabled;
    if (action == 4) pce_state_slot = (unsigned)((pce_state_slot + 3 + delta) % 3);
    if (action == 3) pce_zoom = (unsigned)((pce_zoom + 3 + delta) % 3);
    static const char *const modes[] = {"SMOOTH", "BALANCED", "AUDIO PRIORITY"};
    static const char *const zooms[] = {"1.5X", "2.0X", "2.5X"};
    pax_game_menu_page_t page = {0};
    snprintf(page.title, sizeof(page.title), "PC ENGINE OPTIONS");
    if (action >= 1 && action <= 3)
        snprintf(page.status, sizeof(page.status), "%s", pce_options_save() ? "SAVED" : "NOT SAVED");
    if (action == 5) snprintf(page.status, sizeof(page.status), "%s", pce_state_save(pce_rom_id, pce_state_slot) ? "STATE SAVED" : "SAVE FAILED");
    if (action == 6) snprintf(page.status, sizeof(page.status), "%s", pce_state_load(pce_rom_id, pce_state_slot) ? "STATE LOADED" : "LOAD FAILED");
    if (action == 7) snprintf(page.status, sizeof(page.status), "%s", pce_state_delete(pce_rom_id, pce_state_slot) ? "STATE DELETED" : "DELETE FAILED");
    page.back_action = 8;
    page.count = 8;
    page.selected = action >= 1 && action <= 7 ? action - 1 : 0;
    page.items[0].action = 1; page.items[0].flags = PAX_GAME_MENU_ITEM_ADJUSTABLE;
    snprintf(page.items[0].label, sizeof(page.items[0].label), "VIDEO: %s", modes[pce_video_mode]);
    page.items[1].action = 2; page.items[1].flags = PAX_GAME_MENU_ITEM_ADJUSTABLE;
    snprintf(page.items[1].label, sizeof(page.items[1].label), "AUDIO: %s", pce_audio_enabled ? "ON" : "OFF");
    page.items[2].action = 3; page.items[2].flags = PAX_GAME_MENU_ITEM_ADJUSTABLE;
    snprintf(page.items[2].label, sizeof(page.items[2].label), "SIZE: %s", zooms[pce_zoom]);
    page.items[3].action = 4; page.items[3].flags = PAX_GAME_MENU_ITEM_ADJUSTABLE;
    snprintf(page.items[3].label, sizeof(page.items[3].label), "STATE SLOT: %u", pce_state_slot + 1);
    page.items[4].action = 5; snprintf(page.items[4].label, sizeof(page.items[4].label), "SAVE STATE");
    page.items[5].action = 6; snprintf(page.items[5].label, sizeof(page.items[5].label), "LOAD STATE");
    page.items[6].action = 7; snprintf(page.items[6].label, sizeof(page.items[6].label), "DELETE STATE");
    page.items[7].action = 8; snprintf(page.items[7].label, sizeof(page.items[7].label), "BACK TO GAME MENU");
    _pax_svc->game_menu_set_page(&page);
}

/* Globals declared by the HuExpress OSD contract. */
unsigned char *osd_gfx_buffer;
char *sbuf[PCE_AUDIO_CHANNELS];

static void pce_free(void *p)
{
    if (p) free(p);
}

void *my_special_alloc(unsigned char speed, unsigned char bytes, unsigned long size)
{
    (void)speed;
    (void)bytes;
    void *p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

/* ZIP loading is deliberately not part of this PAX. The SD sidecar points
 * to a raw .pce image, avoiding a large decompressor and temporary files. */
uint32 zipmgr_probe_file(char *zipFilename, char *foundGameFile)
{
    (void)zipFilename;
    (void)foundGameFile;
    return ZIP_ERROR;
}
uint32 zipmgr_extract_to_disk(char *zipFilename, char *destination)
{
    (void)zipFilename;
    (void)destination;
    return 0;
}
char *zipmgr_extract_to_memory(char *zipFilename, char *cartFilename, size_t *cartSize)
{
    (void)zipFilename;
    (void)cartFilename;
    (void)cartSize;
    return NULL;
}

static unsigned pce_frame_hash(void)
{
    unsigned h = 2166136261u;
    for (int y = 0; y < PCE_OUTPUT_H; y += 7)
        for (int x = 0; x < PCE_OUTPUT_W; x += 11)
            h = (h ^ pce_pixels[y * PCE_OUTPUT_W + x]) * 16777619u;
    return h;
}

static void pce_emit_audio(void)
{
    /* Bresenham-style fractional sample count: 22050 / 60 = 367.5. */
    pce_audio_phase += PCE_AUDIO_RATE;
    unsigned count = pce_audio_phase / 60;
    pce_audio_phase %= 60;
    if (count > PCE_AUDIO_CAP) count = PCE_AUDIO_CAP;

    for (int ch = 0; ch < PCE_AUDIO_CHANNELS; ch++)
        WriteBuffer(pce_audio_channels[ch], ch, count * 2);

    int lvol = (int)((io.psg_volume >> 4) * 1.22f);
    int rvol = (int)((io.psg_volume & 0x0F) * 1.22f);
    for (unsigned i = 0; i < count; i++) {
        int left = 0, right = 0;
        for (int ch = 0; ch < PCE_AUDIO_CHANNELS; ch++) {
            left += (signed char)pce_audio_channels[ch][2 * i];
            right += (signed char)pce_audio_channels[ch][2 * i + 1];
        }
        int l = left * lvol;
        int r = right * rvol;
        if (!pce_audio_enabled) l = r = 0;
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        pce_audio[2 * i] = (int16_t)l;
        pce_audio[2 * i + 1] = (int16_t)r;
        int al = l < 0 ? -l : l;
        int ar = r < 0 ? -r : r;
        if (al > pce_audio_peak) pce_audio_peak = al;
        if (ar > pce_audio_peak) pce_audio_peak = ar;
    }
    emu_audio_submit(pce_audio, count);
}

static int pce_gfx_init(void) { return 1; }
static int pce_gfx_mode(void) { return 1; }
static void pce_gfx_shut(void) {}

static void pce_gfx_draw(void)
{
    int src_w = io.screen_w;
    int src_h = io.screen_h;
    if (src_w <= 0 || src_w > PCE_OUTPUT_W) src_w = PCE_OUTPUT_W;
    if (src_h <= 0 || src_h > PCE_OUTPUT_H) src_h = 224;

    /* Emulation and PSG run every frame even when no frame is presented. */
    bool present = !skipNextFrame;
    if (present) {
        memset(pce_pixels, 0, PCE_OUTPUT_W * PCE_OUTPUT_H * sizeof(*pce_pixels));
        int dst_x = (PCE_OUTPUT_W - src_w) / 2;
        int dst_y = (PCE_OUTPUT_H - src_h) / 2;
        for (int y = 0; y < src_h; y++) {
            const unsigned char *src = osd_gfx_buffer + y * PCE_INDEX_STRIDE;
            uint16_t *dst = pce_pixels + (dst_y + y) * PCE_OUTPUT_W + dst_x;
            for (int x = 0; x < src_w; x++) dst[x] = pce_palette[src[x]];
        }
        static const float scales[] = {1.5f, 2.0f, 2.5f};
        _pax_svc->display_write_frame_custom(pce_pixels, PCE_OUTPUT_W,
                                              PCE_OUTPUT_H, scales[pce_zoom], false);
    }
    pce_emit_audio();

    /* Transparent tiles and sprites leave their destination untouched.
     * This port reuses one render buffer, so prepare a clean background and
     * sprite priority mask before the engine draws the following frame. */
    if (present) {
        /* The visible 240 rows include the 32-pixel side margins. The
         * 64-row top and bottom guards are never copied to the display. */
        memset(pce_xbuf + 64 * XBUF_WIDTH, Pal ? Pal[0] : 0, PCE_OUTPUT_H * XBUF_WIDTH);
        memset(pce_spm_raw + 64 * XBUF_WIDTH, 0, PCE_OUTPUT_H * XBUF_WIDTH);
    }

    if (present) {
        unsigned hash = pce_frame_hash();
        if (pce_frame_count && hash != pce_last_hash) pce_changes++;
        pce_last_hash = hash;
    }
    pce_frame_count++;
    /* Do not render scanlines for the frame we will not present. The CPU,
     * interrupts, game input, and sound still advance on every frame. */
    if (present) {
        /* Keep the sampled frame parity alternating without overloading the
         * CPU and starving 60 Hz audio generation. */
        static const unsigned intervals[3][6] = {
            {2, 3, 2, 3, 2, 3}, {2, 3, 4, 2, 3, 4}, {3, 4, 5, 3, 4, 5}
        };
        unsigned interval = intervals[pce_video_mode][pce_skip_pattern++ % 6];
        pce_skip_remaining = interval - 1;
    } else if (pce_skip_remaining) {
        --pce_skip_remaining;
    }
    skipNextFrame = pce_skip_remaining != 0;
    if (pce_frame_count % 180u == 0) {
        int64_t now = _pax_svc->get_time_us();
        int64_t elapsed = now - pce_fps_start;
        if (elapsed > 0)
            _pax_svc->log_printf("PCE PAX: frames=%u fps=%u.%u audio_peak=%d\n",
                pce_frame_count, (unsigned)(180000000LL / elapsed),
                (unsigned)((1800000000LL / elapsed) % 10), pce_audio_peak);
        pce_fps_start = now;
    }

    if (pce_next_deadline == 0) pce_next_deadline = _pax_svc->get_time_us();
    pce_next_deadline += 1000000 / 60;
    int64_t now = _pax_svc->get_time_us();
    if (pce_next_deadline > now)
        _pax_svc->delay_ms((int)((pce_next_deadline - now) / 1000));
    else if (now - pce_next_deadline > 50000)
        pce_next_deadline = now;

#ifdef PAX_TEST_FRAMES
    if (pce_frame_count >= PAX_TEST_FRAMES) pce_exit_requested = 1;
#endif
}

osd_gfx_driver osd_gfx_driver_list[1] = {
    { pce_gfx_init, pce_gfx_mode, pce_gfx_draw, pce_gfx_shut }
};

void osd_gfx_set_color(uchar index, uchar r, uchar g, uchar b)
{
    /* HuExpress supplies 6-bit VCE components. */
    uint8_t r8 = (uint8_t)((r << 2) | (r >> 4));
    uint8_t g8 = (uint8_t)((g << 2) | (g >> 4));
    uint8_t b8 = (uint8_t)((b << 2) | (b >> 4));
    pce_palette[index] = (uint16_t)(((r8 << 8) & 0xF800) |
                                    ((g8 << 3) & 0x07E0) | (b8 >> 3));
}
void osd_gfx_set_message(char *message)
{
    if (_pax_svc && message) _pax_svc->log_printf("PCE: %s\n", message);
}
uint16 osd_gfx_savepict(void) { return 0; }

void osd_snd_set_volume(uchar volume) { (void)volume; }
int osd_snd_init_sound(void) { return 1; }
void osd_snd_trash_sound(void) {}
int osd_init_machine(void) { return 0; }
void osd_shut_machine(void) {}
int osd_init_input(void) { return 0; }
void osd_shutdown_input(void) {}
char osd_keypressed(void) { return 0; }
uint16 osd_readkey(void) { return 0; }

int osd_keyboard(void)
{
    pax_gamepad_state_t pad;
    _pax_svc->input_gamepad_read(&pad);
    io.JOY[0] = 0;
    if (pad.values[PAX_INPUT_LEFT])  io.JOY[0] |= 0x80;
    if (pad.values[PAX_INPUT_RIGHT]) io.JOY[0] |= 0x20;
    if (pad.values[PAX_INPUT_UP])    io.JOY[0] |= 0x10;
    if (pad.values[PAX_INPUT_DOWN])  io.JOY[0] |= 0x40;
    if (pad.values[PAX_INPUT_A])     io.JOY[0] |= 0x01;
    if (pad.values[PAX_INPUT_B])     io.JOY[0] |= 0x02;
    if (pad.values[PAX_INPUT_SELECT]) io.JOY[0] |= 0x04;
    if (pad.values[PAX_INPUT_START])  io.JOY[0] |= 0x08;
    if (pad.values[PAX_INPUT_MENU] ||
        (_pax_svc->input_l3_read && _pax_svc->input_l3_read()) ||
        pce_exit_requested)
        return 1;
    return 0;
}

/* The generic engine references these OSD CD operations even for cartridge
 * builds. This PAX intentionally supports raw HuCard images only. */
int osd_cd_init(char *device) { (void)device; return 1; }
void osd_cd_stop_audio(void) {}
void osd_cd_close(void) {}
void osd_cd_read(uchar *p, uint32 sector) { (void)sector; memset(p, 0, 2048); }
void osd_cd_subchannel_info(uint16 offset) { (void)offset; }
void osd_cd_status(int *status) { if (status) *status = 0; }
void osd_cd_track_info(uchar track, int *min, int *sec, int *fra, int *control)
{ (void)track; if (min) *min = 0; if (sec) *sec = 0; if (fra) *fra = 0; if (control) *control = 0; }
void osd_cd_nb_tracks(int *first, int *last) { if (first) *first = 1; if (last) *last = 1; }
void osd_cd_length(int *min, int *sec, int *fra) { if (min) *min = 0; if (sec) *sec = 0; if (fra) *fra = 0; }
void osd_cd_pause(void) {}
void osd_cd_resume(void) {}
void osd_cd_play_audio_track(uchar track) { (void)track; }
void osd_cd_play_audio_range(uchar min_from, uchar sec_from, uchar fra_from,
                             uchar min_to, uchar sec_to, uchar fra_to)
{ (void)min_from; (void)sec_from; (void)fra_from; (void)min_to; (void)sec_to; (void)fra_to; }

static int pce_load_sidecar(char *path, size_t size)
{
    return emu_rom_path("pce", ".pce|", path, size);
}

static void pce_free_paths(void)
{
    pce_free(cart_name); cart_name = NULL;
    pce_free(short_cart_name); short_cart_name = NULL;
    pce_free(short_iso_name); short_iso_name = NULL;
    pce_free(rom_file_name); rom_file_name = NULL;
    pce_free(config_basepath); config_basepath = NULL;
    pce_free(sav_path); sav_path = NULL;
    pce_free(sav_basepath); sav_basepath = NULL;
    pce_free(tmp_basepath); tmp_basepath = NULL;
    pce_free(video_path); video_path = NULL;
    pce_free(ISO_filename); ISO_filename = NULL;
    pce_free(syscard_filename); syscard_filename = NULL;
    pce_free(cdsystem_path); cdsystem_path = NULL;
    pce_free(log_filename); log_filename = NULL;
}

static int pce_alloc_paths(void)
{
    char **paths[] = { &cart_name, &short_cart_name, &short_iso_name,
        &rom_file_name, &config_basepath, &sav_path, &sav_basepath,
        &tmp_basepath, &video_path, &ISO_filename, &syscard_filename,
        &cdsystem_path, &log_filename };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        *paths[i] = calloc(1, 512);
        if (!*paths[i]) return -1;
    }
    strcpy(config_basepath, "/sd/roms/pax");
    strcpy(sav_basepath, "/sd/roms/pax");
    strcpy(sav_path, "pce");
    return 0;
}

static int pce_alloc_buffers(void)
{
    pce_xbuf = calloc(XBUF_WIDTH * XBUF_HEIGHT, 1);
    pce_spm_raw = calloc(XBUF_WIDTH * XBUF_HEIGHT, 1);
    pce_sprite_positions = calloc(1024, sizeof(*pce_sprite_positions));
    pce_palette = calloc(256, sizeof(*pce_palette));
    pce_pixels = calloc(PCE_OUTPUT_W * PCE_OUTPUT_H, sizeof(*pce_pixels));
    for (int i = 0; i < PCE_AUDIO_CHANNELS; i++)
        pce_audio_channels[i] = calloc(PCE_AUDIO_CAP * 2, 1);
    pce_audio = calloc(PCE_AUDIO_CAP * 2, sizeof(*pce_audio));
    if (!pce_xbuf || !pce_spm_raw || !pce_sprite_positions || !pce_palette ||
        !pce_pixels || !pce_audio) return -1;
    for (int i = 0; i < PCE_AUDIO_CHANNELS; i++)
        if (!pce_audio_channels[i]) return -1;
    osd_gfx_buffer = pce_xbuf + 32 + 64 * XBUF_WIDTH;
    SPM_raw = pce_spm_raw;
    SPM = pce_spm_raw + 64 * XBUF_WIDTH + 32;
    spr_init_pos = pce_sprite_positions;
    for (int i = 0; i < PCE_AUDIO_CHANNELS; i++) sbuf[i] = pce_audio_channels[i];
    return 0;
}

static void pce_free_buffers(void)
{
    pce_free(pce_xbuf); pce_xbuf = NULL; osd_gfx_buffer = NULL;
    pce_free(pce_spm_raw); pce_spm_raw = NULL; SPM_raw = NULL; SPM = NULL;
    pce_free(pce_sprite_positions); pce_sprite_positions = NULL; spr_init_pos = NULL;
    pce_free(pce_palette); pce_palette = NULL;
    pce_free(pce_pixels); pce_pixels = NULL;
    for (int i = 0; i < PCE_AUDIO_CHANNELS; i++) {
        pce_free(pce_audio_channels[i]);
        pce_audio_channels[i] = NULL;
        sbuf[i] = NULL;
    }
    pce_free(pce_audio); pce_audio = NULL;
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAX_ABI_VERSION) return -1;
    _pax_svc = svc;
    pce_options_load();
    char rom_path[512];
    if (pce_load_sidecar(rom_path, sizeof(rom_path))) {
        svc->log_printf("PCE PAX: set /sd/roms/pax/pce.rom to a .pce file\n");
        return -1;
    }

    pce_rom_id = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)rom_path; *p; ++p)
        pce_rom_id = (pce_rom_id ^ *p) * 16777619u;
    pce_state_slot = 0;
    int result = -1;
    if (pce_alloc_paths() == 0 && pce_alloc_buffers() == 0) {
        host.sound.stereo = 1;
        host.sound.signed_sound = 1;
        host.sound.freq = PCE_AUDIO_RATE;
        host.sound.sample_size = 1;
        option.want_supergraphx_emulation = 0;
        option.want_fullscreen_aspect = 1;
        video_driver = 0;
        UPeriod = 0;
        if (!InitPCE(rom_path)) {
            SetPalette();
            emu_audio_start(PCE_AUDIO_RATE);
            svc->log_printf("PCE PAX: running %s\n", rom_path);
            pce_frame_count = 0;
            pce_changes = 0;
            pce_last_hash = 0;
            pce_audio_peak = 0;
            pce_audio_phase = 0;
            pce_next_deadline = 0;
            pce_fps_start = svc->get_time_us();
            pce_exit_requested = 0;
            skipNextFrame = false;
            pce_skip_remaining = pce_skip_pattern = 0;
            if (svc->game_menu_register && svc->game_menu_set_page)
                svc->game_menu_register("PC ENGINE OPTIONS", pce_menu_action, NULL);
            RunPCE();
            if (svc->game_menu_register)
                svc->game_menu_register(NULL, NULL, NULL);
            emu_audio_finish();
            svc->log_printf("PCE test/runtime: frames=%u changed=%u audio_peak=%d\n",
                            pce_frame_count, pce_changes, pce_audio_peak);
            TrashPCE();
            result = 0;
        }
    }
    pce_free_buffers();
    pce_free_paths();
    pax_close_streams();
    pax_cleanup_fds();
    pax_cleanup_heap();
    svc->display_clear(0);
    svc->display_flush();
    return result;
}
