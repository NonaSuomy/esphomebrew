#pragma once

#include "psram_app.h"

/* Run the NES-specific pause/options menu. Returns nonzero when the app
 * should leave the emulator and return to the launcher. */
int pax_nes_menu_run(const app_services_t *svc, const uint16_t *game_frame,
                      uint16_t width, uint16_t height);
int pax_nes_menu_swipe_down_pressed(const app_services_t *svc);
/* Register pages inside the loader overlay; zero means use the legacy modal. */
int pax_nes_shared_menu_attach(const app_services_t *svc, const uint16_t *game_frame,
                                uint16_t width, uint16_t height);
void pax_nes_shared_menu_detach(void);

/* Per-ROM display options used by the NES video bridge. */
void pax_nes_options_load(const app_services_t *svc, uint32_t rom_hash);
void pax_nes_options_load_for_current_rom(const app_services_t *svc);
void pax_nes_options_apply_palette(void);
int pax_nes_options_crop_sides(void);
float pax_nes_options_scale(void);
