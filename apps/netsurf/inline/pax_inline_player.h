/* Experimental asynchronous decoder shared with NetSurf's inline media. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
/* The playback clock already accounts for the audio buffer. At a known end,
 * do not add a second latency delay before enabling replay. */
static inline bool pax_inline_end_due(bool eof, bool have_clock, int64_t clock,
                                      int64_t start, int64_t duration)
{
    return eof && have_clock && duration > 0 && clock >= start &&
           (uint64_t)clock - (uint64_t)start >= (uint64_t)duration;
}
typedef struct pax_inline_player pax_inline_player_t;
enum pax_inline_state { PAX_INLINE_OPENING, PAX_INLINE_READY, PAX_INLINE_PLAYING,
                        PAX_INLINE_PAUSED, PAX_INLINE_ENDED, PAX_INLINE_FAILED };
typedef struct {
    enum pax_inline_state state;
    double position, duration;
    int width, height;
    unsigned frame_seq;
    bool has_audio;
} pax_inline_status_t;
pax_inline_player_t *pax_inline_open(const char *url);
void pax_inline_close(pax_inline_player_t *in);
void pax_inline_play(pax_inline_player_t *in);
void pax_inline_pause(pax_inline_player_t *in);
void pax_inline_seek(pax_inline_player_t *in, double seconds);
void pax_inline_volume(pax_inline_player_t *in, float volume, bool muted);
void pax_inline_loop(pax_inline_player_t *in, bool loop);
void pax_inline_update(pax_inline_player_t *in, pax_inline_status_t *out);
const uint16_t *pax_inline_frame(pax_inline_player_t *in, int *width, int *height);
