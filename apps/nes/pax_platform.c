#define PAX_APP_SIDE 1
#include "psram_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "odroid_input.h"

extern const app_services_t *_pax_svc;
extern void pax_nes_request_exit(void);
char *pax_nes_rom_data;
volatile int nofrendo_quit_flag;
int forceConsoleReset = 1;
volatile int pax_exit_requested;
odroid_battery_state battery;
void *osd_getromdata(void) { return pax_nes_rom_data; }

int pax_nes_load_rom(const char *path)
{
    void *f = _pax_svc->file_open(path, "rb");
    if (!f) {
        _pax_svc->log_printf("NES PAX: ROM open failed: %s\n", path ? path : "(null)");
        return -1;
    }
    _pax_svc->file_seek(f, 0, 2);
    long n = _pax_svc->file_tell(f);
    _pax_svc->file_seek(f, 0, 0);
    if (n <= 0 || n > 8 * 1024 * 1024) {
        _pax_svc->log_printf("NES PAX: ROM size invalid: %ld bytes (%s)\n", n, path);
        _pax_svc->file_close(f);
        return -1;
    }
    pax_nes_rom_data = _pax_svc->mem_caps_alloc((size_t)n, PAX_MEM_CAP_SPIRAM);
    if (!pax_nes_rom_data || _pax_svc->file_read(pax_nes_rom_data, 1, (size_t)n, f) != (size_t)n) {
        if (pax_nes_rom_data) _pax_svc->mem_free(pax_nes_rom_data);
        _pax_svc->log_printf("NES PAX: ROM read failed: %s\n", path);
        pax_nes_rom_data = NULL; _pax_svc->file_close(f); return -1;
    }
    _pax_svc->file_close(f);
    return 0;
}
void pax_nes_cleanup(void) { if (pax_nes_rom_data) _pax_svc->mem_free(pax_nes_rom_data); pax_nes_rom_data = NULL; }
void app_return_to_launcher(void) { pax_exit_requested = 1; pax_nes_request_exit(); }
