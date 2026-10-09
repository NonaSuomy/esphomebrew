/* Host regression: cc -std=c99 -DPAX_APP_SIDE=1 -I.cache/loader_sdk -Iports/quake2
 * ports/quake2/tests/test_arena.c ports/quake2/pax_arena.c -o /tmp/q2-arena-test */
#include "psram_app.h"
#include "q_arena.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { void *p; size_t bytes; } host_blocks[128];
static size_t host_used, host_limit = 4 * 1024 * 1024;
static size_t largest_allowed = 2 * 1024 * 1024;
static void *host_alloc(size_t bytes, uint32_t caps) {
    assert(caps == PAX_MEM_CAP_SPIRAM);
    if (bytes > largest_allowed || bytes > host_limit - host_used) return NULL;
    for (int i = 0; i < 128; i++) {
        if (host_blocks[i].p) continue;
        void *p = malloc(bytes);
        assert(p);
        host_blocks[i].p = p;
        host_blocks[i].bytes = bytes;
        host_used += bytes;
        return p;
    }
    return NULL;
}
static void host_free(void *p) {
    for (int i = 0; i < 128; i++) {
        if (host_blocks[i].p != p) continue;
        host_used -= host_blocks[i].bytes;
        host_blocks[i].p = NULL;
        free(p);
        return;
    }
    assert(!"invalid or duplicate host free");
}
static const app_services_t services = {
    .mem_caps_alloc = host_alloc, .mem_free = host_free
};
const app_services_t *_pax_svc = &services;
int Hunk_Contains(void *p) { (void)p; return 0; }
void Hunk_Free(void *p) { (void)p; assert(0); }

int main(void) {
    void *p[900];
    for (int map = 0; map < 20; map++) {
        Q_Arena_Init(4 * 1024 * 1024);
        for (int i = 0; i < 900; i++) {
            const int size = 22 + i % 300;
            p[i] = Q_Arena_Alloc(size);
            assert(p[i] && ((uintptr_t)p[i] & 63) == 0);
            assert(Q_Arena_AllocationSize(p[i]) == size);
            memset(p[i], i & 255, size);
        }
        for (int i = 0; i < 900; i += 2) Q_Arena_Free(p[i]);
        for (int i = 1; i < 900; i += 2) {
            const unsigned char *bytes = p[i];
            for (int j = 0; j < 22 + i % 300; j++) assert(bytes[j] == (i & 255));
            Q_Arena_Free(p[i]);
        }
        assert(host_used == 0 && Q_Arena_Used() == 0 && Q_Arena_Reserved() == 0);
        void *entities = Q_Arena_Alloc(913424);
        void *bsp = Q_Arena_Alloc(1991536);
        assert(entities && bsp);
        Q_Arena_Free(bsp);
        Q_Arena_Free(entities);
        assert(host_used == 0);
    }
    /* Force smaller reservations, then free adjacent blocks and reuse them. */
    largest_allowed = 70000;
    Q_Arena_Init(4 * 1024 * 1024);
    for (int i = 0; i < 100; i++) {
        p[i] = Q_Arena_Alloc(1000);
        assert(p[i]);
    }
    for (int i = 0; i < 100; i++) Q_Arena_Free(p[i]);
    assert(host_used == 0);
    p[0] = Q_Arena_Alloc(4000);
    p[1] = Q_Arena_Alloc(4000);
    p[2] = Q_Arena_Alloc(4000);
    Q_Arena_Free(p[0]);
    Q_Arena_Free(p[1]);
    const size_t before = host_used;
    p[0] = Q_Arena_Alloc(7000);
    assert(p[0] && host_used == before);
    Q_Arena_Free(p[0]);
    Q_Arena_Free(p[2]);
    assert(host_used == 0);
    assert(!Q_Arena_Alloc(-1));
    p[0] = Q_Arena_Alloc(500);
    assert(p[0]);
    Q_Arena_Shutdown();
    Q_Arena_Shutdown();
    assert(host_used == 0);
    puts("Quake2 arena: repeated map frees, 900 live blocks, coalescing and shutdown passed");
    return 0;
}
