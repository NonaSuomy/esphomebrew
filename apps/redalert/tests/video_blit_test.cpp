#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "../pax_video.cpp"

extern "C" {
const app_services_t* pax_svc = nullptr;
long long pax_time_us(void) { return 0; }
}
Rect::Rect(int x, int y, int w, int h) : X(x), Y(y), Width(w), Height(h) {}
int Get_Mouse_State(void) { return 1; }

static void check(int sw, int sh, int dw, int dh, Rect src, Rect dst, bool mask)
{
    VideoSurfacePAX from(sw, sh, GBC_NONE), to(dw, dh, GBC_NONE);
    unsigned char* input = static_cast<unsigned char*>(from.GetData());
    unsigned char* output = static_cast<unsigned char*>(to.GetData());
    unsigned char expected[100];
    assert(dw * dh <= (int)sizeof(expected));
    for (int i = 0; i < sw * sh; ++i) input[i] = (unsigned char)(i % 7);
    memset(output, 42, dw * dh);
    memset(expected, 42, dw * dh);
    const int bw = src.Width < dst.Width ? src.Width : dst.Width;
    const int bh = src.Height < dst.Height ? src.Height : dst.Height;
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            const int sx = src.X + x, sy = src.Y + y;
            const int dx = dst.X + x, dy = dst.Y + y;
            if (sx < 0 || sy < 0 || sx >= sw || sy >= sh || dx < 0 || dy < 0 || dx >= dw || dy >= dh) continue;
            const unsigned char c = input[sy * sw + sx];
            if (!mask || c) expected[dy * dw + dx] = c;
        }
    }
    to.Blt(dst, &from, src, mask);
    assert(memcmp(output, expected, dw * dh) == 0);
}

int main()
{
    // Drive the real producer with a simulated (paused) presenter. A full
    // queue must not overwrite either owned slot; freeing one slot must allow
    // another frame in order, including across unsigned counter rollover.
    s_presenter = reinterpret_cast<void*>(1);
    for (unsigned start : {0u, 0xFFFFFFFEu}) {
        s_snapshot_write = s_snapshot_read = start;
        unsigned char first[4] = {1, 2, 3, 4};
        unsigned char second[4] = {5, 6, 7, 8};
        unsigned char third[4] = {9, 10, 11, 12};
        pax_present(first, 2, 2);
        pax_present(second, 2, 2);
        pax_present(third, 2, 2);
        assert(s_snapshot_write == start + 2);
        assert(memcmp(s_snapshots[start % 2].pixels, first, 4) == 0);
        assert(memcmp(s_snapshots[(start + 1) % 2].pixels, second, 4) == 0);
        __atomic_store_n(&s_snapshot_read, start + 1, __ATOMIC_RELEASE);
        pax_present(third, 2, 2);
        assert(s_snapshot_write == start + 3);
        assert(memcmp(s_snapshots[(start + 1) % 2].pixels, second, 4) == 0);
        assert(memcmp(s_snapshots[start % 2].pixels, third, 4) == 0);
    }
    // Exhaustively compare the fast path and clipped/masked fallback to the
    // old pixel-copy semantics, including partial rows and offscreen areas.
    for (int sx = -1; sx <= 5; ++sx)
        for (int sy = -1; sy <= 4; ++sy)
            for (int dx = -1; dx <= 6; ++dx)
                for (int dy = -1; dy <= 5; ++dy)
                    for (int mask = 0; mask <= 1; ++mask)
                        check(5, 4, 6, 5, Rect(sx, sy, 4, 3), Rect(dx, dy, 4, 3), mask);
    check(5, 4, 5, 4, Rect(0, 0, 5, 4), Rect(0, 0, 5, 4), false);
    // Four-pixel conversion and unaligned/tail conversion must agree. Cursor
    // index zero stays transparent and negative/offscreen pixels are clipped.
    for (int width = 5; width <= 8; ++width) {
        s_canvas_w = width;
        s_canvas_h = 4;
        unsigned char indices[32];
        uint16_t framebuffer[32];
        Snapshot frame = {};
        frame.pixels = indices;
        frame.w = width;
        frame.h = 4;
        for (int i = 0; i < 256; ++i) frame.palette[i] = (uint16_t)(i * 251);
        for (int i = 0; i < width * 4; ++i) indices[i] = (unsigned char)i;
        frame.cursor_w = 2;
        frame.cursor_h = 1;
        frame.cursor_x = -1;
        frame.cursor_y = 1;
        frame.cursor[0] = 0;
        frame.cursor[1] = 250;
        render(frame, framebuffer);
        for (int i = 0; i < width * 4; ++i)
            assert(framebuffer[i] == frame.palette[i == width ? 250 : indices[i]]);
    }
    // Exercise larger pages, non-word-aligned row starts, and centered
    // margins. Conversion must leave pixels outside the page untouched.
    for (int width : {127, 128, 129, 255, 256, 257, 640}) {
        s_canvas_w = width + 6;
        s_canvas_h = 5;
        unsigned char* indices = new unsigned char[width * 3];
        uint16_t* framebuffer = new uint16_t[s_canvas_w * s_canvas_h];
        Snapshot frame = {};
        frame.pixels = indices;
        frame.w = width;
        frame.h = 3;
        for (int i = 0; i < 256; ++i) frame.palette[i] = (uint16_t)(i * 251);
        for (int i = 0; i < width * 3; ++i) indices[i] = (unsigned char)(i * 17);
        for (int i = 0; i < s_canvas_w * s_canvas_h; ++i) framebuffer[i] = 0xBEEF;
        render(frame, framebuffer);
        for (int y = 0; y < s_canvas_h; ++y) {
            for (int x = 0; x < s_canvas_w; ++x) {
                const bool inside = y >= 1 && y < 4 && x >= 3 && x < width + 3;
                const uint16_t expected = inside ? frame.palette[indices[(y - 1) * width + x - 3]] : 0xBEEF;
                assert(framebuffer[y * s_canvas_w + x] == expected);
            }
        }
        delete[] indices;
        delete[] framebuffer;
    }
    puts("Red Alert video blit tests passed");
}
