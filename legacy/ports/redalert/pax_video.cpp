// Video backend for the Red Alert PAX (replaces common/video_sdl2.cpp).
//
// The game draws 8-bit paletted pages (640x400 by default). The visible page
// is converted through a 256-entry RGB565 table into the loader's canvas,
// centred, with the mouse cursor drawn on top, whenever the game presents a
// frame (Frame_Limiter -> Video_Render_Frame).
//
// The canvas is one of CANVAS_SIZES, picked from what the loader offers (the
// store's Screen setting, else the listing's recommended 640x400): 640x400
// holds the page exactly, with no border to convert or flush; 800x480 is the
// canvas every PAX had before. Red Alert's UI is laid out for 640x400 (the
// sidebar, radar, tabs, dialogs and movies use fixed coordinates), so the
// page itself never grows with the canvas.
#include "pax_port.h"

#include "common/video.h"
#include "common/wwmouse.h"

#include <stdlib.h>
#include <string.h>

// Prefer the game's native 640x400 page to avoid copying a larger canvas on
// every frame. The 800x480 option remains available for compatibility.
static const struct
{
    int w, h;
} CANVAS_SIZES[] = {{640, 400}, {800, 480}};

// The canvas in use: 800x480 until Set_Video_Mode switches it.
static int s_canvas_w = 800;
static int s_canvas_h = 480;

static uint16_t s_palette[256];
static int s_mode_w = 640;
static int s_mode_h = 400;

// The game's mouse position in page pixels; moved by pax_input.cpp.
float pax_mouse_x = 320.0f;
float pax_mouse_y = 200.0f;

static struct
{
    unsigned char* image;  // palette indices, 0 = transparent
    int w, h, hotx, hoty;
} s_cursor = {nullptr, 0, 0, 0, 0};

class SurfaceMonitorClassPAX : public SurfaceMonitorClass
{
public:
    void Restore_Surfaces() override
    {
    }
    void Set_Surface_Focus(bool) override
    {
    }
    void Release() override
    {
    }
};

static SurfaceMonitorClassPAX s_all_surfaces;
SurfaceMonitorClass& AllSurfaces = s_all_surfaces;

SurfaceMonitorClass::SurfaceMonitorClass()
{
    SurfacesRestored = false;
}

// ── Presenting ──────────────────────────────────────────────────────────────
// Converting a 640x400 page to RGB565 (~13 ms, PSRAM-bound) and the loader's
// flush (~22 ms) used to run on the game task for every frame. Present() now
// only copies the page, palette and cursor into a two-slot pipeline; a
// presenter task on core 1 converts and flushes it. When both slots are owned
// by the presenter, the next frame is dropped instead of making the game wait.

struct Snapshot
{
    unsigned char* pixels;
    size_t capacity;
    int w, h;
    uint16_t palette[256];
    unsigned char cursor[64 * 64];
    int cursor_w, cursor_h, cursor_x, cursor_y; // cursor_w == 0: no cursor
};

// Single producer / single consumer. A slot is exclusively owned by the
// producer until write advances, and by the presenter until read advances.
// One additional slot overlaps page copying with conversion/flush without
// allowing the game to overwrite a frame being displayed.
static Snapshot s_snapshots[2];
static unsigned s_snapshot_write = 0;
static unsigned s_snapshot_read = 0;
static volatile bool s_presenter_quit = false;
static volatile bool s_presenter_done = true;
static void* s_presenter = nullptr;

// Game-side counters for the rate log (read by the presenter).
static volatile int s_offered = 0;
static volatile int s_dropped = 0;
static volatile long long s_copy_us = 0;
static long long s_wait_us = 0; // presenter: time asleep waiting for a frame

// Where a w x h page sits on the canvas: centred, never left of or above it.
static void page_origin(int w, int h, int* ox, int* oy)
{
    *ox = (s_canvas_w - w) / 2 > 0 ? (s_canvas_w - w) / 2 : 0;
    *oy = (s_canvas_h - h) / 2 > 0 ? (s_canvas_h - h) / 2 : 0;
}

