#define PAX_APP_SIDE 1
#include "psram_app.h"
#define EMU_AUDIO_SLOTS 6
#define EMU_AUDIO_PREFILL 3
#include "runtime.h"
#include "audio_queue.h"
#include "snes9x.h"
#include "memmap.h"
#include "apu.h"
#include "cpuexec.h"
#include "display.h"
#include "gfx.h"
#include "ppu.h"
#include "soundux.h"
#include "save.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNES_PAX_VERSION "0.1.8"

const app_services_t *_pax_svc;
bool overclock_cycles = false;
int one_c = 4, slow_one_c = 5, two_c = 6;
char *rom_filename = NULL;

static uint16_t *snes_fb;
static uint8_t *snes_subscreen;
static uint8_t *snes_zbuffer;
static uint8_t *snes_subzbuffer;
static int16_t *snes_audio;
static jmp_buf exit_env;
static unsigned snes_frame_count;

int64_t pax_esp_timer_get_time(void)
{
    return _pax_svc ? _pax_svc->get_time_us() : 0;
}

void app_return_to_launcher(void) { longjmp(exit_env, 1); }

uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0) return 0;
    pax_gamepad_state_t pad;
    _pax_svc->input_gamepad_read(&pad);
#ifdef PAX_TEST_FRAMES
    memset(&pad, 0, sizeof(pad));
    pad.values[PAX_INPUT_START] = snes_frame_count >= 60 && snes_frame_count < 65;
    pad.values[PAX_INPUT_A] = snes_frame_count >= 180 && snes_frame_count < 185;
#endif
    uint32_t joy = 0;
    if (pad.values[PAX_INPUT_UP])     joy |= SNES_UP_MASK;
    if (pad.values[PAX_INPUT_DOWN])   joy |= SNES_DOWN_MASK;
    if (pad.values[PAX_INPUT_LEFT])   joy |= SNES_LEFT_MASK;
    if (pad.values[PAX_INPUT_RIGHT])  joy |= SNES_RIGHT_MASK;
    if (pad.values[PAX_INPUT_A])      joy |= SNES_A_MASK;
    if (pad.values[PAX_INPUT_B])      joy |= SNES_B_MASK;
    if (pad.values[PAX_INPUT_X])      joy |= SNES_X_MASK;
    if (pad.values[PAX_INPUT_Y])      joy |= SNES_Y_MASK;
    if (pad.values[PAX_INPUT_L])      joy |= SNES_TL_MASK;
    if (pad.values[PAX_INPUT_R])      joy |= SNES_TR_MASK;
    if (pad.values[PAX_INPUT_SELECT]) joy |= SNES_SELECT_MASK;
    if (pad.values[PAX_INPUT_START])  joy |= SNES_START_MASK;
    return joy;
}

bool S9xReadMousePosition(int32_t which, int32_t *x, int32_t *y, uint32_t *buttons)
{ (void)which; (void)x; (void)y; (void)buttons; return false; }
bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{ (void)x; (void)y; (void)buttons; return false; }
bool JustifierOffscreen(void) { return true; }
void JustifierButtons(uint32_t *justifiers) { (void)justifiers; }
void S9xToggleSoundChannel(int32_t channel) { (void)channel; }

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = (uint8_t *)snes_fb;
    GFX.SubScreen = snes_subscreen;
    GFX.ZBuffer = snes_zbuffer;
    GFX.SubZBuffer = snes_subzbuffer;
    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}
void S9xDeinitDisplay(void) {}

static void snes_clear_core(void)
{
    if (GFX.ZBuffer) { free(GFX.ZBuffer); GFX.ZBuffer = NULL; }
    if (GFX.SubZBuffer) { free(GFX.SubZBuffer); GFX.SubZBuffer = NULL; }
    S9xDeinitGFX();
    S9xDeinitAPU();
    S9xDeinitMemory();
    free(snes_fb); snes_fb = NULL;
    free(snes_subscreen); snes_subscreen = NULL;
    snes_zbuffer = snes_subzbuffer = NULL;
    free(snes_audio); snes_audio = NULL;
}

static int snes_load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 1024 || size > MAX_ROM_SIZE) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    Memory.ROM = calloc(1, MAX_ROM_SIZE);
    if (!Memory.ROM || fread(Memory.ROM, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); return -1;
    }
    fclose(f);
    if (((size_t)size & 0x7ffu) == 512u) {
        memmove(Memory.ROM, Memory.ROM + 512, (size_t)size - 512);
        size -= 512;
    }
    Memory.ROM_Size = (uint32_t)size;
    return LoadROM(NULL) ? 0 : -1;
}

