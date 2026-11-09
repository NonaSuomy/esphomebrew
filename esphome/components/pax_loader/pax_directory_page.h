#pragma once
#include <stdint.h>
/* Optional extension of ABI-1 file_list_dir. Request path:
 * @pax-dir-v1:<raw-entry-offset>:/sd/roms/pax/
 * Response starts with this header, followed by count NUL-terminated names.
 * Directory names retain the old '/' suffix. No handles survive a call.
 * Refresh restarts at offset zero; concurrent directory changes may require
 * refresh, as with readdir itself. Older hosts reject the request normally. */
#define PAX_DIRECTORY_PREFIX "@pax-dir-v1:"
#define PAX_DIRECTORY_MAGIC UINT32_C(0x31445050)
#define PAX_DIRECTORY_PAGE_ENTRIES 32
struct pax_directory_page {
  uint32_t magic, next_offset, count, bytes, more;
};
