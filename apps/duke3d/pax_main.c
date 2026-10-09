/*
 * PSRAM App entry point for Duke Nukem 3D.
 *
 * Receives the service table from the launcher, stores it globally,
 * then starts the Duke3D engine on a separate task with a watchdog
 * monitoring the MENU button for a clean exit.
 */
#include "psram_app.h"
#include <setjmp.h>
#include <string.h>

/* ── From Duke3D game.c ──────────────────────────────────────────────── */
extern int main(int argc, char **argv);

/* Global service pointer — used by all shim + syscall code */
const app_services_t *_pax_svc = NULL;

/* Exit mechanism: longjmp back to entry on clean exit request */
static jmp_buf s_exit_jmp;
volatile int pax_exit_requested = 0;

/* Task handles */
static void *s_wd_handle = NULL;
static void *s_game_task_handle = NULL;
static volatile int s_game_done = 0;

/* Sound task handle (created by pax_sdl_audio.c) */
extern void pax_sound_shutdown(void);

/*
 * app_return_to_launcher — called from game code or exit path
 * to immediately abort game execution and return to the launcher.
 */
void app_return_to_launcher(void)
{
    pax_exit_requested = 1;
    longjmp(s_exit_jmp, 1);
}

/* Watchdog: polls MENU button, sets exit flag after 3 seconds held */
static void watchdog_task(void *arg)
{
    (void)arg;
    pax_gamepad_state_t state;
    int held_ms = 0;

    for (;;) {
        if (_pax_svc->input_l3_read && _pax_svc->input_l3_read()) {
            pax_exit_requested = 1;
            while (1) _pax_svc->delay_ms(100);
        }
        _pax_svc->input_gamepad_read(&state);
        if (state.values[PAX_INPUT_MENU]) {
            held_ms += 100;
            if (held_ms >= 3000) {
                _pax_svc->log_printf("Duke3D PAX: MENU held 3s, requesting exit\n");
                pax_exit_requested = 1;
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

/* The loader does not enter through newlib's CRT. Initialize the C++ MIDI
 * and resampler objects before either worker can access them. Their owned
 * audio resources are explicitly closed by pax_sound_shutdown(). */
typedef void (*pax_init_fn)(void);
extern pax_init_fn __preinit_array_start[], __preinit_array_end[];
extern pax_init_fn __init_array_start[], __init_array_end[];
static void pax_run_constructors(void)
{
    for (pax_init_fn *fn = __preinit_array_start; fn != __preinit_array_end; ++fn)
        if (*fn) (*fn)();
    for (pax_init_fn *fn = __init_array_start; fn != __init_array_end; ++fn)
        if (*fn) (*fn)();
}

/* Game task: runs Duke3D on a separate task with 256KB stack */
static void duke3d_game_task(void *arg)
{
    (void)arg;
    static char *duke_argv[] = { "duke3d", NULL };

    if (setjmp(s_exit_jmp) == 0) {
        main(1, duke_argv);
    }
    s_game_done = 1;
    /* Suspend self — entry task will delete us */
    while (1) _pax_svc->delay_ms(1000);
}

/*
 * app_entry — PSRAM app ABI entry point.
 * Must be placed in .text.entry (offset 0 in the .pax binary).
 */
__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    _pax_svc = svc;
    pax_run_constructors();
    pax_exit_requested = 0;
    s_game_done = 0;

    svc->log_printf("=== Duke3D PSRAM App Starting ===\n");

    /* Scale: 320×200 → 2.4× = 768×480 — fills the 480×800 LCD nicely */
    svc->display_set_scale(1.0f, 1.0f);

    /* Start watchdog on core 1 (like Quake) */
    svc->task_create(watchdog_task, "duke_wd", 4096, NULL, 1, &s_wd_handle, 1);

    /* Run game on core 0 with 256KB stack (like Quake).
       CPU1 IDLE WDT is disabled in sdkconfig so core 0 is fine. */
    int rc = svc->task_create(duke3d_game_task, "duke3d", 262144, NULL, 5,
                              &s_game_task_handle, 0);
    if (rc != 0) {
        svc->log_printf("DUKE3D: task_create FAILED (rc=%d)\n", rc);
    } else {
        svc->log_printf("DUKE3D: game task created OK (256KB stack)\n");
    }

    /* Wait for game to finish */
    while (!s_game_done && !pax_exit_requested) {
        svc->delay_ms(100);
    }
    /* Give the game its normal _exit/longjmp path before deleting it.
     * This also lets in-flight heap/file calls finish cooperatively. */
    for (int i = 0; i < 100 && !s_game_done; ++i) svc->delay_ms(10);

    svc->log_printf("=== Duke3D PSRAM App Exiting ===\n");

    /* ── Comprehensive cleanup ────────────────────────────────────── */

    /* 1. Kill game task */
    if (s_game_task_handle) {
        svc->task_delete(s_game_task_handle);
        s_game_task_handle = NULL;
    }

    /* 2. Kill watchdog task */
    if (s_wd_handle) {
        svc->task_delete(s_wd_handle);
        s_wd_handle = NULL;
    }

    /* 3. Shutdown sound task + free buffers */
    pax_sound_shutdown();

    /* 4. Close any file handles still open */
    pax_cleanup_fds();

    /* 5. Free ALL remaining heap allocations */
    pax_cleanup_heap();

    /* 6. Clear display before returning to launcher */
    svc->display_clear(0x0000);
    svc->display_flush();

    return 0;
}
