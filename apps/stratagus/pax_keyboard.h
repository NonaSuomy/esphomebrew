#pragma once
#include <SDL.h>

// The launcher uses Quake key numbers, not SDL2 key codes.
static inline SDL_Keycode pax_sdl_key(int key)
{
	if (key>=135 && key<=146) return SDLK_F1+(key-135);
	switch (key) {
		case 127: return SDLK_BACKSPACE;
		case 132: return SDLK_LALT;
		case 133: return SDLK_LCTRL;
		case 134: return SDLK_LSHIFT;
		case 147: return SDLK_INSERT;
		case 148: return SDLK_DELETE;
		case 149: return SDLK_PAGEDOWN;
		case 150: return SDLK_PAGEUP;
		case 151: return SDLK_HOME;
		case 152: return SDLK_END;
		case 153: return SDLK_UP;
		case 154: return SDLK_RIGHT;
		case 155: return SDLK_DOWN;
		case 156: return SDLK_LEFT;
		default: return key>0 && key<127 ? key : SDLK_UNKNOWN;
	}
}
