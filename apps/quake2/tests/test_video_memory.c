/* cc -std=c99 -DPAX_APP_SIDE=1 -I.cache/loader_sdk -Iports/quake2
 * ports/quake2/tests/test_video_memory.c ports/quake2/pax_video_memory.c
 * -o /tmp/quake2-video-memory-test */
#include "pax_video_memory.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void *emu, *rgb;
static void *owned;
static int allocations, frees, fail_alloc;
static uint16_t *get_emu(void) { return emu; }
static uint16_t *get_rgb(void) { return rgb; }
void *heap_caps_malloc(size_t size, uint32_t caps) {
    assert(size == Q2_VIDEO_PIXELS && caps == MALLOC_CAP_SPIRAM);
    ++allocations;
    if (fail_alloc) return NULL;
    assert(!owned);
    return owned = malloc(size);
}
void heap_caps_free(void *p) {
    assert(p && p == owned && p != emu && p != rgb);
    ++frees;
    free(p);
    owned = NULL;
}
static const app_services_t services = {
    .display_get_emu_buffer = get_emu, .display_get_framebuffer = get_rgb
};

int main(void) {
    emu = (void *)0x4ff10000u;
    rgb = (void *)0x48040000u;
    assert(pax_video_memory_init(&services, 0));
    assert(pax_video_color_buffer() == emu);
    assert(pax_video_zbuffer(Q2_ZBUFFER_BYTES) == rgb);
    assert(!pax_video_zbuffer(Q2_ZBUFFER_BYTES - 1));
    assert(!allocations);
    pax_video_memory_shutdown();
    assert(!frees);
    for (int i = 0; i < 20; ++i) {
        assert(pax_video_memory_init(&services, 1));
        assert(pax_video_color_buffer() == owned && owned != emu);
        assert(pax_video_zbuffer(Q2_ZBUFFER_BYTES) == emu);
        /* Mode reset frees only our old color allocation. */
        assert(pax_video_memory_init(&services, 1));
        pax_video_memory_shutdown();
        pax_video_memory_shutdown();
        assert(!owned && !pax_video_zbuffer(Q2_ZBUFFER_BYTES));
    }
    assert(allocations == frees && frees == 40);
    fail_alloc = 1;
    assert(pax_video_memory_init(&services, 1));
    assert(pax_video_color_buffer() == emu);
    assert(pax_video_zbuffer(Q2_ZBUFFER_BYTES) == rgb);
    pax_video_memory_shutdown();
    fail_alloc = 0;
    int before = allocations;
    emu = (void *)0x48010000u; /* Loader itself fell back to PSRAM. */
    assert(pax_video_memory_init(&services, 1));
    assert(pax_video_color_buffer() == emu);
    assert(pax_video_zbuffer(Q2_ZBUFFER_BYTES) == rgb);
    assert(allocations == before);
    emu = (void *)0x4ffb0000u; /* Not enough SRAM range for the full depth buffer. */
    assert(pax_video_memory_init(&services, 1));
    assert(allocations == before);
    emu = rgb;
    assert(!pax_video_memory_init(&services, 1));
    emu = NULL;
    assert(!pax_video_memory_init(&services, 1));
    pax_video_memory_shutdown();
    puts("PASS: video buffer ownership, SRAM-depth reuse, mode resets and safe PSRAM/OOM fallback");
}
