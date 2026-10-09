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

    /* Scale 1:1 — pax_i_video uses display_write_frame_custom with 2.4f */
    svc->display_set_scale(1.0f, 1.0f);

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
