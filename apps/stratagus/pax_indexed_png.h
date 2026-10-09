#pragma once

struct SDL_Surface;

// Load non-interlaced 1/2/4/8-bit palette PNGs as one-byte SDL indices,
// retaining font/player colors; also supports 8-bit RGB/RGBA PNGs.
// The caller owns the returned surface. Unsupported formats use the fallback.
SDL_Surface *pax_load_indexed_png(const char *path);
