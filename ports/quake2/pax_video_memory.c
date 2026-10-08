#include "pax_video_memory.h"
#include "esp_heap_caps.h"

static uint8_t *color;
static void *depth;
static void *owned_color;

/* ESP32-P4 address ranges from ESP-IDF's soc/esp32p4/include/soc/soc.h.
 * The loader's emulator buffer can fall back to PSRAM: never assume SRAM. */
static int is_internal(const void *p, size_t bytes) {
    uintptr_t address = (uintptr_t)p;
    return address >= 0x4ff00000u && address < 0x4ffc0000u &&
           bytes <= 0x4ffc0000u - address;
}

static const char *memory_kind(const void *p, size_t bytes) {
    if (is_internal(p, bytes)) return "SRAM";
    uintptr_t address = (uintptr_t)p;
    if (address >= 0x48000000u && address < 0x4c000000u &&
        bytes <= 0x4c000000u - address) return "PSRAM";
    return "other";
}

void pax_video_memory_shutdown(void) {
    if (owned_color) heap_caps_free(owned_color);
    owned_color = NULL;
    color = NULL;
    depth = NULL;
}

int pax_video_memory_init(const app_services_t *svc, int prefer_sram_depth) {
    pax_video_memory_shutdown();
    void *emu = svc->display_get_emu_buffer ? svc->display_get_emu_buffer() : NULL;
    void *rgb = svc->display_get_framebuffer ? svc->display_get_framebuffer() : NULL;
    if (!emu || !rgb || emu == rgb) return 0;

    /* Baseline: borrow the emulator buffer for indexed color, and the RGB
     * framebuffer for depth. Palette conversion overwrites depth only after
     * rasterization has finished. Neither borrowed buffer may be freed. */
    color = emu;
    depth = rgb;
    if (prefer_sram_depth && is_internal(emu, Q2_ZBUFFER_BYTES)) {
        /* Reassign the existing 150 KiB SRAM reservation to depth instead of
         * competing with firmware/network/audio for a second large block.
         * Color needs just 75 KiB of PSRAM. The normal tracked heap-caps path
         * owns this allocation; on failure keep the allocation-free baseline. */
        owned_color = heap_caps_malloc(Q2_VIDEO_PIXELS, MALLOC_CAP_SPIRAM);
        if (owned_color) {
            color = owned_color;
            depth = emu;
        }
    }
    if (svc->log_printf)
        svc->log_printf("QUAKE2 VIDEO layout=%s color=%p/%s z=%p/%s rgb=%p/%s extra_psram=%u\n",
                        owned_color ? "sram-depth" : "baseline", color,
                        memory_kind(color, Q2_VIDEO_PIXELS), depth,
                        memory_kind(depth, Q2_ZBUFFER_BYTES), rgb,
                        memory_kind(rgb, Q2_ZBUFFER_BYTES),
                        owned_color ? (unsigned)Q2_VIDEO_PIXELS : 0);
    return 1;
}

uint8_t *pax_video_color_buffer(void) { return color; }
void *pax_video_zbuffer(size_t bytes) {
    return bytes == Q2_ZBUFFER_BYTES ? depth : NULL;
}
