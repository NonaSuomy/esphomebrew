#ifndef STRATAGUS_PAX_PLATFORM_H
#define STRATAGUS_PAX_PLATFORM_H

#include "psram_app.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const app_services_t *_pax_svc;
extern volatile int pax_exit_requested;

void pax_stratagus_video_init(int width, int height);
void pax_stratagus_video_present(const void *argb8888, int width, int height);
void pax_stratagus_video_present565(const void *rgb565, int width, int height);
void pax_stratagus_video_shutdown(void);
/* Returns -1/1 for navigation, 2 for select, 3 for back, or 100+index
 * when a touch directly selects a visible menu row. */
int pax_stratagus_menu_poll(int page, int selected);
void pax_stratagus_input_poll(void);
int pax_stratagus_touch_button(int held);
void pax_stratagus_pointer_start(void);
void pax_stratagus_pointer_stop(void);
void pax_stratagus_audio_submit(const int16_t *stereo, int frames);
void pax_stratagus_audio_start(void);
// 0: Terran in-game music, 1: one-shot Blizzard soundtrack, 2: title/menu.
void pax_stratagus_audio_music(int track);
int pax_stratagus_audio_music_file(const char *path);
void pax_stratagus_audio_prepare(void);
void pax_stratagus_audio_precache(void);
void pax_stratagus_audio_stop(void);
void pax_stratagus_audio_voice(const char *unit, int acknowledge);
void pax_stratagus_audio_effect_at(const char *name,int x,int y);
void pax_stratagus_audio_view(int x,int y,int width,int height);
void pax_stratagus_audio_unit_event(const char *unit,int voice,int own,int x,int y);
int pax_stratagus_audio_sound_id(const char *name);
void pax_stratagus_audio_play_id(int id,int x,int y,int volume,int flags);
void pax_stratagus_audio_named(const char *name,int x,int y,int volume,int flags);
void pax_stratagus_platform_shutdown(void);
void pax_stratagus_debug(const char *message);
void pax_stratagus_debug_mute(void);

#ifdef __cplusplus
}
#endif

#endif
