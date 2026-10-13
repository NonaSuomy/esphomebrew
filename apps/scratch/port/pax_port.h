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
constexpr int PAX_W = 480, PAX_H = 360;
void pax_blend(int x, int y, int r, int g, int b, int a);
void pax_draw_image(const ImageData &img, const ImageRenderParams &p);
void pax_audio_pump();
void pax_draw_text(const std::string &txt, int x, int y, int color, float scale, bool centered);
#endif
