#include "pax_port.h"
#include "pax_icons.h"
#include <lunasvg.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <image_headless.hpp>
#include <input.hpp>
#include <render.hpp>
#include <speech_manager.hpp>
#include <stb_easy_font.h>
#include <text_headless.hpp>
#include <vector>
#include <memory>
#include <window.hpp>
uint16_t *pax_pixels;
int PAX_W = 480, PAX_H = 360;
static int clipX = 0, clipY = 0, clipW = 480, clipH = 360;
void pax_set_clip(int x, int y, int w, int h) {
    clipX = x;
    clipY = y;
    clipW = w;
    clipH = h;
}
WindowSE *globalWindow = nullptr;
struct PenRowDeleter {
    void operator()(uint8_t *p) const { if (p) pax_svc->mem_free(p); }
};
static std::vector<std::unique_ptr<uint8_t, PenRowDeleter>> pen;
static bool penAllocationFailed = false;
static bool penTarget = false;
class PaxSpeech : public SpeechManager {
    SpeechRenderConfig getSpeechRenderConfig() override {
        SpeechRenderConfig c;
        c.style = SpeechBubbleStyle::Fast;
        return c;
    }
};
static PaxSpeech *speech;
static uint16_t rgb565(int r, int g, int b) {
    return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
}
float pax_ui_zoom = 1;
static int zoomX, zoomY;
void pax_set_ui_zoom(float scale, int x, int y) {
  pax_ui_zoom = scale;
  zoomX = x;
  zoomY = y;
}
static void blendPixel(int x, int y, int r, int g, int b, int a) {
  if (x < 0 || y < 0 || x >= PAX_W || y >= PAX_H || x < clipX || y < clipY ||
      x >= clipX + clipW || y >= clipY + clipH || a <= 0)
    return;
  a = std::min(255, a);
  if (penTarget) {
    if (penAllocationFailed) return;
    auto &row = pen[y];
    if (!row) {
      auto *pixels = static_cast<uint8_t *>(pax_svc->mem_caps_alloc(PAX_W * 4, PAX_MEM_CAP_SPIRAM));
      if (!pixels) {
        penAllocationFailed = true;
        pax_svc->log_printf("Scratch: Warning: pen row allocation failed\n");
        return;
      }
      std::memset(pixels, 0, PAX_W * 4);
      row.reset(pixels);
    }
    auto *d = row.get() + x * 4;
    int da = d[3], oa = a + da * (255 - a) / 255;
    if (oa) {
      d[0] = (r * a + d[0] * da * (255 - a) / 255) / oa;
      d[1] = (g * a + d[1] * da * (255 - a) / 255) / oa;
      d[2] = (b * a + d[2] * da * (255 - a) / 255) / oa;
      d[3] = oa;
    }
    return;
  }
  if (!pax_pixels)
    return;
  uint16_t &d = pax_pixels[y * PAX_W + x];
  if (a < 255) {
    int dr = ((d >> 11) & 31) * 255 / 31, dg = ((d >> 5) & 63) * 255 / 63,
        db = (d & 31) * 255 / 31;
    r = (r * a + dr * (255 - a)) / 255;
    g = (g * a + dg * (255 - a)) / 255;
    b = (b * a + db * (255 - a)) / 255;
  }
  d = rgb565(r, g, b);
}
void pax_draw_icon(PaxIcon icon, int x, int y, int size) {
    struct Raster {
        PaxIcon icon;
        int size, w, h;
        std::vector<uint8_t> pixels;
    };
    static std::vector<Raster> cache;
    auto it = std::find_if(cache.begin(), cache.end(), [&](const Raster &r) {
        return r.icon == icon && r.size == size;
    });
    if (it == cache.end()) {
        auto doc = lunasvg::Document::loadFromData(pax_icon_svg[static_cast<int>(icon)]);
        if (!doc)
            return;
        auto bm = doc->renderToBitmap(size, size);
        bm.convertToRGBA();
        cache.push_back({icon, size, static_cast<int>(bm.width()), static_cast<int>(bm.height()),
                         {bm.data(), bm.data() + bm.width() * bm.height() * 4}});
        it = std::prev(cache.end());
    }
    for (int yy = 0; yy < it->h; yy++)
        for (int xx = 0; xx < it->w; xx++) {
            const auto *c = &it->pixels[(yy * it->w + xx) * 4];
            pax_blend(x + xx, y + yy, c[0], c[1], c[2], c[3]);
        }
}
void pax_blend(int x, int y, int r, int g, int b, int a) {
  if (pax_ui_zoom == 1) {
    blendPixel(x, y, r, g, b, a);
    return;
  }
  int x0 = zoomX + (int)std::floor((x - zoomX) * pax_ui_zoom),
      y0 = zoomY + (int)std::floor((y - zoomY) * pax_ui_zoom);
  int x1 = zoomX + (int)std::floor((x + 1 - zoomX) * pax_ui_zoom),
      y1 = zoomY + (int)std::floor((y + 1 - zoomY) * pax_ui_zoom);
  for (int yy = y0; yy < y1; yy++)
    for (int xx = x0; xx < x1; xx++)
      blendPixel(xx, yy, r, g, b, a);
}
void pax_draw_image(const ImageData &img, const ImageRenderParams &p) {
    pax_audio_pump();
    if (!img.hasPixels() || img.width <= 0 || img.height <= 0 || !std::isfinite(p.scale) || p.scale <= 0)
        return;
    ImageSubrect sub = p.subrect ? *p.subrect : ImageSubrect{0, 0, img.width, img.height};
    if (sub.w <= 0 || sub.h <= 0)
        return;
    float sc = p.scale / std::max(.001f, img.scale), w = sub.w * sc, h = sub.h * sc;
    float cx = p.x + (p.centered ? 0 : w / 2), cy = p.y + (p.centered ? 0 : h / 2);
    float cs = cosf(p.rotation), sn = sinf(p.rotation);
    float bw = fabsf(cs * w) + fabsf(sn * h), bh = fabsf(sn * w) + fabsf(cs * h);
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(bw) || !std::isfinite(bh))
        return;
    int x0 = (int)std::clamp(floorf(cx - bw / 2), 0.f, (float)PAX_W),
        x1 = (int)std::clamp(ceilf(cx + bw / 2), 0.f, (float)PAX_W);
    int y0 = (int)std::clamp(floorf(cy - bh / 2), 0.f, (float)PAX_H),
        y1 = (int)std::clamp(ceilf(cy + bh / 2), 0.f, (float)PAX_H);
    // Most stage backdrops and UI sprites are unrotated. Map X once and
    // decode each source row once, instead of calling rowData per pixel.
    if (p.rotation == 0 && p.opacity == 1 && p.brightness == 0 &&
        pax_ui_zoom == 1 && !penTarget && pax_pixels) {
        x0=std::max(x0,clipX);x1=std::min(x1,clipX+clipW);
        y0=std::max(y0,clipY);y1=std::min(y1,clipY+clipH);
        if (x1<=x0 || y1<=y0) return;
        static std::vector<int> columns;
        columns.resize(x1-x0);
        for (int x=x0;x<x1;++x) {
            float u=(x+.5f-cx)/sc+sub.w/2.f;
            int sx=u>=0 && u<sub.w ? sub.x+(p.flip?sub.w-1-(int)u:(int)u) : -1;
            columns[x-x0]=sx>=0 && sx<img.width?sx:-1;
        }
        for (int y=y0;y<y1;++y) {
            float v=(y+.5f-cy)/sc+sub.h/2.f;
            int sy=sub.y+(int)v;
            if(v<0 || v>=sub.h || sy<0 || sy>=img.height)continue;
            const auto *row=img.rowData(sy);
            for(int x=x0;x<x1;++x) {
                int sx=columns[x-x0];if(sx<0)continue;
                const auto *c=row+sx*4;
                if(c[3]==255)pax_pixels[y*PAX_W+x]=rgb565(c[0],c[1],c[2]);
                else if(c[3])blendPixel(x,y,c[0],c[1],c[2],c[3]);
            }
            if ((y&31)==0)pax_audio_pump();
        }
        return;
    }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + .5f - cx, dy = y + .5f - cy, u = (cs * dx + sn * dy) / sc + sub.w / 2.f,
                  v = (-sn * dx + cs * dy) / sc + sub.h / 2.f;
            if (u < 0 || v < 0 || u >= sub.w || v >= sub.h)
                continue;
            int sx = sub.x + (p.flip ? sub.w - 1 - (int)u : (int)u), sy = sub.y + (int)v;
            if (sx < 0 || sy < 0 || sx >= img.width || sy >= img.height)
                continue;
            const uint8_t *s = img.rowData(sy) + sx * 4;
            int bright = p.brightness * 255 / 100;
            pax_blend(x, y, std::clamp(s[0] + bright, 0, 255), std::clamp(s[1] + bright, 0, 255),
                      std::clamp(s[2] + bright, 0, 255), s[3] * std::clamp(p.opacity, 0.f, 1.f));
        }
}
Image_Headless::Image_Headless(std::string f, bool project, bool half, float s)
    : Image(f, project, half, s) {}
