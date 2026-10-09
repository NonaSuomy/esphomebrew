/* NES/nofrendo PSRAM application entry point. */
#define PAX_APP_SIDE 1
#include "runtime.h"
#include "psram_app.h"
#include "pax_nes_options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const app_services_t *_pax_svc;
volatile int pax_nes_exit;

extern int nofrendo_main(int argc, char **argv);
extern volatile int nofrendo_quit_flag;
extern char *pax_nes_rom_data;
extern int pax_nes_load_rom(const char *path);
extern void pax_nes_cleanup(void);
extern volatile int pax_exit_requested;

static int nes_selection_requested(const app_services_t *svc)
{
    if (svc->settings_rom_path_get) {
        char *configured = svc->settings_rom_path_get();
        if (configured && configured[0])
            return 1;
    }
    /* The sidecar is the durable selection used by the launcher.  Treat a
       non-empty sidecar as an explicit request even if ZIP extraction fails;
       falling through to River City Ransom hides the real error and makes a
       picker selection appear to do nothing. */
    void *f = svc->file_open("/sd/roms/pax/nes.rom", "rb");
    if (!f)
        return 0;
    char selected[2] = {0, 0};
    const size_t n = svc->file_read(selected, 1, 1, f);
    svc->file_close(f);
    return n == 1 && selected[0] != 0 && selected[0] != '\n' && selected[0] != '\r';
}

void pax_nes_request_exit(void)
{
    pax_nes_exit = 1;
    pax_exit_requested = 1;
    nofrendo_quit_flag = 1;
}

static void nes_watchdog(void *arg)
{
    (void)arg;
    int held = 0;
    for (;;) {
        if (_pax_svc->input_l3_read && _pax_svc->input_l3_read()) {
            if (++held >= 3) {
                pax_nes_exit = 1;
                pax_exit_requested = 1;
                /* The launcher deletes this task after nofrendo_main()
                   unwinds.  Do not return from a PAX-created task: the
                   shared PAX ABI keeps task ownership with the app until
                   task_delete() is called explicitly. */
                for (;;) {
                    _pax_svc->delay_ms(100);
                }
            }
        } else {
            held = 0;
        }
        _pax_svc->delay_ms(100);
    }
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    _pax_svc = svc;
    pax_nes_exit = 0;
    pax_exit_requested = 0;
    nofrendo_quit_flag = 0;
    if (!svc || svc->abi_version != PAX_ABI_VERSION)
        return -1;

    char selected_path_buf[512];
    const char *selected_path = NULL;
    /* The launcher stores the selected PAX path in NVS.  Read the matching
       /sd/roms/pax/nes.rom sidecar first so the PAX launch menu can choose
       any NES image without confusing the .pax path for a ROM. */
    const int selection_requested = nes_selection_requested(svc);
    if (emu_rom_path("nes", ".nes|", selected_path_buf,
                     sizeof(selected_path_buf)) == 0) {
        selected_path = selected_path_buf;
        svc->log_printf("NES PAX: selected ROM resolved to %s\n", selected_path);
    } else if (selection_requested) {
        svc->log_printf("NES PAX: selected ROM could not be resolved; refusing built-in fallback\n");
        return -1;
    } else {
        selected_path = "/sd/roms/nes/River City Ransom (USA).nes";
        svc->log_printf("NES PAX: no ROM selected; using default %s\n", selected_path);
    }
    /* The loader owns this static settings buffer. Copy it before using it
       and only free the app-owned copy on exit. */
    size_t path_len = strlen(selected_path) + 1;
    char *rom_path = svc->mem_alloc(path_len);
    if (!rom_path) return -1;
    memcpy(rom_path, selected_path, path_len);
    svc->log_printf("=== NES PAX: %s ===\n", rom_path);
    svc->audio_init(32000);

    void *watchdog = NULL;
    svc->task_create(nes_watchdog, "nes_wd", 4096, NULL, 2, &watchdog, 1);

    int result = -1;
    int core_owns_path = 0;
    if (pax_nes_load_rom(rom_path) == 0) {
        pax_nes_options_load_for_current_rom(svc);
        char *argv[] = { rom_path, NULL };
        /* nofrendo stores argv[0] and frees it during core shutdown. */
        core_owns_path = 1;
        result = nofrendo_main(1, argv);
    }

    if (watchdog) svc->task_delete(watchdog);
    pax_nes_cleanup();
    if (!core_owns_path) svc->mem_free(rom_path);
    svc->display_clear(0);
    svc->display_flush();
    svc->log_printf("=== NES PAX exited (%d) ===\n", result);
    return result;
}
