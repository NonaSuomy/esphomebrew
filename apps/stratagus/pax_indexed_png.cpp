#define PAX_APP_SIDE 1
#include "pax_platform.h"
#include <SDL.h>
#include <zlib.h>
#include <stdint.h>
#include <string.h>

static voidpf png_alloc(voidpf, uInt n, uInt size)
{
	const size_t bytes = (size_t)n * size;
	void *p = _pax_svc->mem_caps_alloc(bytes, PAX_MEM_CAP_SPIRAM);
	return p ? p : _pax_svc->mem_alloc(bytes);
}

static void png_free(voidpf, voidpf p) { if (p) _pax_svc->mem_free(p); }
static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static bool read_exact(void *file, void *data, size_t bytes)
{
	return _pax_svc->file_read(data, 1, bytes, file) == bytes;
}
static uint8_t paeth(int a, int b, int c)
{
	const int p = a + b - c;
	const int pa = p > a ? p - a : a - p;
	const int pb = p > b ? p - b : b - p;
	const int pc = p > c ? p - c : c - p;
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

SDL_Surface *pax_load_indexed_png(const char *path)
{
	if (!_pax_svc || !_pax_svc->file_open || !path) return nullptr;
	void *file = _pax_svc->file_open(path, "rb");
	if (!file) return nullptr;
	SDL_Surface *surface = nullptr;
	uint8_t *row = nullptr;
	uint8_t *previous = nullptr;
	unsigned channels = 1, row_bytes = 0, bit_depth = 8;
	z_stream z{};
	z.zalloc = png_alloc;
	z.zfree = png_free;
	bool initialized = false, complete = false, valid = false;
	unsigned y = 0;
	uint8_t signature[8];
	// Image loads run synchronously on the PAX worker. Keep the inflate input
	// scratch in this app's BSS rather than consuming half its 8 KiB stack.
	static uint8_t buffer[4096];
	const uint8_t expected[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	if (!read_exact(file, signature, 8) || memcmp(signature, expected, 8)) goto done;
	for (;;) {
		uint8_t header[8], crc_bytes[4];
		if (!read_exact(file, header, 8)) break;
		const uint32_t length = be32(header);
		const uint8_t *kind = header + 4;
		uLong crc = crc32(0, kind, 4);
		if (length > 32u * 1024u * 1024u) break;
		uint32_t left = length;
		if (!memcmp(kind, "IHDR", 4)) {
			uint8_t ihdr[13];
			if (surface || length != 13 || !read_exact(file, ihdr, 13)) break;
			crc = crc32(crc, ihdr, 13);
			left = 0;
			const uint32_t w = be32(ihdr), h = be32(ihdr + 4);
			bit_depth = ihdr[8];
			const bool indexed = ihdr[9] == 3;
			const bool supported_depth = indexed ?
				(bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8) : bit_depth == 8;
			if (!w || !h || w > 65535 || h > 65535 || w * h > 16u * 1024u * 1024u ||
				!supported_depth || (ihdr[9] != 3 && ihdr[9] != 2 && ihdr[9] != 6) ||
				ihdr[10] || ihdr[11] || ihdr[12]) break;
			channels = ihdr[9] == 3 ? 1 : ihdr[9] == 2 ? 3 : 4;
			row_bytes = indexed ? (w * bit_depth + 7) / 8 : w * channels;
			surface = channels == 1 ? SDL_CreateRGBSurface(0, w, h, 8, 0, 0, 0, 0) :
				SDL_CreateRGBSurface(0, w, h, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
			row = (uint8_t *)png_alloc(nullptr, 1, row_bytes + 1);
			previous = (uint8_t *)png_alloc(nullptr, 1, row_bytes);
			if (!surface || !row || !previous) break;
			memset(previous, 0, row_bytes);
			if (channels == 1)
				for (int i = 0; i < 256; ++i) surface->format->palette->colors[i].a = 255;
		} else if (!memcmp(kind, "PLTE", 4)) {
			if (!surface || channels != 1 || initialized || !length || length > 768 || length % 3 ||
				!read_exact(file, buffer, length)) break;
			crc = crc32(crc, buffer, length);
			left = 0;
			for (unsigned i = 0; i < length / 3; ++i)
				surface->format->palette->colors[i] = SDL_Color{buffer[i*3], buffer[i*3+1], buffer[i*3+2], 255};
		} else if (!memcmp(kind, "tRNS", 4)) {
			if (!surface || channels != 1 || initialized || length > 256 || !read_exact(file, buffer, length)) break;
			crc = crc32(crc, buffer, length);
			left = 0;
			for (unsigned i = 0; i < length; ++i) {
				surface->format->palette->colors[i].a = buffer[i];
				if (buffer[i] == 0) SDL_SetColorKey(surface, SDL_TRUE, i);
			}
		} else if (!memcmp(kind, "IDAT", 4)) {
			if (!surface || complete) break;
			if (!initialized) {
				if (inflateInit(&z) != Z_OK) break;
				initialized = true;
				z.next_out = row;
				z.avail_out = row_bytes + 1;
			}
			while (left) {
				const unsigned count = left > sizeof(buffer) ? sizeof(buffer) : left;
				if (!read_exact(file, buffer, count)) goto done;
				crc = crc32(crc, buffer, count);
				left -= count;
				z.next_in = buffer;
				z.avail_in = count;
				while (z.avail_in && !complete) {
					const int result = inflate(&z, Z_NO_FLUSH);
					if (result != Z_OK && result != Z_STREAM_END) goto done;
					if (!z.avail_out) {
						if (y >= (unsigned)surface->h || row[0] > 4) goto done;
						uint8_t *dst = (uint8_t *)surface->pixels + y * surface->pitch;
						for (unsigned x = 0; x < row_bytes; ++x) {
							const int a = x >= channels ? row[1+x-channels] : 0, b = previous[x];
							const int c = x >= channels ? previous[x-channels] : 0;
							const int filter = row[0] == 1 ? a : row[0] == 2 ? b :
								row[0] == 3 ? (a+b)/2 : row[0] == 4 ? paeth(a,b,c) : 0;
							row[x+1] += filter;
						}
						if (channels == 1 && bit_depth == 8) memcpy(dst, row + 1, row_bytes);
						else if (channels == 1) {
							// PNG packs palette indices most-significant first. SDL/Stratagus
							// requires one byte per index even for 1/2/4-bit source fonts.
							const unsigned mask = (1u << bit_depth) - 1;
							for (unsigned x = 0; x < (unsigned)surface->w; ++x) {
								const unsigned bit = x * bit_depth;
								dst[x] = (row[1 + bit / 8] >> (8 - bit_depth - bit % 8)) & mask;
							}
						}
						else for (int x = 0; x < surface->w; ++x) {
							const uint8_t *p = row + 1 + x * channels;
							((uint32_t *)dst)[x] = (uint32_t)(channels == 4 ? p[3] : 255) << 24 |
								(uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
						}
						memcpy(previous, row + 1, row_bytes);
						++y;
						z.next_out = row;
						z.avail_out = row_bytes + 1;
					}
					if (result == Z_STREAM_END) complete = true;
				}
				if (z.avail_in) goto done;
			}
		}
		// Skip ancillary chunks using the same bounded buffer, checking CRC.
		while (left) {
			const unsigned count = left > sizeof(buffer) ? sizeof(buffer) : left;
			if (!read_exact(file, buffer, count)) goto done;
			crc = crc32(crc, buffer, count);
			left -= count;
		}
		if (!read_exact(file, crc_bytes, 4) || crc != be32(crc_bytes)) break;
		if (!memcmp(kind, "IEND", 4)) {
			valid = surface && complete && y == (unsigned)surface->h && z.avail_out == row_bytes + 1;
			break;
		}
	}
done:
	if (initialized) inflateEnd(&z);
	png_free(nullptr, row);
	png_free(nullptr, previous);
	_pax_svc->file_close(file);
	if (!valid) { SDL_FreeSurface(surface); return nullptr; }
	return surface;
}