Image_Headless::Image_Headless(std::string f, ZipArchive *zip, bool half, float s)
    : Image(f, zip, half, s) {}
Image_Headless::~Image_Headless() = default;
void Image_Headless::render(ImageRenderParams &p) {
    pax_draw_image(imgData, p);
    freeTimer = maxFreeTimer;
}
void Image_Headless::renderNineslice(double x, double y, double w, double h, double,
                                     bool centered) {
    ImageRenderParams p;
    p.x = x;
    p.y = y;
    p.centered = centered;
    p.scale = std::min(w / imgData.width, h / imgData.height) * imgData.scale;
    render(p);
}
void *Image_Headless::getNativeTexture() { return imgData.pixels; }
nonstd::expected<void, std::string> Image_Headless::refreshTexture() { return {}; }
static std::string asciiText(const std::string &txt) {
    std::string out;
    for (unsigned char c : txt)
        out += c == 10 || (c >= 32 && c <= 126) ? (char)c : '?';
    return out;
}
void pax_draw_text(const std::string &original, int x, int y, int color, float scale,
                   bool centered) {
    pax_audio_pump();
    auto txt = asciiText(original);
    if (txt.empty() || scale <= 0)
        return;
    if (pax_font_draw(txt, x, y, color, scale, centered))
        return;
    std::vector<char> text(txt.begin(), txt.end());
    text.push_back(0);
    float s = 2 * scale;
    if (centered)
        x -= (int)(stb_easy_font_width(text.data()) * s / 2);
    struct Vertex {
        float x, y, z;
        unsigned char c[4];
    };
    std::vector<Vertex> verts(txt.size() * 64 + 4);
    int quads = stb_easy_font_print(0, 0, text.data(), nullptr, verts.data(),
                                    verts.size() * sizeof(Vertex));
    // Text colors use Scratch's RGBA integer (R in the highest byte).
    int r = (color >> 24) & 255, g = (color >> 16) & 255, b = (color >> 8) & 255, a = color & 255;
    for (int q = 0; q < quads; q++) {
        auto v = &verts[q * 4];
        float l = v[0].x, t = v[0].y, rr = l, bb = t;
        for (int j = 1; j < 4; j++) {
            l = std::min(l, v[j].x);
            t = std::min(t, v[j].y);
            rr = std::max(rr, v[j].x);
            bb = std::max(bb, v[j].y);
        }
        for (int yy = y + (int)(t * s); yy < y + (int)(bb * s); yy++)
            for (int xx = x + (int)(l * s); xx < x + (int)(rr * s); xx++)
                pax_blend(xx, yy, r, g, b, a);
    }
}
TextObjectHeadless::TextObjectHeadless(std::string t, double x, double y, std::string font)
    : TextObject(t, x, y, font) {}
