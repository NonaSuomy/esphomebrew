#define PAX_APP_SIDE 1
#include "pax_platform.h"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_mixer.h>
#include "pax_indexed_png.h"
#include "pax_touch_samples.h"
#include "pax_keyboard.h"
#include "pax_pixels.h"
#include "video.h"

#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <filesystem>

/* A deliberately small software SDL2 implementation. Stratagus uses SDL2 as
 * an internal pixel/event/audio API; the PAX has no desktop window system.
 * Surfaces are real SDL layouts, while windows/renderers/textures are opaque
 * PAX-owned handles. */
struct SDL_Window { int w; int h; Uint32 flags; };
struct SDL_Renderer { SDL_Texture *target; Uint8 r, g, b, a; };
struct SDL_Texture { int w; int h; Uint32 format; void *pixels; int pitch; };

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Surface *s_screen;
static Uint32 s_init_flags;
static Uint32 s_buttons;
static int s_mouse_x;
static int s_mouse_y;
static Uint32 s_last_pad;
static char s_error[128];
static SDL_Event s_events[32];
static unsigned s_event_read, s_event_write;
static PaxTouchQueue s_touch_samples;
static PaxMouseQueue s_mouse_samples;
static int s_usb_buttons;
static PaxPaletteCache s_palette_cache;
static void *s_touch_task;
static int s_touch_stop, s_touch_alive;
static constexpr Uint32 PAX_SURFACE_COLORKEY = 0x80000000u;
static constexpr Uint32 PAX_SURFACE_EMBEDDED = 0x40000000u;
// SDL's public flags leave these bits unused in this bridge. Keep alpha
// modulation out of userdata, which stores the indexed color key.
static constexpr Uint32 PAX_ALPHA_MASK = 0x00ff0000u;
#ifndef PAX_STRATAGUS_DATA_ROOT
#define PAX_STRATAGUS_DATA_ROOT "/sd/roms/stargus"
#endif
struct TerrainFrames { SDL_Surface *surface; const Uint8 *lookup; unsigned frames; void *owned{}; };
static TerrainFrames s_terrain_frames[2];
extern "C" unsigned pax_stratagus_terrain_frame_count(SDL_Surface *surface) {
	for (const auto &entry:s_terrain_frames) if (entry.surface==surface) return entry.frames;
	return 0;
}
extern "C" bool pax_stratagus_terrain_frame_pos(SDL_Surface *surface,unsigned frame,int *x,int *y) {
	for (const auto &entry:s_terrain_frames) if (entry.surface==surface && frame<entry.frames) {
		const unsigned tile=entry.lookup[frame*2] | unsigned(entry.lookup[frame*2+1])<<8;
		*x=tile%8*32; *y=tile/8*32; return true;
	}
	return false;
}

static void *palloc(size_t n)
{
	if (!_pax_svc) return nullptr;
	void *p = _pax_svc->mem_caps_alloc ?
		_pax_svc->mem_caps_alloc(n, PAX_MEM_CAP_SPIRAM) : nullptr;
	if (!p && _pax_svc->mem_alloc) p=_pax_svc->mem_alloc(n);
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
	static bool large_failure_reported;
	if (!p && n>=1024*1024 && !large_failure_reported && _pax_svc->log_printf) {
		large_failure_reported=true;
		_pax_svc->log_printf("Stargus: SDL allocation failed bytes=%u caller=%p\n",(unsigned)n,__builtin_return_address(0));
	}
#endif
	return p;
}

static void pfree(void *p)
{
	if (p && _pax_svc && _pax_svc->mem_free) _pax_svc->mem_free(p);
}

static SDL_PixelFormat *make_format(int depth, Uint32 r, Uint32 g,
	Uint32 b, Uint32 a)
{
	SDL_PixelFormat *f = (SDL_PixelFormat *)palloc(sizeof(*f));
	if (!f) return nullptr;
	memset(f, 0, sizeof(*f));
	f->format = SDL_MasksToPixelFormatEnum(depth, r, g, b, a);
	f->BitsPerPixel = (Uint8)depth;
	f->BytesPerPixel = (Uint8)(depth / 8);
	f->Rmask = r; f->Gmask = g; f->Bmask = b; f->Amask = a;
	/* SDL_MapRGB/SDL_MapRGBA use the shifts, not only the masks. */
	while (r && !(r & 1u)) { ++f->Rshift; r >>= 1; }
	while (g && !(g & 1u)) { ++f->Gshift; g >>= 1; }
	while (b && !(b & 1u)) { ++f->Bshift; b >>= 1; }
	while (a && !(a & 1u)) { ++f->Ashift; a >>= 1; }
	return f;
}

static SDL_Surface *surface_alloc(int width, int height, int depth,
	Uint32 r, Uint32 g, Uint32 b, Uint32 a, void *pixels, int pitch)
{
	if (width <= 0 || height <= 0) return nullptr;
	SDL_Surface *s = (SDL_Surface *)palloc(sizeof(*s));
	if (!s) return nullptr;
	memset(s, 0, sizeof(*s));
	s->flags = PAX_ALPHA_MASK;
	s->format = make_format(depth, r, g, b, a);
	if (!s->format) { pfree(s); return nullptr; }
	if (depth == 8) {
		s->format->palette = (SDL_Palette *)palloc(sizeof(SDL_Palette));
		if (!s->format->palette) { pfree(s->format); pfree(s); return nullptr; }
		memset(s->format->palette, 0, sizeof(SDL_Palette));
		s->format->palette->ncolors = 256;
		s->format->palette->colors = (SDL_Color *)palloc(256 * sizeof(SDL_Color));
		if (!s->format->palette->colors) {
			pfree(s->format->palette);
			pfree(s->format);
			pfree(s);
			return nullptr;
		}
		memset(s->format->palette->colors, 0, 256 * sizeof(SDL_Color));
	}
	s->w = width; s->h = height;
	s->pitch = pitch > 0 ? pitch : width * s->format->BytesPerPixel;
	s->clip_rect = {0, 0, (Uint16)width, (Uint16)height};
	const bool adopted_pixels = pixels != nullptr;
	if (adopted_pixels) s->pixels = pixels;
	else s->pixels = palloc((size_t)s->pitch * height);
	if (!s->pixels) {
		if (s->format->palette) {
			s_palette_cache.invalidate(s->format->palette);
			pfree(s->format->palette->colors);
			pfree(s->format->palette);
		}
		pfree(s->format);
		pfree(s);
		return nullptr;
	}
	if (!adopted_pixels) memset(s->pixels, 0, (size_t)s->pitch * height);
	return s;
}

extern "C" int SDL_Init(Uint32 flags)
{
	s_init_flags |= flags;
	return 0;
}

extern "C" void SDL_Quit(void) { pax_stratagus_pointer_stop(); s_init_flags = 0; }
extern "C" Uint32 SDL_WasInit(Uint32 flags) { return s_init_flags & flags; }
extern "C" const char *SDL_GetError(void) { return s_error; }
extern "C" void SDL_ClearError(void) { s_error[0] = 0; }
extern "C" int SDL_SetError(const char *fmt, ...)
{
	(void)fmt;
	strncpy(s_error, "Stratagus PAX SDL error", sizeof(s_error) - 1);
	s_error[sizeof(s_error) - 1] = 0;
	return -1;
}

extern "C" Uint32 SDL_MasksToPixelFormatEnum(int bpp, Uint32 r, Uint32 g,
	Uint32 b, Uint32 a)
{
	if (bpp == 16 && r == 0xf800 && g == 0x07e0 && b == 0x001f)
		return SDL_PIXELFORMAT_RGB565;
	if (bpp == 32 && r == 0x00ff0000 && g == 0x0000ff00 &&
		b == 0x000000ff && a == 0xff000000)
		return SDL_PIXELFORMAT_ARGB8888;
	return SDL_PIXELFORMAT_UNKNOWN;
}

