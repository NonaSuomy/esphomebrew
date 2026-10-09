#include "psram_app.h"
#include "quake2.h"
#ifdef QUAKE2_TEST_NEWGAME
#include "client/client.h"
#else
#include "qcommon/qcommon.h"
#endif
#include "client/keys.h"
#include "pax_video_memory.h"
#include <setjmp.h>
#include <string.h>
#ifdef QUAKE2_TEST_DEATH
extern void (*m_drawfunc)(void);
extern void LoadGame_MenuDraw(void);
#endif

const app_services_t *_pax_svc;
volatile int pax_exit_requested;
static jmp_buf exit_jmp;
static volatile int game_done;
static void *game_task_handle;
static void *watchdog_handle;

extern void pax_input_poll(void);
extern void pax_cleanup_fds(void);
extern void pax_cleanup_heap(void);
extern void pax_sound_shutdown(void);
extern void Q_Arena_Shutdown(void);

void app_return_to_launcher(void) {
    pax_exit_requested = 1;
    longjmp(exit_jmp, 1);
}

static void watchdog(void *arg) {
    (void)arg;
    pax_gamepad_state_t pad;
    int held = 0;
    for (;;) {
        if (_pax_svc->input_l3_read && _pax_svc->input_l3_read()) {
            pax_exit_requested = 1;
            for (;;) _pax_svc->delay_ms(100);
        }
        _pax_svc->input_gamepad_read(&pad);
        if (pad.values[PAX_INPUT_MENU]) {
            if ((held += 100) >= 3000) {
                pax_exit_requested = 1;
                for (;;) _pax_svc->delay_ms(100);
            }
        } else held = 0;
        _pax_svc->delay_ms(100);
    }
}

static void game_task(void *arg) {
    (void)arg;
    char *argv[] = { "quake2", "+set", "basedir", "/sd/roms/quake2", "+set", "freelook", "1", "+set", "s_initsound", "1", NULL };
    if (setjmp(exit_jmp) == 0) {
        Quake2_Init(11, argv);
        /* Leave headroom for overlapping effects before the PAX master
         * volume stage.  Keep Quake's compact 8-bit cache mode: native-width
         * loading sounds cleaner in isolation, but exhausts the arena as
         * more map effects are encountered. */
        Cvar_Set("s_volume", "0.30");
        Cvar_Set("s_loadas8bit", "1");
        /* About three game frames of mix-ahead at the software renderer's
         * typical 9 FPS. Playback itself runs independently at 22050 Hz. */
        Cvar_Set("s_mixahead", "0.30");
        int old = Quake2_Milliseconds();
#ifdef QUAKE2_TEST_NEWGAME
        int test_frames = 0;
        int test_step = 0;
        int test_next = 0;
        int test_sound_next = 0;
#ifdef QUAKE2_TEST_DEATH
        int death_kill_at = 0, death_killed = 0, death_seen = 0, death_ready = 0, death_since = 0;
        int death_last_menu = -1, death_recovered = 0;
#endif
#endif
        while (!pax_exit_requested) {
            pax_input_poll();
            if (pax_exit_requested) break;
            int now = Quake2_Milliseconds();
            int dt = now - old;
            if (dt < 1) { _pax_svc->delay_ms(1); continue; }
            if (dt > 200) dt = 200;
            old = now;
#ifdef QUAKE2_TEST_NEWGAME
            /* Test-only build: exercise the same three Enter events as the
             * controller Start button, after the demo has rendered. */
            if (test_frames >= 60 && test_step < 6 && now >= test_next) {
                const int down = (test_step & 1) == 0;
                _pax_svc->log_printf("QUAKE2 TEST ENTER %d down=%d\n", test_step / 2 + 1, down);
                Quake2_SendKey(K_ENTER, down);
                test_step++;
                test_next = now + (down ? 150 : 1000);
            }
            test_frames++;
            /* Exercise real cached effects continuously during the audio
             * smoke test, even when the player stands still or dies. */
            if (test_step >= 6 && cls.state == ca_active && !cl.attractloop && now >= test_sound_next) {
                S_StartLocalSound("weapons/blastf1a.wav");
                test_sound_next = now + 500;
            }
#endif
            Quake2_Frame(dt);
#ifdef QUAKE2_TEST_NEWGAME
            pax_video_perf_sample(cls.state == ca_active && !cl.attractloop &&
                                   cls.key_dest == key_game, cl.servercount);
#endif
#ifdef QUAKE2_TEST_DEATH
            if (test_step >= 6 && cls.state == ca_active && !cl.attractloop) {
                int health = cl.frame.playerstate.stats[STAT_HEALTH];
                int menu = cls.key_dest == key_menu;
                int load = menu && m_drawfunc == LoadGame_MenuDraw;
                if (death_last_menu != menu) {
                    _pax_svc->log_printf("QUAKE2 TEST MENU menu=%d load=%d health=%d\n", menu, load, health);
                    death_last_menu = menu;
                }
                if (!death_kill_at) death_kill_at = now + 20000;
                if (!death_killed && now >= death_kill_at) {
                    _pax_svc->log_printf("QUAKE2 TEST DEATH kill requested\n");
                    Cbuf_AddText("kill\n");
                    death_killed = 1;
                }
                if (death_killed && health <= 0) {
                    death_seen = 1;
                    if (!death_since) death_since = now;
                    if (!death_ready && now - death_since >= 2000) {
                        _pax_svc->log_printf("QUAKE2 TEST DEATH ready\n");
                        death_ready = 1;
                    }
                }
                if (death_ready && death_seen && !death_recovered && !menu && health > 0) {
                    _pax_svc->log_printf("QUAKE2 TEST DEATH recovered health=%d\n", health);
                    death_recovered = 1;
                }
            }
#endif
        }
    }
    game_done = 1;
    for (;;) _pax_svc->delay_ms(1000);
}

__attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc) {
    _pax_svc = svc;
    pax_exit_requested = 0;
    game_done = 0;
    svc->log_printf("=== Quake 2 PAX starting ===\n");
    if (svc->display_set_scale) svc->display_set_scale(1.0f, 1.0f);
    svc->task_create(watchdog, "q2_wd", 4096, NULL, 1, &watchdog_handle, 1);
    /* Software rendering and the BSP/sound precache path use more stack than
     * the launcher hand-off path.  This task is allocated in PSRAM by the
     * loader, so give it enough headroom to avoid corrupting its canary once
     * the first map is fully active. */
    if (svc->task_create(game_task, "quake2", 131072, NULL, 5,
                         &game_task_handle, 0) != 0) {
        svc->log_printf("Quake 2: task creation failed\n");
        if (watchdog_handle) svc->task_delete(watchdog_handle);
        return -1;
    }
    /* A quit request is not completion: deleting the game task mid-frame
     * can leave an SD/audio service mutex locked. Wait for it to park before
     * removing tasks and releasing their files, sound buffers and arena. */
    while (!game_done) svc->delay_ms(100);
    if (game_task_handle) svc->task_delete(game_task_handle);
    if (watchdog_handle) svc->task_delete(watchdog_handle);
    svc->log_printf("QUAKE2 STOP joining audio task\n");
    pax_sound_shutdown();
    svc->log_printf("QUAKE2 STOP closing files and freeing memory\n");
    pax_cleanup_fds();
    pax_video_memory_shutdown();
    pax_cleanup_heap();
    Q_Arena_Shutdown();
    svc->display_clear(0);
    svc->display_flush();
    svc->log_printf("=== Quake 2 PAX stopped ===\n");
    return 0;
}
