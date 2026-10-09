/*
 * PSRAM App entry point for Doom (prboom-go).
 *
 * Receives the service table from the launcher, stores it globally,
 * then starts the Doom engine on the current thread with a watchdog
 * task monitoring the MENU button for a clean exit.
 */
#include "psram_app.h"
#include <setjmp.h>
#include <string.h>

/* ── From prboom engine ──────────────────────────────────────────────── */
extern int SCREENWIDTH;
extern int SCREENHEIGHT;
extern char **myargv;
extern int myargc;
extern void Z_Init(void);
extern void D_DoomMain(void);

/* Global service pointer — used by all shim + syscall code */
const app_services_t *_pax_svc = NULL;

/* Exit mechanism: longjmp back to entry on clean exit request */
static jmp_buf s_exit_jmp;
volatile int pax_exit_requested = 0;

/* Watchdog task handle */
static void *s_wd_handle = NULL;

/* Sound task handle (created by pax_i_sound.c) */
extern void *doom_sound_task_handle;

/*
 * app_return_to_launcher — called from game code (I_SafeExit, etc.)
 * to immediately abort game execution and return to the launcher.
 */
void app_return_to_launcher(void)
{
    pax_exit_requested = 1;
    longjmp(s_exit_jmp, 1);
}

/* Called by RG_PANIC macro in rg_system.h stub */
void pax_panic(const char *msg)
{
    if (_pax_svc)
        _pax_svc->log_printf("DOOM PANIC: %s\n", msg);
    app_return_to_launcher();
}

/* Watchdog: polls the dedicated L3 close control, with MENU as a fallback
   for older launchers that do not expose input_l3_read. */
static void watchdog_task(void *arg)
{
    (void)arg;
    pax_gamepad_state_t state;
    int held_ms = 0;

    for (;;) {
        _pax_svc->input_gamepad_read(&state);
        const int close_pressed = (_pax_svc->input_l3_read != NULL)
            ? _pax_svc->input_l3_read()
            : state.values[PAX_INPUT_MENU];
        if (close_pressed) {
            held_ms += 100;
            if (held_ms >= 3000) {
                _pax_svc->log_printf("Doom PAX: L3 held 3s, requesting exit\n");
                pax_exit_requested = 1;
                /* Do NOT return — returning from a FreeRTOS task function
                   calls prvTaskExitError() → abort() → reboot.  Spin here
                   until the cleanup path deletes this task. */
                while (1) _pax_svc->delay_ms(100);
            }
        } else {
            held_ms = 0;
        }
        _pax_svc->delay_ms(100);
    }
}

/*
 * The canvas: the largest of Doom's sizes that fits in what the loader offers
 * (display_get_size: the store's Screen setting, else the listing's
 * recommended size, else the device default), or the smallest when none does.
 * The 320x200 frame is scaled to fill it (pax_i_video.c). A launcher without
 * display_set_canvas, or one that refuses the size, keeps the 800x480 canvas
 * and the original 2.4x (768x480).
 */
float pax_doom_scale = 2.4f;

static void choose_canvas(const app_services_t *svc)
{
    /* Largest first; the same sizes as "canvas" in pax.json. */
    static const struct { int w, h, tenths; } sizes[] = {
        {960, 600, 30},   /* 960x600: 3x, the panel's full height       */
        {800, 480, 24},   /* 768x480 on the classic canvas              */
        {640, 400, 20},   /* 640x400: 2x, the least to send per frame   */
    };
    const int count = (int)(sizeof(sizes) / sizeof(sizes[0]));
    if (!svc->display_get_size || !svc->display_set_canvas)
        return;
    int offer_w = 0, offer_h = 0;
    svc->display_get_size(&offer_w, &offer_h);
    int pick = count - 1;
    for (int i = 0; i < count; i++) {
        if (sizes[i].w <= offer_w && sizes[i].h <= offer_h) {
            pick = i;
            break;
        }
    }
    if (svc->display_set_canvas(sizes[pick].w, sizes[pick].h) == 0) {
        pax_doom_scale = sizes[pick].tenths / 10.0f;
        svc->log_printf("Doom PAX: offered a %dx%d canvas, using %dx%d (320x200 x%d.%d)\n",
                        offer_w, offer_h, sizes[pick].w, sizes[pick].h,
                        sizes[pick].tenths / 10, sizes[pick].tenths % 10);
    } else {
        svc->log_printf("Doom PAX: %dx%d canvas refused, keeping 800x480\n",
                        sizes[pick].w, sizes[pick].h);
    }
}

/* Cleanup functions from pax_syscalls.c */
extern void pax_cleanup_fds(void);
extern void pax_cleanup_heap(void);

/* Sound shutdown from pax_i_sound.c */
extern void pax_sound_shutdown(void);

/*
 * app_entry — PSRAM app ABI entry point.
 * Must be placed in .text.entry (offset 0 in the .pax binary).
 */
__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    _pax_svc = svc;
    pax_exit_requested = 0;

    svc->log_printf("=== Doom PSRAM App Starting ===\n");

    /* Scale 1:1 — pax_i_video uses display_write_frame_custom with pax_doom_scale */
    svc->display_set_scale(1.0f, 1.0f);

    /* Pick the canvas on this task, which draws (I_FinishUpdate). */
    choose_canvas(svc);

    /* Start watchdog on core 1 */
    svc->task_create(watchdog_task, "doom_wd", 4096, NULL, 1, &s_wd_handle, 1);

    /* Set up Doom screen dimensions */
    SCREENWIDTH  = 320;
    SCREENHEIGHT = 200;

    /* Set up Doom command-line arguments */
    static char *doom_argv[] = {
        "doom",
        "-save", "/sd/saves/doom",
        "-iwad", "/sd/roms/doom/doom.wad",
        NULL
    };
    myargv = doom_argv;
    myargc = 5;

    /* Run game on this thread (setjmp for emergency exit) */
    if (setjmp(s_exit_jmp) == 0) {
        Z_Init();
        D_DoomMain();   /* never returns normally — runs game loop */
    }

    svc->log_printf("=== Doom PSRAM App Exiting ===\n");

    /* ── Comprehensive cleanup ────────────────────────────────────── */

    /* 1. Kill watchdog task */
    if (s_wd_handle) {
        svc->task_delete(s_wd_handle);
        s_wd_handle = NULL;
    }

    /* 2. Kill sound task + free its buffers */
    pax_sound_shutdown();

    /* 3. Close any file handles still open */
    pax_cleanup_fds();

    /* 4. Free ALL remaining heap allocations (zone memory, WAD data, etc.) */
    pax_cleanup_heap();

    /* 5. Clear display before returning to launcher */
    svc->display_clear(0x0000);
    svc->display_flush();

    return 0;
}
