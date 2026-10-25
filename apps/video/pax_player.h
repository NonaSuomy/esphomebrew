// video: the player (pax_player.c) and the input helper (pax_main.c).
#pragma once

#include "pax_port.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Input ─────────────────────────────────────────────────────────────────
// One poll per UI frame: gamepad edges, keyboard taps, touch taps and drags,
// and the ways to leave the app.

#define PAX_MAX_KEYS 16

typedef struct {
    pax_gamepad_state_t pad, prev;
    int64_t repeat_at[PAX_INPUT_MAX];  // next auto-repeat of a held button
    int keys[PAX_MAX_KEYS];            // keyboard presses this poll (ASCII, Quake codes)
    int nkeys;
    // Touch (canvas coordinates).
    bool touching;
    int x, y;                // where the finger is
    int start_x, start_y;    // where it came down
    bool moved;              // dragged more than a few pixels
    bool tap;                // lifted without moving: a tap at tap_x, tap_y
    int tap_x, tap_y;
    int drag_dy;             // vertical movement since the last poll
    // Leaving: Menu held 3 s, Menu + X (the loader's close control), L3 held.
    int64_t menu_since, l3_since;
    bool quit_app;
} pax_input_t;

void pax_input_reset(pax_input_t *in);
void pax_input_poll(pax_input_t *in);
// True on the poll a button went down, and every `repeat_ms` while it stays
// down after the first 400 ms (0: no repeat).
bool pax_button(pax_input_t *in, int button, int repeat_ms);
bool pax_key(const pax_input_t *in, int key);

// Report the compiled decoder registry before opening media.
void pax_log_codecs(void);

// ── Player ────────────────────────────────────────────────────────────────

enum pax_play_result {
    PLAY_ENDED,     // the end of the file
    PLAY_BACK,      // B, Esc or the Back button: leave the player
    PLAY_QUIT_APP,  // Menu held, the loader's close control: leave the app
    PLAY_FAILED,    // could not open or play it (the error was shown)
};

// Plays `location` (a /sd path, file:// or http(s):// URL) on the canvas
// fb (canvas_w x canvas_h) until the user leaves or it ends.
enum pax_play_result pax_play(const char *location, int canvas_w, int canvas_h, pax_input_t *in);

// Shows a message on the canvas (and the log) for up to `ms`, or until a
// button or tap. Returns false when the user asked to leave the app.
bool pax_message(const char *title, const char *text, int canvas_w, int canvas_h, int ms, pax_input_t *in);

#ifdef __cplusplus
}
#endif