extern "C" void SDL_free(void *p) { pfree(p); }
extern "C" void *SDL_malloc(size_t n) { return palloc(n); }
extern "C" void *SDL_calloc(size_t n, size_t size)
{
	void *p = palloc(n * size);
	if (p) memset(p, 0, n * size);
	return p;
}
extern "C" void *SDL_memset(void *p, int c, size_t n) { return memset(p, c, n); }
extern "C" char *SDL_getenv(const char *) { return nullptr; }
extern "C" int SDL_setenv(const char *, const char *, int) { return 0; }

extern "C" Uint32 SDL_GetTicks(void)
{
	return _pax_svc && _pax_svc->get_time_us ?
		(Uint32)(_pax_svc->get_time_us() / 1000) : 0;
}

extern "C" Uint64 SDL_GetPerformanceCounter(void)
{
	return _pax_svc && _pax_svc->get_time_us ?
		(Uint64)_pax_svc->get_time_us() : 0;
}

extern "C" Uint64 SDL_GetPerformanceFrequency(void) { return 1000000; }
extern "C" void SDL_Delay(Uint32 ms)
{
	if (_pax_svc && _pax_svc->delay_ms) _pax_svc->delay_ms((int)(ms ? ms : 1));
}

extern "C" SDL_Window *SDL_CreateWindow(const char *, int, int, int w, int h,
	Uint32 flags)
{
	if (s_window) return s_window;
	s_window = (SDL_Window *)palloc(sizeof(*s_window));
	if (s_window) { s_window->w = w; s_window->h = h; s_window->flags = flags; }
	pax_stratagus_video_init(w, h);
	return s_window;
}

extern "C" void SDL_DestroyWindow(SDL_Window *w) { if (w == s_window) { pfree(w); s_window = nullptr; } }
extern "C" void SDL_GetWindowSize(SDL_Window *w, int *x, int *y)
{ if (x) *x = w ? w->w : 0; if (y) *y = w ? w->h : 0; }
extern "C" Uint32 SDL_GetWindowFlags(SDL_Window *w) { return w ? w->flags : 0; }
extern "C" void SDL_SetWindowSize(SDL_Window *w, int x, int y)
{ if (w) { w->w = x; w->h = y; } }

extern "C" SDL_Renderer *SDL_CreateRenderer(SDL_Window *, int, Uint32)
{
	if (s_renderer) return s_renderer;
	s_renderer = (SDL_Renderer *)palloc(sizeof(*s_renderer));
	if (s_renderer) memset(s_renderer, 0, sizeof(*s_renderer));
	return s_renderer;
}
extern "C" void SDL_DestroyRenderer(SDL_Renderer *r) { if (r == s_renderer) { pfree(r); s_renderer = nullptr; } }
extern "C" int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 R, Uint8 G, Uint8 B, Uint8 A)
{ if (!r) return -1; r->r=R; r->g=G; r->b=B; r->a=A; return 0; }
extern "C" int SDL_RenderClear(SDL_Renderer *r)
{ if (!r || !r->target || !r->target->pixels) return 0; memset(r->target->pixels, 0, (size_t)r->target->pitch * r->target->h); return 0; }
extern "C" void SDL_RenderPresent(SDL_Renderer *r)
{
	if (r && r->target) {
		if (r->target->format==SDL_PIXELFORMAT_RGB565)
			pax_stratagus_video_present565(r->target->pixels,r->target->w,r->target->h);
		else pax_stratagus_video_present(r->target->pixels,r->target->w,r->target->h);
	}
}
extern "C" int SDL_RenderSetLogicalSize(SDL_Renderer *, int, int) { return 0; }
extern "C" int SDL_RenderSetViewport(SDL_Renderer *, const SDL_Rect *) { return 0; }

extern "C" SDL_Surface *SDL_CreateRGBSurface(Uint32, int w, int h, int depth,
	Uint32 r, Uint32 g, Uint32 b, Uint32 a)
{ return surface_alloc(w, h, depth, r, g, b, a, nullptr, 0); }

extern "C" SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int w, int h,
	int depth, int pitch, Uint32 r, Uint32 g, Uint32 b, Uint32 a)
{ return surface_alloc(w, h, depth, r, g, b, a, pixels, pitch); }

extern "C" SDL_Surface *pax_stratagus_screen_create(int w, int h)
{
	void *pixels=(w==800 && h==480 && _pax_svc && _pax_svc->display_get_framebuffer) ?
		_pax_svc->display_get_framebuffer() : nullptr;
	SDL_Surface *surface=surface_alloc(w,h,16,0xf800,0x07e0,0x001f,0,pixels,0);
	// The launcher owns its canvas; the engine must never free it on resize
	// or exit. Drawing in place eliminates the full BGRA conversion/copy.
	if (surface && pixels) surface->flags|=PAX_SURFACE_EMBEDDED;
	return surface;
}

extern "C" void SDL_FreeSurface(SDL_Surface *s)
{
	if (!s) return;
	for (auto &entry:s_terrain_frames) if (entry.surface==s) { pfree(entry.owned); entry={}; }
	if (s == s_screen) s_screen = nullptr;
	if (!(s->flags & PAX_SURFACE_EMBEDDED)) pfree(s->pixels);
	if (s->format) {
		if (s->format->palette) {
			s_palette_cache.invalidate(s->format->palette);
			pfree(s->format->palette->colors);
			pfree(s->format->palette);
		}
		pfree(s->format);
	}
	pfree(s);
}
extern "C" int SDL_LockSurface(SDL_Surface *) { return 0; }
extern "C" void SDL_UnlockSurface(SDL_Surface *) {}

extern "C" Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b)
{ return !f ? 0 : f->BytesPerPixel==2 ? pax_rgb_to_rgb565((Uint32)r<<16 | (Uint32)g<<8 | b) :
	(((r << f->Rshift) & f->Rmask) | ((g << f->Gshift) & f->Gmask) | ((b << f->Bshift) & f->Bmask)); }