static void __attribute__((optimize("O3"))) render(const Snapshot& f, uint16_t* fb)
{
    int ox, oy;
    page_origin(f.w, f.h, &ox, &oy);
    const int stride = s_canvas_w;
    const int cw = f.w < s_canvas_w ? f.w : s_canvas_w;
    const int ch = f.h < s_canvas_h ? f.h : s_canvas_h;
    // The snapshot is in mapped PSRAM; repeated random palette lookups fight
    // the game/page buffers for cache. The presenter's 8 KiB task stack is
    // internal RAM, so keep this small, frequently accessed table there.
    uint16_t pal[256];
    memcpy(pal, f.palette, sizeof(pal));
    // 4 pixels per 32-bit read, two RGB565 pairs per 32-bit write.
    const bool words = ((f.w | cw | ox) & 3) == 0 && (((uintptr_t)f.pixels | (uintptr_t)fb) & 3) == 0;
    for (int y = 0; y < ch; y++) {
        const unsigned char* src = f.pixels + y * f.w;
        uint16_t* dst = fb + (oy + y) * stride + ox;
        int x = 0;
        if (words) {
            const uint32_t* __restrict s4 = reinterpret_cast<const uint32_t*>(src);
            uint32_t* __restrict d2 = reinterpret_cast<uint32_t*>(dst);
            for (; x < cw; x += 4) {
                const uint32_t p = *s4++;
                d2[0] = pal[p & 0xFF] | (uint32_t)pal[(p >> 8) & 0xFF] << 16;
                d2[1] = pal[(p >> 16) & 0xFF] | (uint32_t)pal[p >> 24] << 16;
                d2 += 2;
            }
        }
        for (; x < cw; x++) {
            dst[x] = pal[src[x]];
        }
    }
    for (int y = 0; y < f.cursor_h; y++) {
        const int py = f.cursor_y + y;
        if (py < 0 || py >= ch) {
            continue;
        }
        for (int x = 0; x < f.cursor_w; x++) {
            const int px = f.cursor_x + x;
            const unsigned char c = f.cursor[y * f.cursor_w + x];
            if (c != 0 && px >= 0 && px < cw) {
                fb[(oy + py) * stride + ox + px] = pal[c];
            }
        }
    }
}

// Every 5 s: frames shown and offered per second, and where the time goes.
static void log_rate(const Snapshot& frame, long long convert_us, long long flush_us)
{
    static long long window = 0, total_convert = 0, total_flush = 0;
    static int shown = 0, offered_at_start = 0, dropped_at_start = 0;
    static long long copy_at_start = 0;
    const long long now = pax_time_us();
    if (window == 0) {
        window = now;
        offered_at_start = s_offered;
        dropped_at_start = s_dropped;
        copy_at_start = s_copy_us;
    }
    shown++;
    total_convert += convert_us;
    total_flush += flush_us;
    if (now - window >= 5000000) {
        const long long elapsed = now - window;
        const int offered = s_offered - offered_at_start;
        const long long copy = s_copy_us - copy_at_start;
        int colored_palette = 0, colored_samples = 0;
        for (int i = 0; i < 256; ++i) {
            colored_palette += frame.palette[i] != 0;
        }
        const size_t sample_bytes = (size_t)frame.w * frame.h;
        for (size_t i = 0; i < sample_bytes; i += 257) {
            colored_samples += frame.palette[frame.pixels[i]] != 0;
        }
        pax_svc->log_printf("RA: %d fps shown, %d fps from the game (%d dropped); per frame: copy %lld us, "
                             "convert %lld us, flush %lld us; presenter waited %lld ms\n",
                             (int)(shown * 1000000LL / elapsed), (int)(offered * 1000000LL / elapsed),
                             s_dropped - dropped_at_start, offered > 0 ? copy / offered : 0,
                             total_convert / shown, total_flush / shown, s_wait_us / 1000);
        pax_svc->log_printf("RA video content: %dx%d palette-colored=%d visible-samples=%d/%u\n",
                             frame.w, frame.h, colored_palette, colored_samples,
                             (unsigned)((sample_bytes + 256) / 257));
        s_wait_us = 0;
        window = now;
        shown = 0;
        total_convert = total_flush = 0;
        offered_at_start = s_offered;
        dropped_at_start = s_dropped;
        copy_at_start = s_copy_us;
    }
}

