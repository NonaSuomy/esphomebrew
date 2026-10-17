#ifndef DESKTOP_RELAY_H
#define DESKTOP_RELAY_H
#include <stddef.h>
#include <stdint.h>
#define RELAY_PORT 5000
#define RELAY_JPEG_LIMIT (1024u * 1024u)
#define RELAY_CURSOR_LIMIT 256u

typedef struct {
  void *context;
  int (*recv)(void *, void *,
              size_t); /* bytes; -2 would block; 0 EOF; -1 error */
  int (*send)(void *, const void *,
              size_t); /* bytes; 0 would block; -1 error */
  void *(*alloc)(void *, size_t);
  void (*free)(void *, void *);
  int64_t (*now)(void *);
  int (*yield)(void *); /* nonzero cancels JPEG decoding */
  void (*audio)(void *, const uint8_t *, size_t, unsigned, unsigned);
} relay_io_t;
typedef struct {
  relay_io_t io;
  uint16_t *pixels;
  unsigned width, height;
  uint8_t header[16], *payload, *cursor;
  void *jpeg_work;
  unsigned header_used, payload_used, payload_size;
  uint8_t outgoing[4096];
  unsigned out_begin, out_end;
  unsigned input_enabled, stats_enabled, frame_ready, changed;
  unsigned cursor_w, cursor_h, hot_x, hot_y, pointer_x, pointer_y;
  unsigned frames, skipped, bytes, audio_bytes, decode_ms;
  unsigned picture_received;
  unsigned modifiers;
  int64_t last_rx, last_stats;
  unsigned wire_mode; /* 0 detection, 1 udisp, 2 AVI */
  unsigned avi_need, avi_depth, avi_streams, avi_current, avi_video, avi_audio;
  unsigned avi_kind[2], avi_lists[8], avi_movi, avi_retain;
  unsigned avi_chunk_active, avi_chunk_pad, avi_pad, avi_width, avi_height;
  unsigned avi_channels, avi_rate, avi_period, avi_started, avi_frame_count;
  unsigned avi_eof, avi_ending, avi_stop, avi_paused;
  uint8_t avi_id[4];
  uint64_t avi_ends[8], avi_offset, avi_samples;
  int64_t avi_epoch, avi_next, avi_pause_at;
  const char *error;
} relay_t;
int relay_open(relay_t *, relay_io_t, uint16_t *, unsigned, unsigned);
int relay_open_format(relay_t *, relay_io_t, uint16_t *, unsigned, unsigned,
                      unsigned rate, unsigned channels);
void relay_close(relay_t *);
int relay_step(relay_t *); /* 0 running, 1 media finished/stopped, -1 error */
void relay_media_pause(relay_t *, int paused);
void relay_media_stop(relay_t *);
int relay_touch(relay_t *, const int *ids, const int *xs, const int *ys,
                unsigned count);
int relay_mouse(relay_t *, int dx, int dy, int wheel, unsigned buttons);
int relay_key(relay_t *, int key, int down);
void relay_draw_cursor(const relay_t *, uint16_t *canvas);
int relay_jpeg(relay_t *, const uint8_t *, size_t, unsigned, unsigned, unsigned,
               unsigned);
#endif