extern "C" Uint32 SDL_MapRGBA(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{ return f ? (SDL_MapRGB(f,r,g,b) | ((Uint32)a << f->Ashift & f->Amask)) : 0; }
extern "C" void SDL_GetRGB(Uint32 p, const SDL_PixelFormat *f, Uint8 *r, Uint8 *g, Uint8 *b)
{
	if (f && f->BytesPerPixel==2) {
		p=pax_rgb565_to_rgb(p);
		if(r)*r=p>>16; if(g)*g=p>>8; if(b)*b=p;
	} else {
		if (r) *r = f ? (Uint8)((p & f->Rmask) >> f->Rshift) : 0;
		if (g) *g = f ? (Uint8)((p & f->Gmask) >> f->Gshift) : 0;
		if (b) *b = f ? (Uint8)((p & f->Bmask) >> f->Bshift) : 0;
	}
}
extern "C" void SDL_GetRGBA(Uint32 p, const SDL_PixelFormat *f, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
{ SDL_GetRGB(p,f,r,g,b); if(a)*a=f&&f->Amask?(Uint8)((p&f->Amask)>>f->Ashift):255; }

extern "C" int SDL_FillRect(SDL_Surface *s, const SDL_Rect *rect, Uint32 color)
{
	if (!s || !s->pixels) return -1;
	SDL_Rect r = rect ? *rect : SDL_Rect{0,0,(Uint16)s->w,(Uint16)s->h};
	const int left = r.x > s->clip_rect.x ? r.x : s->clip_rect.x;
	const int top = r.y > s->clip_rect.y ? r.y : s->clip_rect.y;
	int right = r.x + r.w, bottom = r.y + r.h;
	if (right > s->clip_rect.x + s->clip_rect.w) right = s->clip_rect.x + s->clip_rect.w;
	if (bottom > s->clip_rect.y + s->clip_rect.h) bottom = s->clip_rect.y + s->clip_rect.h;
	if (right > s->w) right = s->w;
	if (bottom > s->h) bottom = s->h;
	if (left < 0 || top < 0 || right <= left || bottom <= top) return 0;
	r = SDL_Rect{left, top, right - left, bottom - top};
	if (color==0 && (s->format->BytesPerPixel==4 || s->format->BytesPerPixel==2)) {
		const int bytes=r.w*s->format->BytesPerPixel;
		Uint8 *pixels=(Uint8 *)s->pixels+r.y*s->pitch+r.x*s->format->BytesPerPixel;
		if (bytes==s->pitch) memset(pixels,0,(size_t)bytes*r.h);
		else for (int y=0;y<r.h;++y) memset(pixels+y*s->pitch,0,bytes);
		return 0;
	}
	if (s->format->BytesPerPixel == 4) {
		for (int y=0; y<r.h; ++y) { Uint32 *p=(Uint32 *)((Uint8 *)s->pixels+(r.y+y)*s->pitch)+r.x; for (int x=0;x<r.w;++x)p[x]=color; }
	} else if (s->format->BytesPerPixel == 2) {
		for (int y=0; y<r.h; ++y) { Uint16 *p=(Uint16 *)((Uint8 *)s->pixels+(r.y+y)*s->pitch)+r.x; for (int x=0;x<r.w;++x)p[x]=(Uint16)color; }
	}
	return 0;
}

extern "C" int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *sr, SDL_Surface *dst, SDL_Rect *dr)
{
	if (!src || !dst || !src->pixels || !dst->pixels) return -1;
	SDL_Rect a = sr ? *sr : SDL_Rect{0,0,(Uint16)src->w,(Uint16)src->h};
	SDL_Rect b = dr ? *dr : SDL_Rect{0,0,(Uint16)dst->w,(Uint16)dst->h};
	// SDL ignores destination w/h for an unscaled blit. Stratagus passes
	// {x,y,0,0}; using those zero dimensions silently discards every tile.
	int w = a.w, h = a.h;
	if (a.x < 0) { const int n = -a.x; a.x = 0; b.x += n; w -= n; }
	if (a.y < 0) { const int n = -a.y; a.y = 0; b.y += n; h -= n; }
	const int left = dst->clip_rect.x > 0 ? dst->clip_rect.x : 0;
	const int top = dst->clip_rect.y > 0 ? dst->clip_rect.y : 0;
	if (b.x < left) { const int n = left - b.x; a.x += n; b.x = left; w -= n; }
	if (b.y < top) { const int n = top - b.y; a.y += n; b.y = top; h -= n; }
	int right = dst->clip_rect.x + dst->clip_rect.w;
	int bottom = dst->clip_rect.y + dst->clip_rect.h;
	if (right > dst->w) right = dst->w;
	if (bottom > dst->h) bottom = dst->h;
	if (w > src->w - a.x) w = src->w - a.x;
	if (h > src->h - a.y) h = src->h - a.y;
	if (w > right - b.x) w = right - b.x;
	if (h > bottom - b.y) h = bottom - b.y;
	if (dr) { dr->x = b.x; dr->y = b.y; dr->w = w > 0 ? w : 0; dr->h = h > 0 ? h : 0; }
	if (w <= 0 || h <= 0) return 0;
	const int src_bytes = src->format->BytesPerPixel;
	const int dst_bytes = dst->format->BytesPerPixel;
	if (src_bytes == dst_bytes) {
		if (src_bytes == 4 && src->format->Amask) {
			/* Embedded unit frames retain transparent pixels in their alpha
			 * channel. Do not overwrite the destination for those pixels. */
			for (int y = 0; y < h; ++y) {
				const Uint32 *sp = (const Uint32 *)((const Uint8 *)src->pixels +
					(a.y + y) * src->pitch) + a.x;
				Uint32 *dp = (Uint32 *)((Uint8 *)dst->pixels +
					(b.y + y) * dst->pitch) + b.x;
				for (int x = 0; x < w; ++x) {
					const Uint32 pixel = sp[x];
					const Uint8 alpha = ((pixel & src->format->Amask) >>
						src->format->Ashift) * ((src->flags&PAX_ALPHA_MASK)>>16) / 255;
					if (alpha == 255) dp[x] = pixel;
					else if (alpha != 0) {
						Uint8 r,g,bl,dr,dg,db;
						SDL_GetRGB(pixel, src->format, &r,&g,&bl);
						SDL_GetRGB(dp[x], dst->format, &dr,&dg,&db);
						dp[x] = SDL_MapRGB(dst->format,
							(r*alpha+dr*(255-alpha))/255,
							(g*alpha+dg*(255-alpha))/255,
							(bl*alpha+db*(255-alpha))/255);
					}
				}
			}
		} else {
			for (int y=0; y<h; ++y) memcpy((Uint8 *)dst->pixels+(b.y+y)*dst->pitch+b.x*dst_bytes,
				(Uint8 *)src->pixels+(a.y+y)*src->pitch+a.x*src_bytes, (size_t)w*src_bytes);
		}
	} else if (src_bytes == 1 && (dst_bytes == 4 || dst_bytes == 2)) {
		if (!src->format->palette) return -1;
		const Uint32 *palette=s_palette_cache.get(src->format->palette,dst->format);
		const int key=(src->flags&PAX_SURFACE_COLORKEY) ? (Uint8)(uintptr_t)src->userdata : -1;
		for (int y=0; y<h; ++y) {
			const Uint8 *sp = (const Uint8 *)src->pixels + (a.y + y) * src->pitch + a.x;
			if (dst_bytes == 4) {
				Uint32 *dp = (Uint32 *)((Uint8 *)dst->pixels + (b.y + y) * dst->pitch) + b.x;
				pax_indexed_row(dp,sp,w,palette,key);
			} else {
				Uint16 *dp = (Uint16 *)((Uint8 *)dst->pixels + (b.y + y) * dst->pitch) + b.x;
				pax_indexed_row(dp,sp,w,palette,key);
			}
		}
	} else if (src_bytes == 2 && dst_bytes == 4) {
		/* Firmware PNG decoding supplies RGB565. The engine screen is ARGB8888. */
		const bool rgb=dst->format->Rmask==0x00ff0000 &&
			dst->format->Gmask==0x0000ff00 && dst->format->Bmask==0x000000ff;
		for (int y=0; y<h; ++y) {
			const Uint16 *sp = (const Uint16 *)((const Uint8 *)src->pixels + (a.y + y) * src->pitch) + a.x;
			Uint32 *dp = (Uint32 *)((Uint8 *)dst->pixels + (b.y + y) * dst->pitch) + b.x;
			if (rgb) {
				for (int x=0;x<w;++x) dp[x]=pax_rgb565_to_rgb(sp[x]);
				continue;
			}
			for (int x = 0; x < w; ++x) {
				const Uint16 p = sp[x];
				const Uint8 r = (Uint8)(((p >> 11) & 31) * 255 / 31);
				const Uint8 g = (Uint8)(((p >> 5) & 63) * 255 / 63);
				const Uint8 bl = (Uint8)((p & 31) * 255 / 31);
				dp[x] = SDL_MapRGB(dst->format, r, g, bl);
			}
		}
	} else if (src_bytes == 4 && dst_bytes == 2) {
		static PaxBlend565Cache blendCache;
		const bool argb=src->format->Rmask==0xff0000 && src->format->Gmask==0xff00 && src->format->Bmask==0xff;
		const unsigned modulation=(src->flags&PAX_ALPHA_MASK)>>16;
		for (int y=0; y<h; ++y) {
			const Uint32 *sp = (const Uint32 *)((const Uint8 *)src->pixels + (a.y + y) * src->pitch) + a.x;
			Uint16 *dp = (Uint16 *)((Uint8 *)dst->pixels + (b.y + y) * dst->pitch) + b.x;
			for (int x = 0; x < w; ++x) {
				const Uint32 pixel=sp[x];
				const unsigned alpha=(src->format->Amask ?
					(pixel&src->format->Amask)>>src->format->Ashift : 255) *
					modulation / 255;
				if (!alpha) continue;
				Uint8 r, g, bl;
				if (argb) { r=pixel>>16; g=pixel>>8; bl=pixel; }
				else SDL_GetRGB(pixel, src->format, &r, &g, &bl);
				if (alpha<255) {
					dp[x]=PaxBlend565Cache::blend(blendCache.get(r,g,bl,alpha),dp[x]);
					continue;
				}
				dp[x] = (Uint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
			}
		}
	} else {
		return -1;
	}
	return 0;
}
extern "C" int SDL_UpperBlitScaled(SDL_Surface *src, const SDL_Rect *a, SDL_Surface *dst, SDL_Rect *b)
{ return SDL_BlitSurface(src, a, dst, const_cast<SDL_Rect *>(b)); }

extern "C" SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, const SDL_PixelFormat *fmt, Uint32)
{
	if (!src || !fmt) return nullptr;
	SDL_Surface *dst=surface_alloc(src->w,src->h,fmt->BitsPerPixel,fmt->Rmask,fmt->Gmask,fmt->Bmask,fmt->Amask,nullptr,0);
	if (dst) {
		if (src->format->palette && dst->format->palette)
			SDL_SetSurfacePalette(dst, src->format->palette);
		// Conversion copies pixels, not a composited image. In particular,
		// transparent indexed entries must retain zero alpha in an RGBA mask.
		if (src->format->BytesPerPixel==dst->format->BytesPerPixel &&
			src->format->Rmask==fmt->Rmask && src->format->Gmask==fmt->Gmask &&
			src->format->Bmask==fmt->Bmask && src->format->Amask==fmt->Amask) {
			for (int y=0;y<src->h;++y)
				memcpy((Uint8 *)dst->pixels+y*dst->pitch,(Uint8 *)src->pixels+y*src->pitch,
					(size_t)src->w*src->format->BytesPerPixel);
			Uint32 key;
			if (!SDL_GetColorKey(src,&key)) SDL_SetColorKey(dst,SDL_TRUE,key);
		} else {
			for (int y=0;y<src->h;++y) for (int x=0;x<src->w;++x) {
				Uint32 pixel=0; Uint8 r,g,b,a;
				memcpy(&pixel,(Uint8 *)src->pixels+y*src->pitch+x*src->format->BytesPerPixel,
					src->format->BytesPerPixel);
				if (src->format->palette) {
					const SDL_Color c=src->format->palette->colors[pixel]; r=c.r;g=c.g;b=c.b;a=c.a;
				} else SDL_GetRGBA(pixel,src->format,&r,&g,&b,&a);
				Uint32 key;
				if (!SDL_GetColorKey(src,&key) && pixel==key) a=0;
				pixel=SDL_MapRGBA(dst->format,r,g,b,a);
				memcpy((Uint8 *)dst->pixels+y*dst->pitch+x*dst->format->BytesPerPixel,&pixel,
					dst->format->BytesPerPixel);
			}
			Uint32 key;
			if (!fmt->Amask && !SDL_GetColorKey(src,&key)) {
				Uint8 r,g,b;
				if (src->format->palette) {
					const SDL_Color c=src->format->palette->colors[key];r=c.r;g=c.g;b=c.b;
				} else SDL_GetRGB(key,src->format,&r,&g,&b);
				SDL_SetColorKey(dst,SDL_TRUE,SDL_MapRGB(dst->format,r,g,b));
			}
		}
		SDL_SetSurfaceAlphaMod(dst,(src->flags&PAX_ALPHA_MASK)>>16);
	}
	return dst;
}
extern "C" SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 format, Uint32 flags)
{
	if (!src) return nullptr;
	SDL_PixelFormat fmt{};
	if (format==SDL_PIXELFORMAT_ARGB8888) {
		fmt.BitsPerPixel=32;fmt.Rmask=0xff0000;fmt.Gmask=0xff00;fmt.Bmask=0xff;fmt.Amask=0xff000000;
	} else if (format==SDL_PIXELFORMAT_RGB565) {
		fmt.BitsPerPixel=16;fmt.Rmask=0xf800;fmt.Gmask=0x7e0;fmt.Bmask=0x1f;
	} else if (format==src->format->format) return SDL_ConvertSurface(src,src->format,flags);
	else { SDL_SetError("Unsupported conversion format"); return nullptr; }
	return SDL_ConvertSurface(src,&fmt,flags);
}