static void present_snapshot(const Snapshot& frame)
{
    uint16_t* fb = pax_svc->display_get_framebuffer();
    if (fb == nullptr) {
        return;
    }
    const long long start = pax_time_us();
    render(frame, fb);
    const long long converted = pax_time_us();
    pax_svc->display_flush();
    log_rate(frame, converted - start, pax_time_us() - converted);
}

static void presenter_task(void*)
{
    while (!s_presenter_quit) {
        const unsigned read = __atomic_load_n(&s_snapshot_read, __ATOMIC_RELAXED);
        if (read == __atomic_load_n(&s_snapshot_write, __ATOMIC_ACQUIRE)) {
            // delay_ms() below one tick (10 ms) is only a yield: sleep a tick.
            const long long t = pax_time_us();
            pax_svc->delay_ms(10);
            s_wait_us += pax_time_us() - t;
            continue;
        }
        present_snapshot(s_snapshots[read % 2]);
        __atomic_store_n(&s_snapshot_read, read + 1, __ATOMIC_RELEASE);
        // Finish a frame at a predictable priority, then block to leave a
        // guaranteed refill interval for the lower-priority port mixer.
        pax_svc->delay_ms(10);
    }
    s_presenter_done = true;
    for (;;) {
        pax_svc->delay_ms(1000); // deleted by pax_video_shutdown
    }
}

extern "C" void pax_video_shutdown(void)
{
    if (s_presenter == nullptr) {
        return;
    }
    s_presenter_quit = true;
    for (int i = 0; i < 200 && !s_presenter_done; i++) {
        pax_svc->delay_ms(5);
    }
    pax_svc->task_delete(s_presenter);
    s_presenter = nullptr;
}

// Called by the game for every frame it presents.
static void pax_present(const unsigned char* pixels, int w, int h)
{
    if (s_presenter == nullptr && s_presenter_done) {
        s_presenter_quit = false;
        s_presenter_done = false;
        // Firmware I2S tasks remain higher priority. The explicit sleep after
        // each frame prevents presentation from starving the port mixer.
        if (pax_svc->task_create(presenter_task, "ra_present", 8 * 1024, nullptr, 6, &s_presenter, 1) != 0) {
            s_presenter = nullptr;
            pax_svc->log_printf("RA: no presenter task, presenting on the game task\n");
        }
    }
    s_offered++;
    const unsigned write = __atomic_load_n(&s_snapshot_write, __ATOMIC_RELAXED);
    if (s_presenter != nullptr
        && write - __atomic_load_n(&s_snapshot_read, __ATOMIC_ACQUIRE) >= 2) {
        s_dropped++; // one frame is displaying and another is already queued
        return;
    }
    Snapshot& s_snap = s_snapshots[write % 2];
    const long long start = pax_time_us();
    const size_t bytes = (size_t)w * h;
    if (s_snap.capacity < bytes) {
        free(s_snap.pixels);
        s_snap.pixels = static_cast<unsigned char*>(malloc(bytes));
        s_snap.capacity = s_snap.pixels != nullptr ? bytes : 0;
        if (s_snap.pixels == nullptr) {
            return;
        }
    }
    memcpy(s_snap.pixels, pixels, bytes);
    s_snap.w = w;
    s_snap.h = h;
    memcpy(s_snap.palette, s_palette, sizeof(s_palette));
    s_snap.cursor_w = s_snap.cursor_h = 0;
    if (s_cursor.image != nullptr && !Get_Mouse_State() && s_cursor.w * s_cursor.h <= (int)sizeof(s_snap.cursor)) {
        memcpy(s_snap.cursor, s_cursor.image, s_cursor.w * s_cursor.h);
        s_snap.cursor_w = s_cursor.w;
        s_snap.cursor_h = s_cursor.h;
        s_snap.cursor_x = (int)pax_mouse_x - s_cursor.hotx;
        s_snap.cursor_y = (int)pax_mouse_y - s_cursor.hoty;
    }
    s_copy_us += pax_time_us() - start;
    if (s_presenter == nullptr) {
        present_snapshot(s_snap);
        return;
    }
    __atomic_store_n(&s_snapshot_write, write + 1, __ATOMIC_RELEASE);
}

class VideoSurfacePAX;
static VideoSurfacePAX* s_front = nullptr;

