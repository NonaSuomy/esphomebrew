#include "psram_app.h"
#include "q_arena.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

extern const app_services_t *_pax_svc;

/* Small PSRAM segments with reusable, coalescing blocks. Empty segments go
 * back to the host heap so large BSP reads can fit after a map transition. */
#define ARENA_SEGMENT_BYTES (256 * 1024)
#define ARENA_MAX_SEGMENTS 64
#define ARENA_ALIGNMENT 64
#define ARENA_MAGIC 0x51324142u

typedef struct arena_block_s {
    struct arena_block_s *prev, *next;
    int capacity, requested;
    uint32_t magic;
} arena_block_t;

typedef struct {
    void *allocation;
    arena_block_t *first;
    int bytes;
} arena_segment_t;

#define ARENA_HEADER ((int)((sizeof(arena_block_t) + 63) & ~(size_t)63))

static arena_segment_t segments[ARENA_MAX_SEGMENTS];
static int budget, reserved, used;

static int segment_for(const void *p) {
    const uintptr_t address = (uintptr_t)p;
    for (int i = 0; i < ARENA_MAX_SEGMENTS; i++) {
        const uintptr_t start = (uintptr_t)segments[i].first;
        if (segments[i].allocation && address >= start &&
            address < start + (uintptr_t)segments[i].bytes)
            return i;
    }
    return -1;
}

void Q_Arena_Shutdown(void) {
    for (int i = 0; i < ARENA_MAX_SEGMENTS; i++) {
        if (segments[i].allocation && _pax_svc)
            _pax_svc->mem_free(segments[i].allocation);
    }
    memset(segments, 0, sizeof(segments));
    budget = reserved = used = 0;
}

int Q_Arena_Init(int size) {
    Q_Arena_Shutdown();
    budget = size > 0 ? size : 0;
    return budget;
}

static arena_block_t *new_segment(int payload) {
    int slot;
    for (slot = 0; slot < ARENA_MAX_SEGMENTS; slot++)
        if (!segments[slot].allocation) break;
    if (slot == ARENA_MAX_SEGMENTS) return NULL;

    const int minimum = payload + ARENA_HEADER;
    int bytes = minimum > ARENA_SEGMENT_BYTES ? minimum : ARENA_SEGMENT_BYTES;
    if (bytes > budget - reserved) bytes = budget - reserved;
    bytes &= ~(ARENA_ALIGNMENT - 1);
    if (bytes < minimum) return NULL;

    void *allocation = NULL;
    /* Near exhaustion a small zone allocation must remain possible even
     * when the host heap cannot provide a full segment. */
    for (;;) {
        allocation = _pax_svc->mem_caps_alloc((size_t)bytes + 63,
                                               PAX_MEM_CAP_SPIRAM);
        if (allocation || bytes == minimum) break;
        bytes = bytes / 2 & ~(ARENA_ALIGNMENT - 1);
        if (bytes < minimum) bytes = minimum;
    }
    if (!allocation) return NULL;

    arena_block_t *first = (arena_block_t *)
        (((uintptr_t)allocation + 63) & ~(uintptr_t)63);
    first->prev = first->next = NULL;
    first->capacity = bytes - ARENA_HEADER;
    first->requested = 0;
    first->magic = ARENA_MAGIC;
    segments[slot].allocation = allocation;
    segments[slot].first = first;
    segments[slot].bytes = bytes;
    reserved += bytes;
    return first;
}

void *Q_Arena_Alloc(int size) {
    if (!_pax_svc || size <= 0 || size > INT_MAX - ARENA_HEADER - 63)
        return NULL;
    const int payload = (size + 63) & ~63;
    arena_block_t *best = NULL;
    for (int i = 0; i < ARENA_MAX_SEGMENTS; i++) {
        for (arena_block_t *b = segments[i].first; b; b = b->next) {
            if (!b->requested && b->capacity >= payload &&
                (!best || b->capacity < best->capacity)) best = b;
        }
    }
    if (!best) best = new_segment(payload);
    if (!best) return NULL;

    if (best->capacity - payload >= ARENA_HEADER + ARENA_ALIGNMENT) {
        arena_block_t *tail = (arena_block_t *)
            ((unsigned char *)best + ARENA_HEADER + payload);
        tail->prev = best;
        tail->next = best->next;
        if (tail->next) tail->next->prev = tail;
        tail->capacity = best->capacity - payload - ARENA_HEADER;
        tail->requested = 0;
        tail->magic = ARENA_MAGIC;
        best->next = tail;
        best->capacity = payload;
    }
    best->requested = size;
    used += best->capacity + ARENA_HEADER;
    void *p = (unsigned char *)best + ARENA_HEADER;
    memset(p, 0, (size_t)size);
    return p;
}

void *Q_Arena_Carve(int size) { return Q_Arena_Alloc(size); }

int Q_Arena_AllocationSize(void *p) {
    const int i = segment_for(p);
    if (i < 0 || (uintptr_t)p < (uintptr_t)segments[i].first + ARENA_HEADER)
        return 0;
    arena_block_t *b = (arena_block_t *)((unsigned char *)p - ARENA_HEADER);
    return b->magic == ARENA_MAGIC ? b->requested : 0;
}

void Q_Arena_Free(void *p) {
    if (!p) return;
    const int i = segment_for(p);
    if (i < 0) {
        if (Hunk_Contains(p)) Hunk_Free(p);
        return;
    }
    if ((uintptr_t)p < (uintptr_t)segments[i].first + ARENA_HEADER) return;
    arena_block_t *b = (arena_block_t *)((unsigned char *)p - ARENA_HEADER);
    if (b->magic != ARENA_MAGIC || !b->requested) return;
    used -= b->capacity + ARENA_HEADER;
    b->requested = 0;
    if (b->next && !b->next->requested) {
        arena_block_t *next = b->next;
        b->capacity += ARENA_HEADER + next->capacity;
        b->next = next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && !b->prev->requested) {
        arena_block_t *prev = b->prev;
        prev->capacity += ARENA_HEADER + b->capacity;
        prev->next = b->next;
        if (prev->next) prev->next->prev = prev;
        b = prev;
    }
    if (!b->prev && !b->next) {
        _pax_svc->mem_free(segments[i].allocation);
        reserved -= segments[i].bytes;
        memset(&segments[i], 0, sizeof(segments[i]));
    }
}

int Q_Arena_Contains(void *p) { return p && segment_for(p) >= 0; }
int Q_Arena_Reserved(void) { return reserved; }
int Q_Arena_Used(void) { return used; }
int Q_Arena_FreeSize(void) { return budget - used; }
int Q_Arena_Largest(void) {
    int largest = 0;
    for (int i = 0; i < ARENA_MAX_SEGMENTS; i++)
        for (arena_block_t *b = segments[i].first; b; b = b->next)
            if (!b->requested && b->capacity > largest) largest = b->capacity;
    return largest;
}
int Q_Arena_Count(void) {
    int count = 0;
    for (int i = 0; i < ARENA_MAX_SEGMENTS; i++)
        for (arena_block_t *b = segments[i].first; b; b = b->next) count++;
    return count;
}