// Fog uses only the first sixteen tile frames of a potentially large terrain
// sheet. Convert that subset, not a 512x768 sheet duplicated into 32-bit RAM.
extern "C" SDL_Surface *pax_stratagus_fog_mask(SDL_Surface *src,int w,int h)
{
	if (!src || w<=0 || h<=0 || src->w<w || src->h<h ||
		(src->w/w)*(src->h/h)<16) return nullptr;
	SDL_Surface *tiles=surface_alloc(w*16,h,src->format->BitsPerPixel,
		src->format->Rmask,src->format->Gmask,src->format->Bmask,src->format->Amask,nullptr,0);
	if (!tiles) return nullptr;
	if (src->format->palette) SDL_SetSurfacePalette(tiles,src->format->palette);
	for (int i=0;i<16;++i) for (int y=0;y<h;++y)
		memcpy((Uint8 *)tiles->pixels+y*tiles->pitch+i*w*src->format->BytesPerPixel,
			(Uint8 *)src->pixels+((i/(src->w/w))*h+y)*src->pitch+
			(i%(src->w/w))*w*src->format->BytesPerPixel,(size_t)w*src->format->BytesPerPixel);
	Uint32 key;
	if (!SDL_GetColorKey(src,&key)) SDL_SetColorKey(tiles,SDL_TRUE,key);
	SDL_Surface *mask=SDL_ConvertSurfaceFormat(tiles,SDL_PIXELFORMAT_ARGB8888,0);
	SDL_FreeSurface(tiles);
	return mask;
}
extern "C" SDL_bool SDL_SetClipRect(SDL_Surface *s, const SDL_Rect *r)
{ if(!s)return SDL_FALSE; s->clip_rect=r?*r:SDL_Rect{0,0,(Uint16)s->w,(Uint16)s->h}; return SDL_TRUE; }
extern "C" void SDL_GetClipRect(SDL_Surface *s, SDL_Rect *r) { if(s&&r)*r=s->clip_rect; }
extern "C" int SDL_SetColorKey(SDL_Surface *s, int enabled, Uint32 key)
{
	if (!s) return -1;
	if (enabled) {
		s->flags |= PAX_SURFACE_COLORKEY;
		s->userdata = (void *)(uintptr_t)key;
	} else {
		s->flags &= ~PAX_SURFACE_COLORKEY;
		s->userdata = nullptr;
	}
	return 0;
}
extern "C" int SDL_GetColorKey(SDL_Surface *s, Uint32 *key)
{
	if (!s || !(s->flags & PAX_SURFACE_COLORKEY)) return -1;
	if (key) *key = (Uint32)(uintptr_t)s->userdata;
	return 0;
}
extern "C" int SDL_SetSurfaceAlphaMod(SDL_Surface *s, Uint8 a)
{ if(!s)return -1; s->flags=(s->flags&~PAX_ALPHA_MASK)|(Uint32(a)<<16);return 0; }
extern "C" int SDL_GetSurfaceAlphaMod(SDL_Surface *s, Uint8 *a)
{ if(!s || !a)return -1; *a=(s->flags&PAX_ALPHA_MASK)>>16;return 0; }
extern "C" int SDL_SetSurfaceColorMod(SDL_Surface *, Uint8, Uint8, Uint8) { return 0; }
extern "C" int SDL_SetSurfaceBlendMode(SDL_Surface *, SDL_BlendMode) { return 0; }
extern "C" int SDL_SetSurfacePalette(SDL_Surface *s, SDL_Palette *palette)
{
	if (!s || !s->format || !s->format->palette || !palette || !palette->colors) return -1;
	const int count = palette->ncolors < s->format->palette->ncolors ?
		palette->ncolors : s->format->palette->ncolors;
	memcpy(s->format->palette->colors, palette->colors, (size_t)count * sizeof(SDL_Color));
	++s->format->palette->version;
	return 0;
}