class VideoSurfacePAX : public VideoSurface
{
public:
    VideoSurfacePAX(int w, int h, GBC_Enum flags)
        : w(w)
        , h(h)
        , pixels(static_cast<unsigned char*>(calloc(w * h, 1)))
    {
        if (flags & GBC_VISIBLE) {
            s_front = this;
        }
    }

    ~VideoSurfacePAX() override
    {
        if (s_front == this) {
            s_front = nullptr;
        }
        free(pixels);
    }

    void* GetData() const override
    {
        return pixels;
    }
    int GetPitch() const override
    {
        return w;
    }
    bool IsAllocated() const override
    {
        return false;
    }
    void AddAttachedSurface(VideoSurface*) override
    {
    }
    bool IsReadyToBlit() override
    {
        return true;
    }
    bool LockWait() override
    {
        return true;
    }
    bool Unlock() override
    {
        return true;
    }

    void Blt(const Rect& dst, VideoSurface* source, const Rect& src, bool mask) override
    {
        VideoSurfacePAX* from = static_cast<VideoSurfacePAX*>(source);
        const int bw = src.Width < dst.Width ? src.Width : dst.Width;
        const int bh = src.Height < dst.Height ? src.Height : dst.Height;
        // Movies copy a complete opaque page on every callback. Avoid bounds
        // checks and virtual-surface address arithmetic for every pixel. Keep
        // the original path for clipping, transparency and self-overlap.
        if (!mask && from != this && bw > 0 && bh > 0 &&
            src.X >= 0 && src.Y >= 0 && dst.X >= 0 && dst.Y >= 0 &&
            bw <= from->w - src.X && bh <= from->h - src.Y &&
            bw <= w - dst.X && bh <= h - dst.Y) {
            if (src.X == 0 && dst.X == 0 && bw == from->w && bw == w) {
                memcpy(pixels + dst.Y * w, from->pixels + src.Y * from->w, (size_t)bw * bh);
            } else {
                for (int y = 0; y < bh; ++y) {
                    memcpy(pixels + (dst.Y + y) * w + dst.X,
                           from->pixels + (src.Y + y) * from->w + src.X, (size_t)bw);
                }
            }
            return;
        }
        for (int y = 0; y < bh; y++) {
            const int sy = src.Y + y;
            const int dy = dst.Y + y;
            if (sy < 0 || sy >= from->h || dy < 0 || dy >= h) {
                continue;
            }
            for (int x = 0; x < bw; x++) {
                const int sx = src.X + x;
                const int dx = dst.X + x;
                if (sx < 0 || sx >= from->w || dx < 0 || dx >= w) {
                    continue;
                }
                const unsigned char c = from->pixels[sy * from->w + sx];
                if (!mask || c != 0) {
                    pixels[dy * w + dx] = c;
                }
            }
        }
    }

    // Same inclusive bounds as the SDL backend (Width/Height + 1).
    void FillRect(const Rect& rect, unsigned char color) override
    {
        for (int y = rect.Y; y <= rect.Y + rect.Height && y < h; y++) {
            if (y < 0) {
                continue;
            }
            int x0 = rect.X < 0 ? 0 : rect.X;
            int x1 = rect.X + rect.Width;
            if (x1 >= w) {
                x1 = w - 1;
            }
            if (x1 >= x0) {
                memset(pixels + y * w + x0, color, x1 - x0 + 1);
            }
        }
    }

    void Present()
    {
        pax_present(pixels, w, h);
    }

private:
    int w;
    int h;
    unsigned char* pixels;
};

void Video_Render_Frame()
{
    if (s_front != nullptr) {
        s_front->Present();
    }
}

Video::Video()
{
}

Video::~Video()
{
}

Video& Video::Shared()
{
    static Video video;
    return video;
}

VideoSurface* Video::CreateSurface(int w, int h, GBC_Enum flags)
{
    return new VideoSurfacePAX(w, h, flags);
}

