/*
 * SDL Event/Input shim for PSRAM App — routes through app_services_t.
 * Replaces components/opentyrian_sdl/SDL_event.c
 *
 * Injects SDL_QUIT when the launcher watchdog sets pax_exit_requested.
 */
#include "psram_app.h"
#include "SDL_event.h"
#include <string.h>

extern const app_services_t *_pax_svc;
extern volatile int pax_exit_requested;

typedef struct {
    int pax_id;
    SDL_Scancode scancode;
    SDL_Keycode keycode;
} InputKeyMap;

static const InputKeyMap keymap[] = {
    {PAX_INPUT_UP,     SDL_SCANCODE_UP,     SDLK_UP},
    {PAX_INPUT_DOWN,   SDL_SCANCODE_DOWN,   SDLK_DOWN},
    {PAX_INPUT_LEFT,   SDL_SCANCODE_LEFT,   SDLK_LEFT},
    {PAX_INPUT_RIGHT,  SDL_SCANCODE_RIGHT,  SDLK_RIGHT},
    {PAX_INPUT_A,      SDL_SCANCODE_SPACE,  SDLK_SPACE},
    {PAX_INPUT_B,      SDL_SCANCODE_RETURN, SDLK_RETURN},
    {PAX_INPUT_START,  SDL_SCANCODE_LALT,   SDLK_LALT},
    {PAX_INPUT_SELECT, SDL_SCANCODE_LCTRL,  SDLK_LCTRL},
    {PAX_INPUT_MENU,   SDL_SCANCODE_ESCAPE, SDLK_ESCAPE},
};

#define NUM_KEYS (sizeof(keymap) / sizeof(keymap[0]))

static int last_state[PAX_INPUT_MAX] = {0};
static int input_initialized = 0;

int SDL_PollEvent(SDL_Event *event)
{
    if (!input_initialized) {
        memset(last_state, 0, sizeof(last_state));
        input_initialized = 1;
    }

    /* If exit was requested, inject QUIT event */
    if (pax_exit_requested) {
        event->key.type = SDL_QUIT;
        return 1;
    }

    pax_gamepad_state_t state;
    _pax_svc->input_gamepad_read(&state);

    event->key.keysym.mod = 0;

    for (int i = 0; i < (int)NUM_KEYS; i++) {
        int id = keymap[i].pax_id;
        int cur = state.values[id];
        if (cur != last_state[id]) {
            last_state[id] = cur;
            event->key.keysym.scancode = keymap[i].scancode;
            event->key.keysym.sym = keymap[i].keycode;
            event->key.type = cur ? SDL_KEYDOWN : SDL_KEYUP;
            event->key.state = cur ? SDL_PRESSED : SDL_RELEASED;
            return 1;
        }
    }

    return 0;
}

void inputInit(void)
{
    input_initialized = 1;
}