extern "C" SDL_Texture *SDL_CreateTexture(SDL_Renderer *, Uint32 format, int, int w, int h)
{
	SDL_Texture *t=(SDL_Texture *)palloc(sizeof(*t)); if (!t) return nullptr;
	t->w=w; t->h=h; t->format=format; t->pitch=w*SDL_BYTESPERPIXEL(format);
	/* The PAX presents the software screen directly.  Do not allocate a
	 * second full-screen PSRAM surface for the unused SDL compositor target. */
	if ((w == 320 && h == 200) ||
		(w == 800 && (h == 480 || h == 600)) || (w == 1024 && h == 600)) {
		t->pixels = nullptr;
		return t;
	}
	t->pixels=palloc((size_t)t->pitch*h);
	if (!t->pixels) { pfree(t); return nullptr; } memset(t->pixels,0,(size_t)t->pitch*h); return t;
}
extern "C" void SDL_DestroyTexture(SDL_Texture *t) { if (t) { pfree(t->pixels); pfree(t); } }
extern "C" int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *r, const void *p, int pitch)
{ if (!t || !p) return -1; if (!t->pixels) return 0; const int bpp=SDL_BYTESPERPIXEL(t->format); SDL_Rect q=r?*r:SDL_Rect{0,0,(Uint16)t->w,(Uint16)t->h}; for(int y=0;y<q.h;++y) memcpy((Uint8*)t->pixels+(q.y+y)*t->pitch+q.x*bpp,(const Uint8*)p+y*pitch,(size_t)q.w*bpp); return 0; }
extern "C" SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *r, SDL_Surface *s)
{ if (!s) return nullptr; SDL_Texture *t=SDL_CreateTexture(r,s->format->format,SDL_TEXTUREACCESS_STREAMING,s->w,s->h); if(t) SDL_UpdateTexture(t,nullptr,s->pixels,s->pitch); return t; }
extern "C" int SDL_SetTextureBlendMode(SDL_Texture *, SDL_BlendMode) { return 0; }
extern "C" int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *, const SDL_Rect *)
{ if (!r || !t) return -1; r->target=t; return 0; }
extern "C" int SDL_SetRenderTarget(SDL_Renderer *r, SDL_Texture *t) { if(!r)return -1; r->target=t; return 0; }
extern "C" void SDL_RenderGetLogicalSize(SDL_Renderer *, int *w, int *h) { if(w)*w=s_window?s_window->w:0; if(h)*h=s_window?s_window->h:0; }
extern "C" int SDL_RenderDrawRect(SDL_Renderer *, const SDL_Rect *) { return 0; }
extern "C" int SDL_RenderFillRect(SDL_Renderer *, const SDL_Rect *) { return 0; }
extern "C" int SDL_RenderReadPixels(SDL_Renderer *, const SDL_Rect *, Uint32, void *, int) { return -1; }
extern "C" int SDL_GetRendererInfo(SDL_Renderer *, SDL_RendererInfo *i) { if(!i)return -1; memset(i,0,sizeof(*i)); i->name="pax-software"; return 0; }
extern "C" int SDL_GetNumVideoDisplays(void) { return 1; }
extern "C" int SDL_GetDesktopDisplayMode(int, SDL_DisplayMode *m) { if(!m)return -1; memset(m,0,sizeof(*m)); m->w=800;m->h=480;m->refresh_rate=60;return 0; }
extern "C" int SDL_SetWindowFullscreen(SDL_Window *w, Uint32 f) { if(w)w->flags=f;return 0; }
extern "C" void SDL_SetWindowGrab(SDL_Window *, SDL_bool) {}
extern "C" SDL_bool SDL_GetWindowGrab(SDL_Window *) { return SDL_FALSE; }
extern "C" int SDL_UpdateWindowSurfaceRects(SDL_Window *, const SDL_Rect *, int) { return 0; }
extern "C" SDL_bool SDL_SetHint(const char *, const char *) { return SDL_TRUE; }
extern "C" SDL_bool SDL_SetHintWithPriority(const char *, const char *, SDL_HintPriority) { return SDL_TRUE; }
extern "C" const char *SDL_GetHint(const char *) { return nullptr; }
extern "C" SDL_bool SDL_GetHintBoolean(const char *, SDL_bool d) { return d; }
extern "C" Uint32 SDL_RegisterEvents(int n) { static Uint32 e=SDL_USEREVENT; return n>0?e+=n:(Uint32)-1; }
extern "C" int SDL_PushEvent(SDL_Event *event) {
	if (!event || s_event_write - s_event_read == 32) return -1;
	s_events[s_event_write++ % 32] = *event;
	return 1;
}
extern "C" int SDL_PeepEvents(SDL_Event *, int, SDL_eventaction, Uint32, Uint32) { return 0; }
extern "C" void SDL_StartTextInput(void) {}
extern "C" void SDL_StopTextInput(void) {}
extern "C" int SDL_SetClipboardText(const char *) { return 0; }
extern "C" char *SDL_GetClipboardText(void) { char *p=(char *)palloc(1); if(p)*p=0; return p; }
extern "C" SDL_RWops *SDL_AllocRW(void) { SDL_RWops *p=(SDL_RWops *)palloc(sizeof(SDL_RWops)); if(p) memset(p,0,sizeof(*p)); return p; }
extern "C" void SDL_FreeRW(SDL_RWops *p) { pfree(p); }
extern "C" SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *, int, int) { return nullptr; }
extern "C" void SDL_SetCursor(SDL_Cursor *) {}
extern "C" void SDL_FreeCursor(SDL_Cursor *) {}
extern "C" int SDL_SetPaletteColors(SDL_Palette *p, const SDL_Color *c, int first, int count)
{
	if(!p||!p->colors||!c||first<0||count<0||first>p->ncolors||count>p->ncolors-first)return -1;
	// Fonts and player sprites repeatedly apply the same colors while drawing.
	// Retain their converted lookup table unless the colors actually changed.
	if (memcmp(p->colors+first,c,(size_t)count*sizeof(*c))) {
		memcpy(p->colors+first,c,(size_t)count*sizeof(*c)); ++p->version;
	}
	return 0;
}

