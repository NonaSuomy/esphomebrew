#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct PaxIndexedAsset {
    uint16_t width{}, height{}, colors{};
    unsigned char palette[1024]{};
    unsigned char *pixels{};
};

// Read only the selected indexed sprite. Surfaces own the returned pixels;
// the pack is never kept resident or mapped into executable memory.
inline bool pax_read_asset_pack(FILE *file, const char *name,
    void *(*allocate)(size_t), void (*release)(void *), PaxIndexedAsset &out)
{
    if (!file || !name || !allocate || !release) return false;
    if (fseek(file, 0, SEEK_END)) return false;
    const long length = ftell(file);
    if (length < 12 || length > 32 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) return false;
    unsigned char header[12];
    if (fread(header, 1, 12, file) != 12 || memcmp(header, "PAPASST1", 8)) return false;
    auto le16 = [](const unsigned char *p) { return unsigned(p[0]) | (unsigned(p[1]) << 8); };
    auto le32 = [le16](const unsigned char *p) { return le16(p) | (uint32_t(le16(p + 2)) << 16); };
    const unsigned count = le32(header + 8);
    if (!count || count > 128) return false;
    for (unsigned i = 0; i < count; ++i) {
        unsigned char meta[6];
        if (fread(meta, 1, 6, file) != 6) return false;
        const unsigned namesize = le16(meta), bytes = le32(meta + 2);
        const long position = ftell(file);
        if (position < 0 || namesize == 0 || namesize > 255 || bytes < 6 ||
            bytes > 8 * 1024 * 1024 || uint64_t(position) + namesize + bytes > uint64_t(length)) return false;
        char candidate[256];
        if (fread(candidate, 1, namesize, file) != namesize) return false;
        if (strlen(name) != namesize || memcmp(name, candidate, namesize)) {
            if (fseek(file, bytes, SEEK_CUR)) return false;
            continue;
        }
        unsigned char shape[6];
        if (fread(shape, 1, 6, file) != 6) return false;
        const unsigned width = le16(shape), height = le16(shape + 2), colors = le16(shape + 4);
        if (!width || !height || !colors || colors > 256 ||
            uint64_t(width) * height + 6 + colors * 4 != bytes) return false;
        if (fread(out.palette, 4, colors, file) != colors) return false;
        auto *pixels = static_cast<unsigned char *>(allocate(size_t(width) * height));
        if (!pixels) return false;
        if (fread(pixels, 1, size_t(width) * height, file) != size_t(width) * height) {
            release(pixels);
            return false;
        }
        out.width = width; out.height = height; out.colors = colors; out.pixels = pixels;
        return true;
    }
    return false;
}
