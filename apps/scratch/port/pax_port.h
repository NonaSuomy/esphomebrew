#pragma once
#include "psram_app.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const app_services_t *pax_svc;
void pax_quit(int code) __attribute__((noreturn));
void pax_free_all_memory(void);
void pax_close_all_files(void);
long long pax_time_us(void);
#ifdef __cplusplus
}
#include <image.hpp>
extern uint16_t *pax_pixels;
extern int PAX_W, PAX_H;
extern int pax_editor_width, pax_editor_height;
void pax_choose_canvas();
void pax_update_project_clock();
extern bool pax_editor_playing;
bool pax_resize_surface(int width, int height);
void pax_set_clip(int x, int y, int w, int h);
void pax_blend(int x, int y, int r, int g, int b, int a);
void pax_draw_image(const ImageData &img, const ImageRenderParams &p);
void pax_audio_pump();
void pax_draw_text(const std::string &txt, int x, int y, int color, float scale,
                   bool centered);
#endif