static void mouse_move(int x, int y, Uint32 ticks=0)
{
	x=std::max(0,std::min(x,Video.Width-1));
	y=std::max(0,std::min(y,Video.Height-1));
	SDL_Event e{}; e.type = SDL_MOUSEMOTION;
	e.motion.timestamp=ticks;
	e.motion.xrel=x-s_mouse_x; e.motion.yrel=y-s_mouse_y;
	e.motion.state=s_buttons;
	e.motion.x = s_mouse_x = x;
	e.motion.y = s_mouse_y = y;
	SDL_PushEvent(&e);
}
static void mouse_button(int button, bool down, Uint32 ticks=0)
{
	SDL_Event e{}; e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	e.button.timestamp=ticks; e.button.state=down ? SDL_PRESSED : SDL_RELEASED;
	e.button.button = button; e.button.x = s_mouse_x; e.button.y = s_mouse_y;
	if (down) s_buttons |= SDL_BUTTON(button); else s_buttons &= ~SDL_BUTTON(button);
	SDL_PushEvent(&e);
}
static void touch_worker(void *)
{
	PaxTouchFilter touches;
	int last_buttons=0, pending_dx=0, pending_dy=0;
	while (!__atomic_load_n(&s_touch_stop, __ATOMIC_ACQUIRE)) {
		PaxTouchSample sample{};
		sample.down = _pax_svc->touch_read && _pax_svc->touch_read(&sample.x, &sample.y) != 0;
		sample.ticks = SDL_GetTicks();
		touches.update(sample,s_touch_samples);
		int dx=0, dy=0, buttons=0;
		if (_pax_svc->input_mouse_read && _pax_svc->input_mouse_read(&dx,&dy,&buttons)) {
			pending_dx+=dx; pending_dy+=dy;
			if ((pending_dx || pending_dy || buttons!=last_buttons) &&
				s_mouse_samples.push({pending_dx,pending_dy,buttons,sample.ticks})) {
				pending_dx=pending_dy=0; last_buttons=buttons;
			}
		}
		_pax_svc->delay_ms(10);
	}
	__atomic_store_n(&s_touch_alive, 0, __ATOMIC_RELEASE);
	for (;;) _pax_svc->delay_ms(100);
}
void pax_stratagus_pointer_start(void)
{
	if (s_touch_task || !_pax_svc || (!_pax_svc->touch_read && !_pax_svc->input_mouse_read) ||
		!_pax_svc->task_create || !_pax_svc->task_delete) return;
	__atomic_store_n(&s_touch_stop, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&s_touch_alive, 1, __ATOMIC_RELEASE);
	if (_pax_svc->task_create(touch_worker, "stargus_touch", 8192, nullptr, 6, &s_touch_task, 0)) {
		__atomic_store_n(&s_touch_alive, 0, __ATOMIC_RELEASE);
		if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: touch sampler unavailable; using frame polling\n");
	} else if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: touch and USB mouse sampler active at 100 Hz\n");
}
void pax_stratagus_pointer_stop(void)
{
	__atomic_store_n(&s_touch_stop, 1, __ATOMIC_RELEASE);
	while (__atomic_load_n(&s_touch_alive, __ATOMIC_ACQUIRE)) _pax_svc->delay_ms(5);
	if (s_touch_task) _pax_svc->task_delete(s_touch_task);
	s_touch_task=nullptr;
}
static void poll_pointer()
{
	static bool touching, dragging;
	static int start_x, start_y;
	static Uint32 started;
	PaxTouchSample sample{};
	bool have_sample=true;
	if (s_touch_task) have_sample=s_touch_samples.pop(sample);
	else {
		sample.down=_pax_svc && _pax_svc->touch_read && _pax_svc->touch_read(&sample.x, &sample.y);
		sample.ticks=SDL_GetTicks();
	}
	// The launcher reports touch in the 800x480 presentation canvas; the
	// native Warcraft UI renders at 640x480 (or another logical resolution).
	const int x=sample.x*Video.Width/800, y=sample.y*Video.Height/480;
	const bool down=sample.down;
	if (have_sample) {
	if (down && !touching) {
		touching = true; dragging = false; started = sample.ticks;
		start_x = x; start_y = y; mouse_move(x, y, sample.ticks);
	} else if (down) {
		if (!dragging && ((x-start_x)*(x-start_x)+(y-start_y)*(y-start_y) >= 64)) {
			dragging = true; mouse_button(SDL_BUTTON_LEFT, true, sample.ticks);
		}
		if (x != s_mouse_x || y != s_mouse_y) mouse_move(x, y, sample.ticks);
	} else if (touching) {
		touching = false;
		const int button = dragging ? SDL_BUTTON_LEFT :
			pax_stratagus_touch_button(sample.ticks-started >= 500);
		if (!dragging) mouse_button(button, true, started);
		mouse_button(button, false, sample.ticks);
		if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: touch %s at %d,%d\n",
			dragging ? "drag" : button == SDL_BUTTON_RIGHT ? "command" : "tap", s_mouse_x, s_mouse_y);
	}
	}
	PaxMouseSample mouse{};
	bool have_mouse=false;
	// Keep touchscreen drags exclusive, but do not compare physical mouse
	// state with touch-generated SDL buttons or synthesize an early release.
	if (!touching && s_event_read==s_event_write) {
		if (s_touch_task) have_mouse=pax_pop_mouse(s_mouse_samples,mouse,s_usb_buttons);
		else if (_pax_svc && _pax_svc->input_mouse_read) {
			have_mouse=_pax_svc->input_mouse_read(&mouse.dx,&mouse.dy,&mouse.buttons)!=0;
			mouse.ticks=SDL_GetTicks();
		}
	}
	if (have_mouse) {
		if (mouse.dx || mouse.dy) mouse_move(s_mouse_x+mouse.dx, s_mouse_y+mouse.dy, mouse.ticks);
		const int mapping[] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE};
		for (int i = 0; i < 3; ++i)
			if ((mouse.buttons^s_usb_buttons) & (1 << i))
				mouse_button(mapping[i], mouse.buttons & (1 << i), mouse.ticks);
		if ((mouse.buttons^s_usb_buttons) && _pax_svc->log_printf)
			_pax_svc->log_printf("Stargus: USB mouse buttons=%d at %d,%d\n",mouse.buttons,s_mouse_x,s_mouse_y);
		s_usb_buttons=mouse.buttons;
	}
}
static void keyboard_event(SDL_Event *event, SDL_Keycode key, bool down)
{
	static Uint16 modifiers;
	const Uint16 bit=key==SDLK_LSHIFT ? KMOD_LSHIFT : key==SDLK_LCTRL ? KMOD_LCTRL : key==SDLK_LALT ? KMOD_LALT : 0;
	if (down) modifiers|=bit; else modifiers&=~bit;
	event->type=down ? SDL_KEYDOWN : SDL_KEYUP;
	event->key.state=down ? SDL_PRESSED : SDL_RELEASED;
	event->key.keysym.sym=key; event->key.keysym.mod=modifiers;
#ifdef PAX_STRATAGUS_WARCRAFT
	// Native SDL2 Guichan discards printable KEYDOWN events: it expects
	// TEXTINPUT, which a freestanding HID bridge must generate itself.
	if (down && key>=32 && key<127 && !(modifiers&(KMOD_CTRL|KMOD_ALT))) {
		SDL_Event text{}; text.type=SDL_TEXTINPUT;
		text.text.text[0]=char(key); SDL_PushEvent(&text);
	}
#endif
}
extern "C" int SDL_PollEvent(SDL_Event *event)
{
	if (!event) return 0;
	memset(event, 0, sizeof(*event));
	pax_stratagus_input_poll();
	// SDL_QUIT is an event, not a permanently nonempty queue. Movie/menu
	// event-draining loops must be able to finish after receiving it.
	static bool quitDelivered;
	if (pax_exit_requested) {
		if (quitDelivered) return 0;
		quitDelivered=true; event->type=SDL_QUIT; return 1;
	}
	quitDelivered=false;
	if (s_event_read == s_event_write) poll_pointer();
	if (s_event_read != s_event_write) {
		*event = s_events[s_event_read++ % 32]; return 1;
	}
	pax_keyboard_event_t key{};
	if (_pax_svc && _pax_svc->input_keyboard_read && _pax_svc->input_keyboard_read(&key)) {
		keyboard_event(event,pax_sdl_key(key.key),key.down);
		return 1;
	}
	pax_gamepad_state_t p{};
	if (_pax_svc && _pax_svc->input_gamepad_read) _pax_svc->input_gamepad_read(&p);
	// The loader also aliases held mouse buttons to gamepad A/B/X. Deliver
	// them once as pointer buttons, not again as Z/X/A keyboard shortcuts.
	if (s_usb_buttons&1) p.values[PAX_INPUT_A]=0;
	if (s_usb_buttons&2) p.values[PAX_INPUT_B]=0;
	if (s_usb_buttons&4) p.values[PAX_INPUT_X]=0;
	Uint32 bits=0; for (int i=0;i<PAX_INPUT_MAX && i<16;++i) if(p.values[i]) bits |= 1u<<i;
	const Uint32 changed = bits ^ s_last_pad;
	if (changed) {
		const int input = __builtin_ctz(changed);
		const bool down = (bits & (1u << input)) != 0;
		s_last_pad ^= 1u << input;
		static const SDL_Keycode keymap[PAX_INPUT_MAX] = {
			SDLK_UP, SDLK_RIGHT, SDLK_DOWN, SDLK_LEFT,
			SDLK_TAB, SDLK_RETURN, SDLK_z, SDLK_x, SDLK_a, SDLK_s,
			SDLK_q, SDLK_w, SDLK_ESCAPE, SDLK_F10
		};
		keyboard_event(event,input < PAX_INPUT_MAX ? keymap[input] : SDLK_UNKNOWN,down);
		return 1;
	}
	return 0;
}
extern "C" Uint32 SDL_GetMouseState(int *x, int *y) { if(x)*x=s_mouse_x; if(y)*y=s_mouse_y; return s_buttons; }