static unsigned snes_hash(void)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < SNES_WIDTH * SNES_HEIGHT; i += 17)
        h = (h ^ snes_fb[i]) * 16777619u;
    return h;
}


#define SNES_STATE_SLOTS 10
static uint32_t snes_rom_id;
static unsigned snes_state_slot;
static bool snes_force_render;

static void snes_state_path(char *out, size_t cap, unsigned slot, const char *suffix)
{
    snprintf(out, cap, "/sd/roms/pax/snes-%08lx-%u.state%s",
             (unsigned long)snes_rom_id, slot, suffix ? suffix : "");
}

static bool snes_state_exists(unsigned slot)
{
    if (!_pax_svc || !_pax_svc->file_stat) return false;
    char path[96]; pax_file_stat_t info;
    snes_state_path(path, sizeof(path), slot, NULL);
    return _pax_svc->file_stat(path, &info) == 0 && !info.is_dir;
}

static bool snes_state_save(unsigned slot)
{
    if (!_pax_svc || !_pax_svc->file_rename || !_pax_svc->file_remove ||
        !_pax_svc->file_stat) return false;
    char path[96], temp[100], backup[100];
    snes_state_path(path, sizeof(path), slot, NULL);
    snes_state_path(temp, sizeof(temp), slot, ".tmp");
    snes_state_path(backup, sizeof(backup), slot, ".bak");
    pax_file_stat_t info;
    if (_pax_svc->file_stat(temp, &info) == 0 && _pax_svc->file_remove(temp) != 0)
        return false;
    if (!S9xSaveState(temp)) return false;
    if (_pax_svc->file_stat(temp, &info) != 0 || !!info.is_dir || info.size < 16) {
        _pax_svc->file_remove(temp); return false;
    }
    bool had_old = false;
    if (_pax_svc->file_stat(path, &info) == 0) {
        if (_pax_svc->file_stat(backup, &info) == 0 ||
            _pax_svc->file_rename(path, backup) != 0) {
            _pax_svc->file_remove(temp); return false;
        }
        had_old = true;
    }
    if (_pax_svc->file_rename(temp, path) != 0) {
        if (had_old) _pax_svc->file_rename(backup, path);
        _pax_svc->file_remove(temp); return false;
    }
    if (had_old) _pax_svc->file_remove(backup);
    return true;
}

static bool snes_state_load(unsigned slot)
{
    char path[96]; snes_state_path(path, sizeof(path), slot, NULL);
    return S9xLoadState(path);
}

static bool snes_state_delete(unsigned slot)
{
    if (!_pax_svc || !_pax_svc->file_remove || !snes_state_exists(slot)) return false;
    char path[96]; snes_state_path(path, sizeof(path), slot, NULL);
    return _pax_svc->file_remove(path) == 0;
}

