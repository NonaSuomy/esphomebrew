#pragma once
#include <stddef.h>
#include <stdint.h>

// Packed 16-bit channels must not be interpolated as three separate bytes.
// Preserve the packed pixel, respect source pitch, and never touch padding.
inline void pax_resize16(uint8_t *dst, int width, int height,
                          const uint8_t *src, int source_width,
                          int source_height, size_t source_pitch)
{
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = src + (int64_t(y) * source_height / height) * source_pitch;
        for (int x = 0; x < width; ++x) {
            const size_t from = size_t(int64_t(x) * source_width / width) * 2;
            *dst++ = row[from];
            *dst++ = row[from + 1];
        }
    }
}