extern "C" int IMG_Init(int flags) { return flags; }
extern "C" void IMG_Quit(void) {}
extern "C" SDL_Surface *IMG_Load_RW(SDL_RWops *, int) { return nullptr; }
extern "C" int IMG_SavePNG(SDL_Surface *, const char *) { return -1; }

static bool pax_read_exact(void *stream, void *buffer, size_t bytes)
{
	return _pax_svc && _pax_svc->file_read &&
		_pax_svc->file_read(buffer, 1, bytes, stream) == bytes;
}

extern "C" const unsigned char _binary_badlands_paxidx_bin_start[]
	__attribute__((weak));
extern "C" const unsigned char _binary_badlands_paxidx_bin_end[]
	__attribute__((weak));

static SDL_Surface *pax_make_indexed_surface(Uint16 width, Uint16 height,
	Uint16 colors, const Uint8 *palette_bytes, const Uint8 *pixels,
	bool embedded)
{
	SDL_Surface *surface = surface_alloc(width, height, 8, 0, 0, 0, 0,
		const_cast<Uint8 *>(pixels), width);
	if (!surface) return nullptr;
	for (unsigned i = 0; i < colors; ++i) {
		surface->format->palette->colors[i] = SDL_Color{
			palette_bytes[i * 4], palette_bytes[i * 4 + 1],
			palette_bytes[i * 4 + 2], palette_bytes[i * 4 + 3]};
	}
	if (embedded) surface->flags |= PAX_SURFACE_EMBEDDED;
	// PNG tRNS is not necessarily palette entry zero (Warcraft fog uses
	// entry 255). An opaque zero must remain drawable, not disappear.
	for (unsigned i = 0; i < colors; ++i) {
		if (!palette_bytes[i * 4 + 3]) {
			SDL_SetColorKey(surface, SDL_TRUE, i);
			break;
		}
	}
	return surface;
}

static SDL_Surface *pax_load_embedded_indexed()
{
	if (!_binary_badlands_paxidx_bin_start || !_binary_badlands_paxidx_bin_end ||
		_binary_badlands_paxidx_bin_end <= _binary_badlands_paxidx_bin_start) return nullptr;
	const Uint8 *blob = _binary_badlands_paxidx_bin_start;
	const size_t blob_size = (size_t)(_binary_badlands_paxidx_bin_end - blob);
	if (blob_size < 16 || memcmp(blob, "PAPPIDX1", 8) != 0) return nullptr;
	const Uint16 width = (Uint16)(blob[8] | (blob[9] << 8));
	const Uint16 height = (Uint16)(blob[10] | (blob[11] << 8));
	const Uint16 colors = (Uint16)(blob[12] | (blob[13] << 8));
	const size_t palette_size = (size_t)colors * 4;
	const size_t pixel_size = (size_t)width * height;
	if (!width || !height || colors == 0 || colors > 256 ||
		16 + palette_size + pixel_size > blob_size) return nullptr;
	return pax_make_indexed_surface(width, height, colors, blob + 16,
		blob + 16 + palette_size, true);
}

static unsigned le16(const Uint8 *p) { return p[0] | (p[1] << 8); }
static unsigned le32(const Uint8 *p) { return le16(p) | (le16(p+2) << 16); }
extern "C" const unsigned char _binary_stargus_terrain_bin_start[] __attribute__((weak));
extern "C" const unsigned char _binary_stargus_terrain_bin_end[] __attribute__((weak));
static SDL_Surface *pax_load_terrain(const char *path) {
	const char *name=path?strstr(path,"/graphics/"):nullptr;
	if (!name) return nullptr;
	// The SD companion holds both lossless atlases; load only this mission's
	// payload before map allocation instead of keeping both in the PAX image.
	if (!_binary_stargus_terrain_bin_start || !_binary_stargus_terrain_bin_end) {
		FILE *file=fopen(PAX_STRATAGUS_DATA_ROOT "/media/terrain.paxtile","rb");
		if (!file) return nullptr;
		Uint8 header[12]; SDL_Surface *result=nullptr;
		if (fread(header,1,12,file)==12 && !memcmp(header,"PAPTILE1",8) && le32(header+8)<=2) {
			for (unsigned i=0;i<le32(header+8);++i) {
				Uint8 meta[6]; char candidate[128]{};
				if (fread(meta,1,6,file)!=6) break;
				const unsigned n=le16(meta),bytes=le32(meta+2);
				if (!n || n>=sizeof(candidate) || bytes>8*1024*1024 || fread(candidate,1,n,file)!=n) break;
				if (strcmp(candidate,name+10)) { if (fseek(file,bytes,SEEK_CUR)) break; continue; }
				Uint8 *data=(Uint8 *)palloc(bytes);
				if (!data) break;
				if (fread(data,1,bytes,file)==bytes && bytes>=1036) {
					const unsigned w=le16(data),h=le16(data+2),frames=le32(data+4),colors=le16(data+8);
					const Uint8 *lookup=data+12+1024;
					bool valid=w==256 && h && h%32==0 && frames && frames<=100000 && colors==256 &&
						le16(data+10)==32 && 12+1024+frames*2+w*h==bytes;
					if (valid) for (unsigned f=0;f<frames;++f) if (le16(lookup+f*2)>=w*h/1024) { valid=false; break; }
					if (valid) for (auto &entry:s_terrain_frames) if (!entry.surface) {
						result=pax_make_indexed_surface(w,h,colors,data+12,lookup+frames*2,true);
						if (result) entry={result,lookup,frames,data}; break;
					}
				}
				if (!result) pfree(data);
				break;
			}
		}
		fclose(file);
		if (result && _pax_svc->log_printf) _pax_svc->log_printf("Stargus: SD terrain loaded %s\n",name+10);
		return result;
	}
	name+=10; const Uint8 *p=_binary_stargus_terrain_bin_start,*end=_binary_stargus_terrain_bin_end;
	if (end-p<12 || memcmp(p,"PAPTILE1",8)) return nullptr;
	const unsigned count=le32(p+8); p+=12;
	for (unsigned i=0;i<count && end-p>=6;++i) {
		const unsigned n=le16(p),bytes=le32(p+2); p+=6;
		if (n>unsigned(end-p) || bytes>unsigned(end-p-n)) return nullptr;
		if (strlen(name)==n && !memcmp(p,name,n)) {
			const Uint8 *data=p+n; if (bytes<12) return nullptr;
			const unsigned w=le16(data),h=le16(data+2),frames=le32(data+4),colors=le16(data+8);
			if (w!=256 || !h || h%32 || !frames || frames>100000 || colors!=256 || le16(data+10)!=32 ||
				12+colors*4+frames*2+w*h!=bytes) return nullptr;
			const Uint8 *lookup=data+12+colors*4;
			for (unsigned frame=0;frame<frames;++frame) if (le16(lookup+frame*2)>=w*h/1024) return nullptr;
			for (auto &entry:s_terrain_frames) if (!entry.surface) {
				SDL_Surface *s=pax_make_indexed_surface(w,h,colors,data+12,lookup+frames*2,true);
				if (!s) return nullptr; entry={s,lookup,frames}; return s;
			}
			return nullptr;
		}
		p+=n+bytes;
	}
	return nullptr;
}
#include "pax_asset_pack.h"
static SDL_Surface *pax_load_runtime_asset(const char *path)
{
    const char *name = path ? strstr(path, "/graphics/") : nullptr;
    if (!name) return nullptr;
    FILE *file = fopen(PAX_STRATAGUS_DATA_ROOT "/media/runtime.paxassets", "rb");
    if (!file) return nullptr;
    PaxIndexedAsset asset;
    const bool ok = pax_read_asset_pack(file, name + 10, palloc, pfree, asset);
    fclose(file);
    if (!ok) return nullptr;
    SDL_Surface *surface = pax_make_indexed_surface(asset.width, asset.height,
        asset.colors, asset.palette, asset.pixels, false);
    if (!surface) pfree(asset.pixels);
    return surface;
}