enum { SNES_MENU_SLOT = 1, SNES_MENU_SAVE, SNES_MENU_LOAD, SNES_MENU_DELETE, SNES_MENU_BACK };
static void snes_shared_menu_action(void *context, uint32_t action, int32_t direction)
{
    (void) context;
    if (!_pax_svc || !_pax_svc->game_menu_set_page) return;
    if (action == SNES_MENU_BACK) { _pax_svc->game_menu_set_page(NULL); return; }
    if (action == SNES_MENU_SLOT)
        snes_state_slot = (snes_state_slot + SNES_STATE_SLOTS + (direction < 0 ? -1 : 1)) % SNES_STATE_SLOTS;
    const char *status = "";
    if (action == SNES_MENU_SAVE) {
        bool saved = snes_state_save(snes_state_slot);
        status = saved ? "STATE SAVED" : "SAVE FAILED";
        if (_pax_svc->log_printf)
            _pax_svc->log_printf("SNES state save slot=%u result=%s\n", snes_state_slot + 1, saved ? "ok" : "failed");
    } else if (action == SNES_MENU_LOAD) {
        bool loaded = snes_state_load(snes_state_slot);
        if (_pax_svc->log_printf)
            _pax_svc->log_printf("SNES state load slot=%u result=%s\n", snes_state_slot + 1, loaded ? "ok" : "failed");
        if (loaded) {
            snes_force_render = true;
            if (_pax_svc->game_menu_close) _pax_svc->game_menu_close();
            return;
        }
        status = snes_state_exists(snes_state_slot) ? "LOAD FAILED" : "EMPTY SLOT";
    } else if (action == SNES_MENU_DELETE) {
        bool deleted = snes_state_delete(snes_state_slot);
        status = deleted ? "STATE DELETED" : "DELETE FAILED";
        if (_pax_svc->log_printf)
            _pax_svc->log_printf("SNES state delete slot=%u result=%s\n", snes_state_slot + 1, deleted ? "ok" : "failed");
    }
    pax_game_menu_page_t page = {0};
    snprintf(page.title, sizeof(page.title), "SNES OPTIONS %s", SNES_PAX_VERSION);
    snprintf(page.status, sizeof(page.status), "%s", status);
    page.count = 5; page.selected = action >= SNES_MENU_SLOT && action <= SNES_MENU_DELETE ? action - 1 : 0;
    page.back_action = SNES_MENU_BACK;
    page.items[0].action = SNES_MENU_SLOT; page.items[0].flags = PAX_GAME_MENU_ITEM_ADJUSTABLE;
    snprintf(page.items[0].label, sizeof(page.items[0].label), "STATE SLOT %u: %s",
             snes_state_slot + 1, snes_state_exists(snes_state_slot) ? "SAVED" : "EMPTY");
    page.items[1].action = SNES_MENU_SAVE; snprintf(page.items[1].label, sizeof(page.items[1].label), "SAVE STATE");
    page.items[2].action = SNES_MENU_LOAD; snprintf(page.items[2].label, sizeof(page.items[2].label), "LOAD STATE");
    page.items[3].action = SNES_MENU_DELETE; snprintf(page.items[3].label, sizeof(page.items[3].label), "DELETE STATE");
    page.items[4].action = SNES_MENU_BACK; snprintf(page.items[4].label, sizeof(page.items[4].label), "BACK TO GAME MENU");
    _pax_svc->game_menu_set_page(&page);
}

