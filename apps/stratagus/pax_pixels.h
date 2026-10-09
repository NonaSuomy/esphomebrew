#pragma once
#include <SDL.h>
#include <stdint.h>

class PaxPaletteCache {
	struct Entry {
		const SDL_Palette *palette{};
		Uint32 version{}, r{}, g{}, b{};
		int depth{};
		Uint32 pixels[256]{};
	} entries[16];
	unsigned next{};
public:
	void invalidate(const SDL_Palette *palette) {
		for (Entry &entry : entries) if (entry.palette==palette) entry.palette=nullptr;
	}
	const Uint32 *get(const SDL_Palette *palette, const SDL_PixelFormat *format) {
		Entry *entry=nullptr;
		for (Entry &cached : entries)
			if (cached.palette==palette && cached.version==palette->version &&
				cached.depth==format->BytesPerPixel && cached.r==format->Rmask &&
				cached.g==format->Gmask && cached.b==format->Bmask) return cached.pixels;
		entry=&entries[next++%16]; entry->palette=palette; entry->version=palette->version;
		entry->depth=format->BytesPerPixel; entry->r=format->Rmask; entry->g=format->Gmask; entry->b=format->Bmask;
		for (unsigned i=0;i<256;++i) {
			const SDL_Color c=i<(unsigned)palette->ncolors ? palette->colors[i] : SDL_Color{0,0,0,0};
			entry->pixels[i]=format->BytesPerPixel==2 ?
				((c.r>>3)<<11)|((c.g>>2)<<5)|(c.b>>3) :
				(((Uint32)c.r<<format->Rshift)&format->Rmask)|
				(((Uint32)c.g<<format->Gshift)&format->Gmask)|
				(((Uint32)c.b<<format->Bshift)&format->Bmask);
		}
		return entry->pixels;
	}
};

template<class Pixel> static inline void pax_indexed_row(
	Pixel *dst, const Uint8 *src, int count, const Uint32 *palette, int key)
{
	if constexpr(sizeof(Pixel)==2) {
		if (((uintptr_t)dst&3)==0) {
			int i=0;
			for (;i+1<count;i+=2) {
				const unsigned a=src[i],b=src[i+1];
				if (key<0 || (a!=(unsigned)key && b!=(unsigned)key)) {
					const Uint32 pixels=Uint16(palette[a]) | (Uint32)Uint16(palette[b])<<16;
					__builtin_memcpy(dst+i,&pixels,4);
				} else {
					if(a!=(unsigned)key) dst[i]=palette[a];
					if(b!=(unsigned)key) dst[i+1]=palette[b];
				}
			}
			if(i<count && (key<0 || src[i]!=key)) dst[i]=palette[src[i]];
			return;
		}
	}
	if (key<0) for (int i=0;i<count;++i) dst[i]=(Pixel)palette[src[i]];
	else for (int i=0;i<count;++i) if (src[i]!=key) dst[i]=(Pixel)palette[src[i]];
}

static inline Uint32 pax_rgb565_to_rgb(Uint16 pixel)
{
	// Exact expansion used by the previous renderer, without per-pixel
	// divides or SDL calls. Tables are only 96 bytes and fit in cache.
	static constexpr Uint8 five[32]={0,8,16,24,32,41,49,57,65,74,82,90,98,106,115,123,
		131,139,148,156,164,172,180,189,197,205,213,222,230,238,246,255};
	static constexpr Uint8 six[64]={0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,
		64,68,72,76,80,85,89,93,97,101,105,109,113,117,121,125,129,133,137,141,
		145,149,153,157,161,165,170,174,178,182,186,190,194,198,202,206,210,214,
		218,222,226,230,234,238,242,246,250,255};
	return (Uint32)five[pixel>>11]<<16 | (Uint32)six[(pixel>>5)&63]<<8 | five[pixel&31];
}

static inline Uint16 pax_rgb_to_rgb565(Uint32 pixel)
{
	return ((pixel>>8)&0xf800)|((pixel>>5)&0x07e0)|((pixel>>3)&0x001f);
}

// Exact channel tables for repeatedly blended fog colours. No per-pixel
// division, approximation, heap allocation, or extra full-frame buffer.
class PaxBlend565Cache {
	struct Entry { Uint32 key{}; bool valid{}; Uint16 r[32]{},g[64]{},b[32]{}; } entries[16];
	unsigned next{},last{};
public:
	const Entry *get(Uint8 r,Uint8 g,Uint8 b,unsigned alpha) {
		const Uint32 key=Uint32(alpha)<<24|Uint32(r)<<16|Uint32(g)<<8|b;
		if (entries[last].valid && entries[last].key==key) return &entries[last];
		for (unsigned i=0;i<16;++i) if (entries[i].valid && entries[i].key==key) { last=i; return &entries[i]; }
		last=next++%16;
		Entry &entry=entries[last]; entry.key=key; entry.valid=true;
		for (unsigned i=0;i<32;++i) {
			const unsigned bg=pax_rgb565_to_rgb(Uint16(i))&255;
			entry.r[i]=Uint16(((r*alpha+bg*(255-alpha))/255)>>3)<<11;
			entry.b[i]=Uint16(((b*alpha+bg*(255-alpha))/255)>>3);
		}
		for (unsigned i=0;i<64;++i) {
			const unsigned bg=(pax_rgb565_to_rgb(Uint16(i<<5))>>8)&255;
			entry.g[i]=Uint16(((g*alpha+bg*(255-alpha))/255)>>2)<<5;
		}
		return &entry;
	}
	static Uint16 blend(const Entry *entry,Uint16 bg) {
		return entry->r[bg>>11]|entry->g[(bg>>5)&63]|entry->b[bg&31];
	}
};

static inline void pax_rgb565_row(Uint16 *dst, const Uint32 *src, int count)
{
	int i=0;
	// Two packed pixels per store. Use the simple path if a clipped output
	// row begins on a halfword rather than a word boundary.
	if (((uintptr_t)dst&3)==0) {
		for (;i+1<count;i+=2) {
			const Uint32 pixels=(Uint32)pax_rgb_to_rgb565(src[i]) |
				(Uint32)pax_rgb_to_rgb565(src[i+1])<<16;
			__builtin_memcpy(dst+i,&pixels,sizeof(pixels));
		}
	}
	for (;i<count;++i) dst[i]=pax_rgb_to_rgb565(src[i]);
}