// Switch to the largest of CANVAS_SIZES that fits in the loader's offer and
// holds the w x h page (the smallest one when none fits the offer). A loader
// without display_set_canvas, or one that refuses, leaves 800x480.
static void choose_canvas(int w, int h)
{
    if (pax_svc->display_get_size == nullptr || pax_svc->display_set_canvas == nullptr) {
        return;
    }
    int offer_w = 0, offer_h = 0;
    pax_svc->display_get_size(&offer_w, &offer_h);
    const int count = (int)(sizeof(CANVAS_SIZES) / sizeof(CANVAS_SIZES[0]));
    int pick = count - 1;
    for (int i = 0; i < count; i++) {
        if (CANVAS_SIZES[i].w <= offer_w && CANVAS_SIZES[i].h <= offer_h && CANVAS_SIZES[i].w >= w
            && CANVAS_SIZES[i].h >= h) {
            pick = i;
            break;
        }
    }
    const int cw = CANVAS_SIZES[pick].w, ch = CANVAS_SIZES[pick].h;
    if (pax_svc->display_set_canvas(cw, ch) == 0) {
        s_canvas_w = cw;
        s_canvas_h = ch;
    }
    pax_svc->log_printf("RA: offered a %dx%d canvas, using %dx%d\n", offer_w, offer_h, s_canvas_w, s_canvas_h);
}

// Called once at startup, on the game task, before the first frame (and so
// before the presenter task that reads the canvas size exists).
bool Set_Video_Mode(int w, int h, int bits_per_pixel)
{
    pax_svc->log_printf("RA: video mode %dx%d %d bpp\n", w, h, bits_per_pixel);
    s_mode_w = w;
    s_mode_h = h;
    pax_mouse_x = w / 2.0f;
    pax_mouse_y = h / 2.0f;
    choose_canvas(w, h);
    pax_svc->display_clear(0x0000);
    return w <= s_canvas_w && h <= s_canvas_h;
}

// Where the game page's top-left corner is on the canvas (touch mapping).
void pax_video_page_origin(int* x, int* y)
{
    page_origin(s_mode_w, s_mode_h, x, y);
}

bool Is_Video_Fullscreen()
{
    return true;
}

void Toggle_Video_Fullscreen()
{
}

void Reset_Video_Mode()
{
}

void Get_Video_Scale(float& x, float& y)
{
    x = 1.0f;
    y = 1.0f;
}

void Set_Video_Cursor_Clip(bool)
{
}

void Move_Video_Mouse(float xrel, float yrel)
{
    pax_mouse_x += xrel;
    pax_mouse_y += yrel;
    if (pax_mouse_x < 0) {
        pax_mouse_x = 0;
    } else if (pax_mouse_x > s_mode_w - 1) {
        pax_mouse_x = (float)(s_mode_w - 1);
    }
    if (pax_mouse_y < 0) {
        pax_mouse_y = 0;
    } else if (pax_mouse_y > s_mode_h - 1) {
        pax_mouse_y = (float)(s_mode_h - 1);
    }
}

void Get_Video_Mouse(int& x, int& y)
{
    x = (int)pax_mouse_x;
    y = (int)pax_mouse_y;
}

void Set_Video_Cursor(void* cursor, int w, int h, int hotx, int hoty)
{
    free(s_cursor.image);
    s_cursor.image = nullptr;
    if (cursor != nullptr && w > 0 && h > 0) {
        s_cursor.image = static_cast<unsigned char*>(malloc(w * h));
        if (s_cursor.image != nullptr) {
            memcpy(s_cursor.image, cursor, w * h);
        }
    }
    s_cursor.w = w;
    s_cursor.h = h;
    s_cursor.hotx = hotx;
    s_cursor.hoty = hoty;
}

unsigned int Get_Free_Video_Memory()
{
    return 1000000000;
}

// No blitter: the game keeps its hidden page in plain memory.
unsigned Get_Video_Hardware_Capabilities()
{
    return 0;
}

void Wait_Vert_Blank()
{
}

void Wait_Blit()
{
}

// 256 RGB triplets of 6-bit VGA values -> RGB565 (red in the high bits).
void Set_DD_Palette(void* palette)
{
    const unsigned char* rgb = static_cast<const unsigned char*>(palette);
    for (int i = 0; i < 256; i++) {
        const unsigned r = (rgb[i * 3 + 0] & 0x3F) << 2;
        const unsigned g = (rgb[i * 3 + 1] & 0x3F) << 2;
        const unsigned b = (rgb[i * 3 + 2] & 0x3F) << 2;
        s_palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}
