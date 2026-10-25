#pragma once
#include <SDL.h>

enum PaxRenderPhase { PAX_DRAW, PAX_CLEAR, PAX_TERRAIN, PAX_SPRITES,
	PAX_FOG, PAX_PRESENT, PAX_LOGIC, PAX_INPUT_WAIT, PAX_UNIT_LOGIC,
	PAX_TRIGGER_LOGIC, PAX_PHASES };
extern "C" void pax_stratagus_profile_add(unsigned phase, unsigned microseconds);
extern "C" void pax_stratagus_profile_frame(unsigned long cycle);

class PaxRenderScope {
	unsigned phase;
	Uint64 start;
public:
	explicit PaxRenderScope(unsigned id) : phase(id), start(SDL_GetPerformanceCounter()) {}
	~PaxRenderScope() { pax_stratagus_profile_add(phase,SDL_GetPerformanceCounter()-start); }
};
