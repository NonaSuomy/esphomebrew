#pragma once
#include <SDL.h>
// 1=Wasteland (Mission 2), 3=Boot Camp (Mission 1), 2=load, 0=exit.
int pax_stratagus_menu_run(SDL_Surface *screen,bool showIntro=true);
int pax_stratagus_game_menu_run(SDL_Surface *screen,int page=0);
const char *pax_stratagus_menu_save_name();
void pax_stratagus_menu_set_tutorial(bool tutorial);
bool pax_stratagus_portrait_draw(SDL_Surface *screen,const char *ident,int x,int y);
void pax_stratagus_portrait_close();
void pax_stratagus_open_game_menu();
unsigned pax_stratagus_modal_generation();
// SD-backed, timed RGB565 video pack, shared with Stargus's intro decoder.
bool pax_stratagus_movie_play(SDL_Surface *screen,const char *name);