static SDL_Surface *pax_load_indexed_companion(const char *path)
{
	if (!_pax_svc || !_pax_svc->file_open || !_pax_svc->file_read ||
		!_pax_svc->file_close || !path) return nullptr;
	char *companion = (char *)palloc(512);
	if (!companion) return nullptr;
	const size_t path_len = strlen(path);
	if (path_len + 8 >= 512) { pfree(companion); return nullptr; }
	memcpy(companion, path, path_len + 1);
	char *suffix = strrchr(companion, '.');
	if (!suffix) { pfree(companion); return nullptr; }
	strcpy(suffix, ".paxidx");
	void *stream = _pax_svc->file_open(companion, "rb");
	pfree(companion);
	if (!stream) return nullptr;

	Uint8 header[16];
	if (!pax_read_exact(stream, header, sizeof(header)) ||
		memcmp(header, "PAPPIDX1", 8) != 0) {
		_pax_svc->file_close(stream);
		return nullptr;
	}
	const Uint16 width = (Uint16)(header[8] | (header[9] << 8));
	const Uint16 height = (Uint16)(header[10] | (header[11] << 8));
	const Uint16 colors = (Uint16)(header[12] | (header[13] << 8));
	if (!width || !height || colors == 0 || colors > 256) {
		_pax_svc->file_close(stream);
		return nullptr;
	}

	Uint8 *packed_palette = (Uint8 *)palloc(256 * 4);
	if (!packed_palette || !pax_read_exact(stream, packed_palette, (size_t)colors * 4)) {
		pfree(packed_palette);
		_pax_svc->file_close(stream);
		return nullptr;
	}

	const size_t pixel_bytes = (size_t)width * height;
	Uint8 *pixels = (Uint8 *)palloc(pixel_bytes);
	if (!pixels || !pax_read_exact(stream, pixels, pixel_bytes)) {
		pfree(pixels);
		pfree(packed_palette);
		_pax_svc->file_close(stream);
		return nullptr;
	}
	_pax_svc->file_close(stream);

	SDL_Surface *surface = pax_make_indexed_surface(width, height, colors,
		packed_palette, pixels, false);
	if (!surface) {
		pfree(pixels);
		pfree(packed_palette);
		return nullptr;
	}
	pfree(packed_palette);
	return surface;
}

extern "C" SDL_Surface *IMG_Load(const char *path)
{
	if (!_pax_svc || !_pax_svc->png_load_rgb565) return nullptr;
	if (!path) return nullptr;
	// FatFs does not reliably resolve the desktop game's graphics/../graphics
	// briefing paths. Canonicalize lexically, without requiring a host filesystem.
	const std::string normalized=std::filesystem::path(path).lexically_normal().string();
	path=normalized.c_str();
	if (SDL_Surface *terrain=pax_load_terrain(path)) return terrain;
	if (SDL_Surface *asset = pax_load_runtime_asset(path)) return asset;
	const char *basename = path ? strrchr(path, '/') : nullptr;
	basename = basename ? basename + 1 : path;
	/* The embedded indexed fallback is only for the large badlands tileset.
	 * Applying it to every PNG makes cmdicons, portraits, and HUD art look
	 * like a 512x9696 terrain sheet and eventually overruns their frame maps. */
	if (basename && strcmp(basename, "badlands.png") == 0) {
		if (SDL_Surface *embedded = pax_load_embedded_indexed()) return embedded;
	}
	if (SDL_Surface *indexed = pax_load_indexed_companion(path)) return indexed;
	if (SDL_Surface *indexed = pax_load_indexed_png(path)) return indexed;
	Uint16 w=0,h=0; Uint16 *src=_pax_svc->png_load_rgb565(path,&w,&h); if(!src)return nullptr;
	/* Keep large tilesets in their native 16-bit form. A 512x9696 sheet is
	 * about 10 MiB in RGB565 but almost 20 MiB as ARGB8888, which is too much
	 * temporary PSRAM pressure for the PAX loader. */
	for (int i = 0; i < w * h; ++i) {
		/* png_load_rgb565 returns the launcher's panel-oriented RGB565:
		 * byte-swapped and with red/blue exchanged. Undo those transforms. */
		Uint16 p = src[i];
		p = (Uint16)(((p & 0x001f) << 11) | (p & 0x07e0) |
			((p >> 11) & 0x001f));
		 src[i] = (Uint16)((p >> 8) | (p << 8));
	}
	SDL_Surface *s=SDL_CreateRGBSurfaceFrom(src,w,h,16,w*2,0xf800,0x07e0,0x001f,0);
	if (!s) _pax_svc->mem_free(src);
	return s;
}

extern "C" int Mix_OpenAudio(int, Uint16, int, int) { return 0; }
extern "C" int Mix_Init(int flags) { return flags; }
extern "C" void Mix_Quit(void) {}
extern "C" int Mix_AllocateChannels(int n) { return n < 0 ? 0 : n; }
extern "C" int Mix_Volume(int, int volume) { return volume; }
extern "C" int Mix_VolumeMusic(int volume) { return volume; }
extern "C" int Mix_SetPanning(int, Uint8, Uint8) { return 1; }
extern "C" void Mix_Resume(int) {}
extern "C" void Mix_ResumeMusic(void) {}
extern "C" int Mix_Playing(int) { return 0; }
extern "C" int Mix_PlayingMusic(void) { return 0; }
extern "C" Mix_Chunk *Mix_GetChunk(int) { return nullptr; }
extern "C" int Mix_GetNumChunkDecoders(void) { return 0; }
extern "C" const char *Mix_GetChunkDecoder(int) { return nullptr; }
extern "C" int Mix_GetNumMusicDecoders(void) { return 0; }
extern "C" const char *Mix_GetMusicDecoder(int) { return nullptr; }
extern "C" void Mix_HookMusicFinished(void (*)(void)) {}
extern "C" void Mix_ChannelFinished(void (*)(int)) {}
extern "C" Mix_Chunk *Mix_LoadWAV(const char *) { return nullptr; }
extern "C" Mix_Music *Mix_LoadMUS_RW(SDL_RWops *, int) { return nullptr; }
extern "C" void Mix_CloseAudio(void) {}
extern "C" int Mix_PlayChannel(int, Mix_Chunk *, int) { return 0; }
extern "C" int Mix_HaltChannel(int) { return 0; }
extern "C" Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *, int) { return nullptr; }
extern "C" Mix_Music *Mix_LoadMUS(const char *) { return nullptr; }
extern "C" int Mix_PlayMusic(Mix_Music *, int) { return 0; }
extern "C" int Mix_HaltMusic(void) { return 0; }
extern "C" void Mix_FreeChunk(Mix_Chunk *) {}
extern "C" void Mix_FreeMusic(Mix_Music *) {}
