#include "iso9660_extract.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#define ISO_SECTOR_BYTES 2048U
#define ISO_MAX_PATH_BYTES 256U
#define ISO_MAX_SEGMENT_BYTES 64U
#define ISO_COPY_BUFFER_BYTES 8192U

/* One PAX performs one disc extraction at a time. Keep the I/O scratch space
 * in the app's zeroed BSS (PSRAM), not on the loader's 8 KiB worker stack. */
static uint8_t iso_copy_buffer[ISO_COPY_BUFFER_BYTES];
static uint8_t iso_volume_descriptor[ISO_SECTOR_BYTES];

typedef struct {
  uint32_t extent;
  uint32_t size;
  bool directory;
  bool multi_extent;
} iso_entry_t;

static uint32_t iso_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *iso_find_slash(const char *text)
{
  if (!text) return NULL;
  for (const char *p = text; *p != '\0'; ++p)
    if (*p == '/') return p;
  return NULL;
}

static unsigned char iso_ascii_upper(unsigned char c)
{
  return (c >= 'a' && c <= 'z') ? (unsigned char)(c - ('a' - 'A')) : c;
}

static bool iso_offset(const iso9660_layout_t *layout, uint32_t lba,
                       uint64_t byte_in_sector, uint64_t *out)
{
  if (!layout || !out || layout->sector_size < ISO_SECTOR_BYTES ||
      layout->user_data_offset + ISO_SECTOR_BYTES > layout->sector_size)
    return false;
  const uint64_t lba_offset = (uint64_t)lba * layout->sector_size;
  if (lba_offset > UINT64_MAX - layout->track_offset -
                   layout->user_data_offset - byte_in_sector)
    return false;
  *out = layout->track_offset + lba_offset + layout->user_data_offset + byte_in_sector;
  return *out <= (uint64_t)LONG_MAX;
}

static bool iso_read_at(const iso9660_io_t *io, const iso9660_layout_t *layout,
                        uint32_t lba, uint64_t within_sector,
                        void *buffer, size_t bytes)
{
  uint64_t offset;
  if (!io || !io->seek || !io->read || !iso_offset(layout, lba, within_sector, &offset) ||
      io->seek(io->context, (long)offset, 0) != 0)
    return false;
  return io->read(buffer, 1, bytes, io->context) == bytes;
}

static bool iso_safe_source_path(const char *path)
{
  if (!path || !path[0] || path[0] == '/' || path[0] == '\\') return false;
  size_t segment_length = 0;
  bool segment_dot = true;
  bool segment_dotdot = true;
  for (const char *p = path; ; ++p) {
    const char c = *p;
    if (c == '\\') return false;
    if (c == '/' || c == '\0') {
      if (segment_length == 0 || segment_dot || segment_dotdot) return false;
      if (c == '\0') return true;
      segment_length = 0;
      segment_dot = true;
      segment_dotdot = true;
      continue;
    }
    if (segment_length >= ISO_MAX_SEGMENT_BYTES) return false;
    if (segment_length == 0) segment_dot = c == '.';
    else segment_dot = false;
    if (segment_length == 0) segment_dotdot = c == '.';
    else if (segment_length == 1) segment_dotdot = segment_dotdot && c == '.';
    else segment_dotdot = false;
    segment_length++;
  }
}

static bool iso_find_primary_descriptor(const iso9660_io_t *io,
                                        const iso9660_layout_t *layout,
                                        long image_size, uint8_t *pvd)
{
  for (uint32_t lba = 16; lba < 80; ++lba) {
    uint64_t offset;
    if (!iso_offset(layout, lba, 0, &offset) ||
        offset > (uint64_t)image_size ||
        sizeof(iso_volume_descriptor) > (uint64_t)image_size - offset) return false;
    if (!iso_read_at(io, layout, lba, 0, iso_volume_descriptor,
                     sizeof(iso_volume_descriptor))) return false;
    if (memcmp(iso_volume_descriptor + 1, "CD001", 5) != 0 ||
        iso_volume_descriptor[6] != 1) return false;
    if (iso_volume_descriptor[0] == 1) {
      memcpy(pvd, iso_volume_descriptor, sizeof(iso_volume_descriptor));
      return true;
    }
    if (iso_volume_descriptor[0] == 255) return false;
  }
  return false;
}

static bool iso_name_matches(const uint8_t *raw, size_t raw_length,
                             const char *wanted)
{
  if (!raw || !wanted || raw_length == 0) return false;
  size_t name_length = raw_length;
  for (size_t i = 0; i < raw_length; ++i) {
    if (raw[i] == ';') { name_length = i; break; }
  }
  while (name_length > 0 && raw[name_length - 1] == '.') name_length--;
  size_t wanted_length = strlen(wanted);
  for (size_t i = 0; i < wanted_length; ++i)
    if (wanted[i] == ';') { wanted_length = i; break; }
  while (wanted_length > 0 && wanted[wanted_length - 1] == '.') wanted_length--;
  if (name_length != wanted_length) return false;
  for (size_t i = 0; i < name_length; ++i)
    if (iso_ascii_upper(raw[i]) != iso_ascii_upper((unsigned char)wanted[i])) return false;
  return true;
}

