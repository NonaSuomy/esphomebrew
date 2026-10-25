#include "pax_data.h"
#include <stdio.h>
#include <string.h>

#ifndef PAX_AW_DATA_DIR
#define PAX_AW_DATA_DIR "/sd/roms/anotherworld"
#endif

unsigned char gb_memlist_bin[3000];
unsigned int gb_memlist_bin_size;

static unsigned be16(const unsigned char *p) { return (unsigned(p[0]) << 8) | p[1]; }

bool pax_aw_validate_memlist(const unsigned char *data, size_t bytes)
{
    // The VM owns 150 entries, including the end marker. Reject incomplete
    // tables and bad sizes before the legacy core can index its fixed arrays.
    if (!data || !bytes || bytes > sizeof(gb_memlist_bin)) return false;
    for (size_t offset = 0, count = 0; count < 150 && offset < bytes; ++count, offset += 20) {
        if (data[offset] == 0xff) return count > 0;
        if (bytes - offset < 20 || data[offset + 1] > 6) return false;
        const unsigned packed = be16(data + offset + 14), size = be16(data + offset + 18);
        if ((size == 0) != (packed == 0) || packed > size || (!data[offset + 7] && packed)) return false;
    }
    return false;
}

bool pax_aw_load_memlist()
{
    gb_memlist_bin_size = 0;
    FILE *file = fopen(PAX_AW_DATA_DIR "/memlist.bin", "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return false; }
    const long length = ftell(file);
    if (length <= 0 || length > (long)sizeof(gb_memlist_bin) || fseek(file, 0, SEEK_SET)) {
        fclose(file); return false;
    }
    const size_t bytes = fread(gb_memlist_bin, 1, (size_t)length, file);
    fclose(file);
    if (bytes != (size_t)length || !pax_aw_validate_memlist(gb_memlist_bin, bytes)) return false;
    gb_memlist_bin_size = (unsigned)bytes;
    return true;
}

bool pax_aw_read_bank(unsigned bank, uint32_t offset, void *buffer, size_t bytes)
{
    if (!bank || bank > 255 || !buffer || !bytes || offset > 0x7fffffffU || bytes > 65535) return false;
    char path[64];
    snprintf(path, sizeof(path), PAX_AW_DATA_DIR "/bank%02x", bank);
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    const bool ok = !fseek(file, (long)offset, SEEK_SET) && fread(buffer, 1, bytes, file) == bytes;
    fclose(file);
    return ok;
}