TextObjectHeadless::~TextObjectHeadless() = default;
void TextObjectHeadless::setText(std::string t) { text = std::move(t); }
void TextObjectHeadless::render(int x, int y) {
    pax_draw_text(text, x, y, color, scale, centerAligned);
}
std::vector<float> TextObjectHeadless::getSize() { return getStringSize(text); }
std::vector<float> TextObjectHeadless::getStringSize(const std::string &t) {
    float width, height;
    if (pax_font_measure(asciiText(t), scale, width, height))
        return {width, height};
    return {(float)stb_easy_font_width((char *)asciiText(t).c_str()) * 2 * scale,
            (float)stb_easy_font_height((char *)asciiText(t).c_str()) * 2 * scale};
}
namespace {
int cursorX=-1, cursorY=-1, cursorW=0, cursorH=0;
uint16_t cursorUnder[10*18];
int64_t cursorPresentedAt=0;
void removeCursor() {
  if (cursorX >= 0 && pax_pixels)
    for (int y=0; y<cursorH; ++y)
      std::copy_n(cursorUnder+y*10, cursorW, pax_pixels+(cursorY+y)*PAX_W+cursorX);
  cursorX=-1;
}
void drawCursor() {
  if (!pax_mouse_present || pax_native_cursor || !pax_pixels) return;
  auto pos=Input::getTouchPosition();
  cursorX=std::clamp(pos[0],0,PAX_W-1);cursorY=std::clamp(pos[1],0,PAX_H-1);
  cursorW=std::min(10,PAX_W-cursorX);cursorH=std::min(18,PAX_H-cursorY);
  for(int y=0;y<cursorH;++y) {
    std::copy_n(pax_pixels+(cursorY+y)*PAX_W+cursorX,cursorW,cursorUnder+y*10);
    for(int x=0;x<cursorW&&x<=y/2;++x)
      pax_pixels[(cursorY+y)*PAX_W+cursorX+x]=(x==0||x==y/2||y==17)?0xffff:0x18c3;
  }
}
void presentFrame() {
  cursorPresentedAt=pax_time_us();
  int cw=PAX_W,ch=PAX_H;
  if(pax_svc->display_get_size)pax_svc->display_get_size(&cw,&ch);
  pax_svc->display_write_frame_custom(pax_pixels,PAX_W,PAX_H,std::min((float)cw/PAX_W,(float)ch/PAX_H),false);
}
}
void pax_refresh_pointer() {
  auto pos=Input::getTouchPosition();
  const bool heartbeat = pax_time_us()-cursorPresentedAt >= 500000;
  if ((!pax_mouse_present || pax_native_cursor) && cursorX<0 && !heartbeat) return;
  if (pax_mouse_present && !pax_native_cursor && pos[0]==cursorX && pos[1]==cursorY && !heartbeat) return;
  removeCursor();drawCursor();presentFrame();
}
bool pax_resize_surface(int w, int h) {
    cursorX=-1;
    if (w < 1 || h < 1 || w > 1024 || h > 768)
        return false;
    auto *pixels = (uint16_t *)pax_svc->mem_caps_alloc(w * h * 2, PAX_MEM_CAP_SPIRAM);
    if (!pixels)
        return false;
    pax_svc->mem_free(pax_pixels);
    pax_pixels = pixels;
    PAX_W = w;
    PAX_H = h;
    pen.clear();
    pen.shrink_to_fit();
    penTarget = false;
    penAllocationFailed = false;
    Render::hasFrameBegan = false;
    pax_set_clip(0, 0, w, h);
    return true;
}
bool Render::Init() {
    pax_set_clip(0, 0, PAX_W, PAX_H);
    pax_pixels = (uint16_t *)pax_svc->mem_caps_alloc(PAX_W * PAX_H * 2, PAX_MEM_CAP_SPIRAM);
    return pax_pixels != nullptr;
}
void Render::deInit() {
    destroySpeechManager();
    pen.clear();
    pen.shrink_to_fit();
    pax_svc->mem_free(pax_pixels);
    pax_pixels = nullptr;
    TextObject::cleanupText();
}
void *Render::getRenderer() { return pax_pixels; }
void Render::setRenderTarget(void *) {}
void Render::clearRenderTarget() {}
bool Render::createSpeechManager() {
    if (!speech)
        speech = new PaxSpeech();
    return true;
}
void Render::destroySpeechManager() {
    delete speech;
    speech = nullptr;
}
SpeechManager *Render::getSpeechManager() { return speech; }
void Render::beginFrame(int, int r, int g, int b) {
    if (!hasFrameBegan) {
        cursorX=-1;
        std::fill_n(pax_pixels, PAX_W * PAX_H, rgb565(r, g, b));
        hasFrameBegan = true;
    }
}
static long long nextTurboFrame;
bool pax_turbo_frame_ready() { return pax_time_us() >= nextTurboFrame; }
void pax_turbo_frame_finish() {
    nextTurboFrame = pax_time_us() + 1000000 / std::max(1, Scratch::FPS);
}
void Render::endFrame(bool) {
    if (pax_project_playing || pax_editor_playing) {
        drawBox(78, 30, PAX_W - 123, 17, Scratch::turbo ? 95 : 233,
                Scratch::turbo ? 210 : 241, Scratch::turbo ? 120 : 252);
        pax_draw_text(Scratch::turbo ? "Turbo ON" : "Turbo OFF", PAX_W - 123, 9,
                      0x26374cff, .65f, true);
        drawBox(74, 30, PAX_W - 40, 17, 233, 241, 252);
        pax_draw_icon(PaxIcon::Stop, PAX_W - 52, 5, 24);

    }
    drawCursor();
    presentFrame();
    hasFrameBegan = false;
    pax_audio_pump();
    if ((pax_project_playing || pax_editor_playing) && Scratch::turbo)
        pax_turbo_frame_finish();
}
int Render::getWidth() { return PAX_W; }
int Render::getHeight() { return PAX_H; }
float Render::getPixelDensity() { return 1; }
bool Render::appShouldRun() {
    pax_update_project_clock();
    static long long lastYield = 0;
    const auto now = pax_time_us();
    // A timed slice, rather than one OS sleep per block/loop iteration.
    // Keep elapsed clocks active on every step; device/audio work gets a
    // regular opportunity to run even in a tight turbo loop.
    if (!Scratch::turbo || !(pax_project_playing || pax_editor_playing) ||
        now < lastYield || now - lastYield >= 4000) {
        pax_audio_pump();
        pax_svc->delay_ms(1);
        lastYield = pax_time_us();
    }
    return !OS::toExit;
}
void Render::drawBox(int w, int h, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (pax_ui_zoom==1 && !penTarget && pax_pixels && a==255) {
    int left=std::max({0,clipX,x-w/2}),right=std::min({PAX_W,clipX+clipW,x+(w+1)/2});
    int top=std::max({0,clipY,y-h/2}),bottom=std::min({PAX_H,clipY+clipH,y+(h+1)/2});
    if(right<=left)return;
    uint16_t color=rgb565(r,g,b);
    for(int yy=top;yy<bottom;++yy)std::fill_n(pax_pixels+yy*PAX_W+left,right-left,color);
    return;
  }

  for (int yy = pax_ui_zoom == 1 ? std::max(0, y - h / 2) : y - h / 2;
       yy <
       (pax_ui_zoom == 1 ? std::min(PAX_H, y + (h + 1) / 2) : y + (h + 1) / 2);
       yy++)
    for (int xx = pax_ui_zoom == 1 ? std::max(0, x - w / 2) : x - w / 2;
         xx < (pax_ui_zoom == 1 ? std::min(PAX_W, x + (w + 1) / 2)
                                : x + (w + 1) / 2);
         xx++)
      pax_blend(xx, yy, r, g, b, a);
}
void Render::drawRoundedBox(int w, int h, int x, int y, int, uint8_t r, uint8_t g, uint8_t b,
                            uint8_t a) {
    drawBox(w, h, x, y, r, g, b, a);
}
bool Render::initPen() {
    if (pen.empty()) pen.resize(PAX_H);
    return !penAllocationFailed;
}
void Render::penClear() {
    for (auto &row : pen) row.reset();
    penAllocationFailed = false;
}
void Render::penMoveAccurate(double x1, double y1, double x2, double y2, Sprite *s) {
    if (!initPen()) return;
    auto c = CSBT2RGBA(s->penData.color);
    float scale =
        std::min((float)PAX_W / Scratch::projectWidth, (float)PAX_H / Scratch::projectHeight);
    float ax = PAX_W / 2 + x1 * scale, ay = PAX_H / 2 - y1 * scale, bx = PAX_W / 2 + x2 * scale,
          by = PAX_H / 2 - y2 * scale;
    float radius = std::max(.5f, (float)s->penData.size * scale / 2), dx = bx - ax, dy = by - ay,
          len = dx * dx + dy * dy;
    int l = std::max(0, (int)floorf(std::min(ax, bx) - radius)),
        r = std::min(PAX_W, (int)ceilf(std::max(ax, bx) + radius));
    int t = std::max(0, (int)floorf(std::min(ay, by) - radius)),
        b = std::min(PAX_H, (int)ceilf(std::max(ay, by) + radius));
    penTarget = true;
    for (int y = t; y < b; y++)
        for (int x = l; x < r; x++) {
            float f =
                len ? std::clamp(((x + .5f - ax) * dx + (y + .5f - ay) * dy) / len, 0.f, 1.f) : 0;
            float xx = x + .5f - ax - f * dx, yy = y + .5f - ay - f * dy;
            if (xx * xx + yy * yy <= radius * radius)
                pax_blend(x, y, c.r, c.g, c.b, 255 * (100 - s->penData.color.transparency) / 100);
        }
    penTarget = false;
}
void Render::penMoveFast(double a, double b, double c, double d, Sprite *s) {
    penMoveAccurate(a, b, c, d, s);
}
void Render::penDotAccurate(Sprite *s) {
    penMoveAccurate(s->xPosition, s->yPosition, s->xPosition, s->yPosition, s);
}
void Render::penDotFast(Sprite *s) { penDotAccurate(s); }
static void drawSprite(Sprite *s, bool stamp = false) {
    if ((!s->visible && !stamp) || s->costumes.empty() || s->currentCostume < 0 ||
        s->currentCostume >= (int)s->costumes.size())
        return;
    auto c = Scratch::costumeImages.find(s->costumes[s->currentCostume].fullName);
    if (c == Scratch::costumeImages.end()) {
        Scratch::loadCurrentCostumeImage(s);
        c = Scratch::costumeImages.find(s->costumes[s->currentCostume].fullName);
        if (c == Scratch::costumeImages.end()) return;
    }
    Render::calculateRenderPosition(s, s->costumes[s->currentCostume].isSVG);
    ImageRenderParams p;
    p.x = s->renderInfo.renderX;
    p.y = s->renderInfo.renderY;
    p.scale = s->renderInfo.renderScaleY;
    p.rotation = s->renderInfo.renderRotation;
    p.flip = s->rotationStyle == s->LEFT_RIGHT && s->rotation < 0;
    p.opacity = 1 - std::clamp(s->ghostEffect, 0.f, 100.f) * .01f;
    p.brightness = s->brightnessEffect;
    c->second->render(p);
}
void Render::penStamp(Sprite *s) {
    if (!initPen()) return;
    penTarget = true;
    drawSprite(s, true);
    penTarget = false;
}
void Render::renderPenLayer() {
    if (pen.empty())
        return;
    for (int y = 0; y < PAX_H; y++) {
        if (!pen[y]) continue;
        auto *row = pen[y].get();
        for (int x = 0; x < PAX_W; x++) {
            auto *s = row + x * 4;
            pax_blend(x, y, s[0], s[1], s[2], s[3]);
        }
    }
}
void Render::renderSprites() {
    const auto composeStarted=pax_time_us();
    beginFrame(0, 0, 0, 0);
    drawBox((int)std::lround(Scratch::projectWidth * renderScale),
            (int)std::lround(Scratch::projectHeight * renderScale),
            PAX_W / 2, PAX_H / 2, 255, 255, 255);
    for (auto it = Scratch::sprites.rbegin(); it != Scratch::sprites.rend(); ++it) {
        drawSprite(*it);
        if ((*it)->isStage)
            renderPenLayer();
    }
    if (speech)
        speech->render();
    renderMonitors();
    static int64_t composeTotal=0,composeWorst=0,composeLastLog=0;
    static unsigned composeFrames=0;
    const auto composeTime=pax_time_us()-composeStarted;
    composeTotal+=composeTime;composeWorst=std::max<int64_t>(composeWorst,composeTime);++composeFrames;
    if(pax_time_us()-composeLastLog>=5000000){
      pax_svc->log_printf("Scratch: player compose avg=%lld us worst=%lld us frames=%u\n",(long long)(composeTotal/composeFrames),(long long)composeWorst,composeFrames);
      composeLastLog=pax_time_us();composeTotal=composeWorst=0;composeFrames=0;
    }
    endFrame();
}