static bool iso_find_child(const iso9660_io_t *io, const iso9660_layout_t *layout,
                           const iso_entry_t *directory, const char *wanted,
                           iso_entry_t *found)
{
  uint32_t cursor = 0;
  uint8_t record[256];
  while (cursor < directory->size) {
    uint64_t offset;
    if (!iso_offset(layout, directory->extent + cursor / ISO_SECTOR_BYTES,
                    cursor % ISO_SECTOR_BYTES, &offset) ||
        offset > (uint64_t)LONG_MAX || io->seek(io->context, (long)offset, 0) != 0)
      return false;
    uint8_t record_length = 0;
    if (io->read(&record_length, 1, 1, io->context) != 1) return false;
    if (record_length == 0) {
      const uint32_t next = ((cursor / ISO_SECTOR_BYTES) + 1U) * ISO_SECTOR_BYTES;
      if (next <= cursor) return false;
      cursor = next;
      continue;
    }
    if (record_length < 34 ||
        record_length > ISO_SECTOR_BYTES - (cursor % ISO_SECTOR_BYTES) ||
        record_length > directory->size - cursor) return false;
    record[0] = record_length;
    if (io->read(record + 1, 1, record_length - 1U, io->context) != record_length - 1U)
      return false;
    const uint8_t identifier_length = record[32];
    if ((uint16_t)33U + identifier_length > record_length) return false;
    if (identifier_length > 1 && iso_name_matches(record + 33, identifier_length, wanted)) {
      found->extent = iso_le32(record + 2);
      found->size = iso_le32(record + 10);
      found->directory = (record[25] & 0x02U) != 0;
      found->multi_extent = (record[25] & 0x80U) != 0;
      return true;
    }
    cursor += record_length;
  }
  return false;
}

static bool iso_find_path(const iso9660_io_t *io, const iso9660_layout_t *layout,
                          const uint8_t *pvd, const char *path,
                          iso_entry_t *file)
{
  if (pvd[156] < 34 || (pvd[156 + 25] & 0x02U) == 0) return false;
  iso_entry_t current = {
    .extent = iso_le32(pvd + 156 + 2),
    .size = iso_le32(pvd + 156 + 10),
    .directory = true,
  };
  const char *cursor = path;
  char segment[ISO_MAX_SEGMENT_BYTES + 1];
  while (*cursor) {
    const char *slash = iso_find_slash(cursor);
    size_t length = slash ? (size_t)(slash - cursor) : strlen(cursor);
    if (length == 0 || length >= sizeof(segment)) return false;
    memcpy(segment, cursor, length);
    segment[length] = '\0';
    iso_entry_t child = {0};
    if (!current.directory || !iso_find_child(io, layout, &current, segment, &child))
      return false;
    if (child.multi_extent || (slash && !child.directory) || (!slash && child.directory))
      return false;
    current = child;
    if (!slash) break;
    cursor = slash + 1;
  }
  if (*cursor == '\0' && cursor > path && cursor[-1] == '/') return false;
  *file = current;
  return true;
}

int iso9660_copy_file_with_progress(const iso9660_io_t *input_io,
                                    const iso9660_io_t *output_io,
                                    const char *source_path,
                                    const iso9660_layout_t *layout,
                                    long image_size,
                                    uint64_t max_output_bytes,
                                    uint64_t *copied_bytes,
                                    uint64_t *source_bytes,
                                    iso9660_progress_fn progress,
                                    void *progress_context)
{
  if (copied_bytes) *copied_bytes = 0;
  if (source_bytes) *source_bytes = 0;
  if (!input_io || !input_io->seek || !input_io->tell || !input_io->read ||
      !output_io || !output_io->write ||
      !layout || image_size <= 0 || max_output_bytes == 0 ||
      !iso_safe_source_path(source_path) ||
      (layout->sector_size != 2048 && layout->sector_size != 2352) ||
      (layout->sector_size == 2048 && layout->user_data_offset != 0) ||
      (layout->sector_size == 2352 && layout->user_data_offset != 16)) return -1;

  uint8_t *pvd = iso_volume_descriptor;
  if (!iso_find_primary_descriptor(input_io, layout, image_size, pvd)) return -2;
  iso_entry_t entry = {0};
  if (!iso_find_path(input_io, layout, pvd, source_path, &entry)) return -3;
  if (source_bytes) *source_bytes = entry.size;
  if (entry.size == 0) return -4;
  if (entry.size > max_output_bytes) return -7;

  uint64_t offset;
  const uint32_t start_lba = entry.extent;
  if (!iso_offset(layout, start_lba, 0, &offset) ||
      offset > (uint64_t)image_size || entry.size > (uint64_t)image_size - offset)
    return -5;
  if (input_io->seek(input_io->context, (long)offset, 0) != 0) return -5;

  uint8_t *buffer = iso_copy_buffer;
  uint32_t remaining = entry.size;
  uint64_t completed = 0;
  if (progress != NULL) progress(progress_context, 0, entry.size);
  while (remaining > 0) {
    const size_t request = remaining > sizeof(iso_copy_buffer) ? sizeof(iso_copy_buffer) : remaining;
    const size_t read = input_io->read(buffer, 1, request, input_io->context);
    if (read != request || output_io->write(buffer, 1, read, output_io->context) != read)
      return -6;
    remaining -= (uint32_t)read;
    completed += read;
    if (progress != NULL) progress(progress_context, completed, entry.size);
  }
  if (copied_bytes) *copied_bytes = entry.size;
  return 0;
}

int iso9660_copy_file(const iso9660_io_t *input_io,
                      const iso9660_io_t *output_io, const char *source_path,
                      const iso9660_layout_t *layout, long image_size,
                      uint64_t max_output_bytes, uint64_t *copied_bytes)
{
  return iso9660_copy_file_with_progress(input_io, output_io, source_path,
                                         layout, image_size, max_output_bytes,
                                         copied_bytes, NULL, NULL, NULL);
}
