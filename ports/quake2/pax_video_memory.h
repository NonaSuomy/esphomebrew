#ifndef PAX_QUAKE2_VIDEO_MEMORY_H
#define PAX_QUAKE2_VIDEO_MEMORY_H

#include "psram_app.h"
#include <stddef.h>
#include <stdint.h>

#ifndef QUAKE2_SRAM_ZBUFFER
/* Opt-in experiment: reuse the loader's SRAM buffer for depth and allocate
 * 75 KiB of PSRAM color. Keep the working baseline as the release default;
 * the clean-boot base1 comparison showed only a small FPS improvement. */
#define QUAKE2_SRAM_ZBUFFER 0
#endif

#define Q2_VIDEO_PIXELS ((size_t)320 * 240)
#define Q2_ZBUFFER_BYTES (Q2_VIDEO_PIXELS * sizeof(uint16_t))

int pax_video_memory_init(const app_services_t *svc, int prefer_sram_depth);
void pax_video_memory_shutdown(void);
uint8_t *pax_video_color_buffer(void);
void *pax_video_zbuffer(size_t bytes);
#ifdef QUAKE2_TEST_NEWGAME
void pax_video_perf_sample(int playable, int servercount);
#endif

#endif
