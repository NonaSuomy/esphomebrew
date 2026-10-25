#pragma once
#include <SDL.h>
struct PaxResultRow {
 char name[64]; int units,buildings,minerals,gas,kills,razings;
};
struct PaxResults {
 int outcome,seconds,count; PaxResultRow rows[8];
};
extern PaxResults pax_results;
void pax_stratagus_results_capture();
void pax_stratagus_results_show(SDL_Surface *screen);