static void snes_hash_rom(void)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < Memory.ROM_Size; ++i)
        h = (h ^ Memory.ROM[i]) * 16777619u;
    snes_rom_id = h;
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAX_ABI_VERSION) return -1;
    _pax_svc = svc;
    char rom_path[512];
    if (emu_rom_path("snes", ".smc|.sfc|", rom_path, sizeof(rom_path))) {
        svc->log_printf("SNES: set ROM path in /sd/roms/pax/snes.rom\n");
        return -1;
    }

    int result = -1;
    if (setjmp(exit_env)) goto cleanup;

    snes_fb = calloc(SNES_WIDTH * SNES_HEIGHT_EXTENDED, sizeof(uint16_t));
    snes_subscreen = calloc(SNES_WIDTH * SNES_HEIGHT_EXTENDED, sizeof(uint16_t));
    snes_zbuffer = calloc(512 * SNES_HEIGHT_EXTENDED, 1);
    snes_subzbuffer = calloc(512 * SNES_HEIGHT_EXTENDED, 1);
    /* 32 kHz / 50 Hz (PAL) is the largest per-frame stereo block. */
    snes_audio = calloc(((32000 + 49) / 50) * 2, sizeof(int16_t));
    if (!snes_fb || !snes_subscreen || !snes_zbuffer || !snes_subzbuffer || !snes_audio)
        app_return_to_launcher();

    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = 32000;
    Settings.DisableSoundEcho = false;
    Settings.InterpolatedSound = true;

    if (!S9xInitMemory() || !S9xInitAPU() || !S9xInitSound(0, 0) ||
        !S9xInitDisplay() || !S9xInitGFX() || snes_load_rom(rom_path) < 0)
        app_return_to_launcher();
    S9xSetPlaybackRate(Settings.SoundPlaybackRate);
    snes_hash_rom();
    snes_state_slot = 0;
    snes_force_render = false;
    emu_audio_start(32000);
    if (svc->game_menu_register && svc->game_menu_set_page)
        svc->game_menu_register("SNES OPTIONS", snes_shared_menu_action, NULL);
    svc->log_printf("SNES PAX: running %s (%s)\n", rom_path,
                    Settings.PAL ? "PAL" : "NTSC");

    int hz = Settings.PAL ? 50 : 60;
    unsigned sample_phase = 0;
    int64_t deadline = svc->get_time_us();
    int64_t fps_start = deadline;
    unsigned frame_count = 0, changes = 0, last_hash = 0;
    snes_frame_count = 0;
    int peak = 0, max_step = 0;
    unsigned drawn_frames = 0, late_frame_skips = 0;
    int16_t previous_audio[2] = {0, 0};
    for (;;) {
        pax_gamepad_state_t pad;
        svc->input_gamepad_read(&pad);
#ifdef PAX_TEST_FRAMES
        memset(&pad, 0, sizeof(pad));
        pad.values[PAX_INPUT_START] = frame_count >= 60 && frame_count < 65;
        pad.values[PAX_INPUT_A] = frame_count >= 180 && frame_count < 185;
#endif
        if (pad.values[PAX_INPUT_MENU] && !pad.values[PAX_INPUT_X] && !pad.values[PAX_INPUT_Y])
            break;
        /* SNES rendering is expensive on this board. Keep the CPU, timers and
         * APU running on every frame, but draw/present every third one. When
         * an emulation frame is already late, skip that scheduled draw too so
         * raster work does not deepen audio queue starvation. */
        bool scheduled_render = (frame_count % 3u) == 0;
        bool render_frame = (scheduled_render || snes_force_render) &&
                            (snes_force_render || svc->get_time_us() <= deadline + 2000);
        if (scheduled_render && !render_frame) ++late_frame_skips;
        if (render_frame) snes_force_render = false;
        if (render_frame) ++drawn_frames;
        IPPU.RenderThisFrame = render_frame;
        S9xMainLoop();
        /* Carry the fractional 32 kHz samples across frames: NTSC alternates
         * 533/534 frames, while PAL emits exactly 640 per frame. */
        sample_phase += 32000;
        unsigned sample_frames = sample_phase / (unsigned)hz;
        sample_phase %= (unsigned)hz;
        S9xMixSamples(snes_audio, (int32_t)(sample_frames * 2));
        for (unsigned i = 0; i < sample_frames * 2; ++i) {
            int value = snes_audio[i];
            int magnitude = value < 0 ? -value : value;
            if (magnitude > peak) peak = magnitude;
            int delta = value - (i < 2 ? previous_audio[i] : snes_audio[i - 2]);
            if (delta < 0) delta = -delta;
            if (delta > max_step) max_step = delta;
        }
        previous_audio[0] = snes_audio[sample_frames * 2 - 2];
        previous_audio[1] = snes_audio[sample_frames * 2 - 1];
        emu_audio_submit(snes_audio, sample_frames);
        if (render_frame)
            svc->display_write_frame_custom(snes_fb, SNES_WIDTH, SNES_HEIGHT, 2.0f, false);
#ifdef PAX_TEST_FRAMES
        {
            unsigned hash = snes_hash();
            if (frame_count && hash != last_hash) ++changes;
            last_hash = hash;
        }
#endif
        ++frame_count;
        snes_frame_count = frame_count;
#ifdef PAX_TEST_FRAMES
        if (frame_count >= PAX_TEST_FRAMES) {
            svc->log_printf("SNES test: frames=%u changed=%u audio_peak=%d elapsed_ms=%ld\n",
                            frame_count, changes, peak,
                            (long)((svc->get_time_us() - deadline) / 1000));
            break;
        }
#endif
        if (frame_count % 180u == 0) {
            int64_t now_log = svc->get_time_us();
            int64_t elapsed = now_log - fps_start;
            if (elapsed > 0)
                svc->log_printf("SNES PAX: frames=%u fps=%u.%u drawn=%u late_skips=%u audio_peak=%d audio_step=%d\n",
                    frame_count, (unsigned)(180000000LL / elapsed),
                    (unsigned)((1800000000LL / elapsed) % 10), drawn_frames,
                    late_frame_skips, peak, max_step);
            fps_start = now_log;
            peak = max_step = 0;
            drawn_frames = late_frame_skips = 0;
        }
        deadline += 1000000 / hz;
        int64_t now = svc->get_time_us();
        if (deadline > now) svc->delay_ms((int)((deadline - now) / 1000));
        else if (now - deadline > 50000) deadline = now;
    }
    result = 0;
    if (svc->game_menu_register) svc->game_menu_register(NULL, NULL, NULL);
    emu_audio_finish();

cleanup:
    snes_clear_core();
    pax_close_streams();
    pax_cleanup_fds();
    pax_cleanup_heap();
    svc->display_clear(0);
    svc->display_flush();
    return result;
}
