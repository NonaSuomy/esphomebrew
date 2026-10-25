#include "psram_app.h"
#include "client/keys.h"
#include "client/client.h"
#include "quake2.h"
#include <string.h>

extern const app_services_t *_pax_svc;
extern volatile int pax_exit_requested;
static int mdx, mdy, buttons;
static int64_t last_poll_us;
static int old_buttons;
static int prev[PAX_INPUT_MAX];
static int pressed_key[PAX_INPUT_MAX];

static int pad_key(int i) {
    static const int map[PAX_INPUT_MAX] = { K_UPARROW, K_RIGHTARROW, K_DOWNARROW, K_LEFTARROW, K_ESCAPE, K_ENTER, K_MOUSE1, K_MOUSE2, 'c', K_SHIFT, K_F1, 'd', K_ESCAPE, 0 };
    if (cls.key_dest == key_menu) {
        if (i == PAX_INPUT_A) return K_ENTER;
        if (i == PAX_INPUT_B) return K_ESCAPE;
    }
    return map[i];
}
void pax_input_poll(void) {
    pax_gamepad_state_t p; _pax_svc->input_gamepad_read(&p);
    for (int i=0;i<PAX_INPUT_MAX;i++) {
        int down = p.values[i] != 0;
        if (down != prev[i]) {
            /* A menu can open/close while a button is held. Always release
             * the key chosen on its press, not a different contextual key. */
            int key = down ? pad_key(i) : pressed_key[i];
            if (down) pressed_key[i] = key;
            if (key) Quake2_SendKey(key, down);
            if (i == PAX_INPUT_L && _pax_svc->log_printf)
                _pax_svc->log_printf("QUAKE2 INPUT L1 -> F1 down=%d\n", down);
            if (key == K_ESCAPE && i != PAX_INPUT_MENU && _pax_svc->log_printf)
                _pax_svc->log_printf("QUAKE2 INPUT %s -> ESCAPE down=%d\n",
                                      i == PAX_INPUT_B ? "B" : "SELECT", down);
            if (!down) pressed_key[i] = 0;
        }
        prev[i] = down;
    }
    if (_pax_svc->input_mouse_read) {
        int x = 0, y = 0;
        int64_t now = _pax_svc->get_time_us();
        _pax_svc->input_mouse_read(&x, &y, &buttons);
        /* Polling may happen several times before IN_Move consumes input.
         * Keep every mouse/stick delta, but discard movement accumulated
         * while a menu or a long map/asset load prevented gameplay. */
        if (cls.key_dest != key_game || (last_poll_us && now - last_poll_us > 250000)) {
            mdx = mdy = 0;
        } else {
            mdx += x;
            mdy += y;
            if (mdx > 4096) mdx = 4096;
            if (mdx < -4096) mdx = -4096;
            if (mdy > 4096) mdy = 4096;
            if (mdy < -4096) mdy = -4096;
        }
        last_poll_us = now;
    }
    pax_keyboard_event_t ev;
    while (_pax_svc->input_keyboard_read && _pax_svc->input_keyboard_read(&ev)) Quake2_SendKey(ev.key, ev.down);
    if (p.values[PAX_INPUT_MENU] && !old_buttons) pax_exit_requested = 1;
    old_buttons = p.values[PAX_INPUT_MENU];
}
int QG_GetKey(int *down, int *key) { (void)down; (void)key; return 0; }
void QG_GetMouseMove(int *x, int *y) { *x=mdx; *y=mdy; mdx=mdy=0; }
