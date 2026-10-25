#include "../pax_input.c"
#include "../../../esphome/components/pax_loader/pax_stick_mouse.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>

const app_services_t *_pax_svc;
volatile int pax_exit_requested;
client_static_t cls;
static pax_gamepad_state_t pad;
static int mouse_x, mouse_y, events;
static int last_key, last_down;
static int64_t now_us = 1000;

void Quake2_SendKey(int key, qboolean down) { ++events; last_key = key; last_down = down; }
static void read_pad(pax_gamepad_state_t *out) { *out = pad; }
static int read_mouse(int *x, int *y, int *b) {
    *x = mouse_x; *y = mouse_y; *b = 0;
    mouse_x = mouse_y = 0;
    return 1;
}
static int64_t get_time(void) { return now_us; }

static void stick_test(void) {
    assert(pax_stick_mouse_is_quake2("http://host/QuAkE2-smoke.pax"));
    assert(!pax_stick_mouse_is_quake2("redalert.pax"));
    assert(!pax_stick_mouse_is_quake2("quake.pax"));
    assert(!pax_stick_mouse_is_quake2("q"));
    assert(!pax_stick_mouse_is_quake2(NULL));
    const int rates[] = {9, 30, 120, 1000};
    for (unsigned rate = 0; rate < sizeof rates / sizeof rates[0]; ++rate) {
        pax_stick_mouse_t state = {0};
        int dx, dy, total_x = 0, total_y = 0;
        pax_stick_mouse_step(&state, 100, -100, 1000, &dx, &dy);
        assert(dx == 0 && dy == 0);
        for (int i = 1; i <= rates[rate]; ++i) {
            pax_stick_mouse_step(&state, 100, -100,
                                  1000 + (int64_t)i * 1000000 / rates[rate], &dx, &dy);
            total_x += dx; total_y += dy;
        }
        assert(total_x >= 2699 && total_x <= 2700);
        assert(total_y >= -2700 && total_y <= -2699);
        pax_stick_mouse_step(&state, 19, -20, 1011000, &dx, &dy);
        assert(dx == 0 && dy == 0);
        pax_stick_mouse_step(&state, 60, 60, 1111000, &dx, &dy);
        assert(dx == 135 && dy == 135); /* Half tilt -> half speed. */
        pax_stick_mouse_step(&state, 100, 100, 1001111000, &dx, &dy);
        assert(dx == 540 && dy == 540); /* Long stalls cannot wind up a spin. */
        pax_stick_mouse_step(&state, NAN, NAN, 1001121000, &dx, &dy);
        assert(dx == 0 && dy == 0);
    }
}

int main(void) {
    stick_test();
    static const app_services_t svc = {
        .input_gamepad_read = read_pad, .input_mouse_read = read_mouse, .get_time_us = get_time
    };
    _pax_svc = &svc;
    cls.key_dest = key_game;
    pad.values[PAX_INPUT_L] = 1;
    pax_input_poll(); assert(events == 1 && last_key == K_F1 && last_down == 1);
    pax_input_poll(); assert(events == 1); /* Held shoulder must not retrigger. */
    pad.values[PAX_INPUT_L] = 0;
    pax_input_poll(); assert(events == 2 && last_key == K_F1 && last_down == 0);
    /* B must still jump/use in gameplay, but be Back in every menu. */
    pad.values[PAX_INPUT_B] = 1;
    pax_input_poll(); assert(last_key == K_MOUSE2 && last_down == 1);
    cls.key_dest = key_menu;
    pad.values[PAX_INPUT_B] = 0;
    pax_input_poll(); assert(last_key == K_MOUSE2 && last_down == 0);
    pad.values[PAX_INPUT_B] = 1;
    pax_input_poll(); assert(last_key == K_ESCAPE && last_down == 1);
    cls.key_dest = key_game;
    pad.values[PAX_INPUT_B] = 0;
    pax_input_poll(); assert(last_key == K_ESCAPE && last_down == 0);
    cls.key_dest = key_menu;
    pad.values[PAX_INPUT_A] = 1;
    pax_input_poll(); assert(last_key == K_ENTER && last_down == 1);
    cls.key_dest = key_game;
    pad.values[PAX_INPUT_A] = 0;
    pax_input_poll(); assert(last_key == K_ENTER && last_down == 0);
    pad.values[PAX_INPUT_A] = 1;
    pax_input_poll(); assert(last_key == K_MOUSE1 && last_down == 1);
    pad.values[PAX_INPUT_A] = 0;
    pax_input_poll(); assert(last_key == K_MOUSE1 && last_down == 0);
    for (int menu = 0; menu <= 1; ++menu) {
        cls.key_dest = menu ? key_menu : key_game;
        pad.values[PAX_INPUT_SELECT] = 1;
        pax_input_poll(); assert(last_key == K_ESCAPE && last_down == 1);
        pad.values[PAX_INPUT_SELECT] = 0;
        pax_input_poll(); assert(last_key == K_ESCAPE && last_down == 0);
    }
    cls.key_dest = key_game;
    mouse_x = 12; mouse_y = -7; pax_input_poll();
    now_us += 1000;
    mouse_x = 10; mouse_y = -9; pax_input_poll();
    int x, y;
    QG_GetMouseMove(&x, &y); assert(x == 22 && y == -16);
    QG_GetMouseMove(&x, &y); assert(x == 0 && y == 0);
    cls.key_dest = key_menu;
    mouse_x = 200; pax_input_poll();
    QG_GetMouseMove(&x, &y); assert(x == 0 && y == 0);
    cls.key_dest = key_game;
    now_us += 1000000; mouse_x = 1000; pax_input_poll();
    QG_GetMouseMove(&x, &y); assert(x == 0 && y == 0);
    now_us += 1000; mouse_x = 18; pax_input_poll();
    QG_GetMouseMove(&x, &y); assert(x == 18 && y == 0);
    pad.values[PAX_INPUT_MENU] = 1;
    pax_input_poll(); assert(pax_exit_requested);
    puts("PASS: menu A/Back, Select/Escape, contextual releases, F1, mouse accumulation and analog look");
    return 0;
}
