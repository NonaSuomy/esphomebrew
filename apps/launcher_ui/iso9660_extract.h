#ifndef ESPHOMEBREW_ISO9660_EXTRACT_H
#define ESPHOMEBREW_ISO9660_EXTRACT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  void *context;
  int (*seek)(void *context, long offset, int origin);
  long (*tell)(void *context);
  size_t (*read)(void *ptr, size_t size, size_t count, void *context);
  size_t (*write)(const void *ptr, size_t size, size_t count, void *context);
} iso9660_io_t;

/* `track_offset` points to the first byte of the selected data track in a BIN.
 * A plain ISO uses {0, 2048, 0}; Mode 1 raw BIN uses {track_start, 2352, 16}. */
typedef struct {
  uint64_t track_offset;
  uint16_t sector_size;
  uint16_t user_data_offset;
} iso9660_layout_t;

typedef void (*iso9660_progress_fn)(void *context, uint64_t copied_bytes,
                                    uint64_t total_bytes);

/* Copies one explicit ISO9660 path from an open image to an open output stream.
 * Supports primary-volume ISO9660 names (including ;version suffixes),
 * directories, plain 2048-byte ISO images, and Mode 1/2352 BIN tracks.
 * Does not overwrite or create output paths; the caller owns atomic staging. */
int iso9660_copy_file(const iso9660_io_t *input_io,
                      const iso9660_io_t *output_io, const char *source_path,
                      const iso9660_layout_t *layout, long image_size,
                      uint64_t max_output_bytes, uint64_t *copied_bytes);

/* Same copy operation with optional per-buffer progress reporting. */
int iso9660_copy_file_with_progress(const iso9660_io_t *input_io,
                                    const iso9660_io_t *output_io,
                                    const char *source_path,
                                    const iso9660_layout_t *layout,
                                    long image_size,
                                    uint64_t max_output_bytes,
                                    uint64_t *copied_bytes,
                                    uint64_t *source_bytes,
                                    iso9660_progress_fn progress,
                                    void *progress_context);

#endif
