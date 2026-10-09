#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void pax_stratagus_debug(const char *message);
void pax_stratagus_debug_mute(void);
void pax_stratagus_video_present(const void *argb8888, int width, int height);
void pax_stratagus_video_present565(const void *rgb565, int width, int height);
int pax_stratagus_menu_poll(int page, int selected);
void pax_stratagus_input_poll(void);
void pax_stratagus_audio_stop(void);
void pax_stratagus_audio_voice(const char *unit, int acknowledge);
void pax_stratagus_audio_effect(const char *name);
void pax_stratagus_audio_effect_at(const char *name,int x,int y);
void pax_stratagus_audio_view(int x,int y,int width,int height);
void pax_stratagus_audio_unit_event(const char *unit,int voice,int own,int x,int y);
int pax_stratagus_audio_sound_id(const char *name);
void pax_stratagus_audio_play_id(int id,int x,int y,int volume,int flags);
void pax_stratagus_audio_named(const char *name,int x,int y,int volume,int flags);
extern volatile int pax_exit_requested;

#ifdef __cplusplus
}
#endif
