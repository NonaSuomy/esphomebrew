#include "relay.h"

static unsigned le16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;
}
static void put16(uint8_t *p, unsigned n) {
  p[0] = n;
  p[1] = n >> 8;
}
static void put32(uint8_t *p, uint32_t n) {
  put16(p, n);
  put16(p + 2, n >> 16);
}
static int fail(relay_t *r, const char *message) {
  r->error = message;
  return -1;
}
static int queue(relay_t *r, const uint8_t *data, unsigned n) {
  if (r->out_begin && r->out_end + n > sizeof r->outgoing) {
    unsigned kept = r->out_end - r->out_begin;
    for (unsigned i = 0; i < kept; i++)
      r->outgoing[i] = r->outgoing[r->out_begin + i];
    r->out_begin = 0;
    r->out_end = kept;
  }
  if (n > sizeof r->outgoing - r->out_end)
    return fail(r, "Relay input queue full");
  for (unsigned i = 0; i < n; i++)
    r->outgoing[r->out_end++] = data[i];
  return 0;
}
static int flush(relay_t *r) {
  if (r->out_begin == r->out_end)
    return 0;
  int n = r->io.send(r->io.context, r->outgoing + r->out_begin,
                     r->out_end - r->out_begin);
  if (n < 0 || (unsigned)n > r->out_end - r->out_begin)
    return fail(r, "Relay connection closed while sending");
  r->out_begin += (unsigned)n;
  if (r->out_begin == r->out_end)
    r->out_begin = r->out_end = 0;
  return 0;
}
int relay_open(relay_t *r, relay_io_t io, uint16_t *pixels, unsigned w,
               unsigned h) {
  return relay_open_format(r, io, pixels, w, h, 48000, 2);
}
int relay_open_format(relay_t *r, relay_io_t io, uint16_t *pixels, unsigned w,
                      unsigned h, unsigned rate, unsigned channels) {
  /* The owning app calls close before reusing a receiver. */
  uint8_t *raw = (uint8_t *)r;
  for (size_t i = 0; i < sizeof *r; i++)
    raw[i] = 0;
  r->io = io;
  r->pixels = pixels;
  r->width = w;
  r->height = h;
  r->error = "";
  r->avi_current = r->avi_video = r->avi_audio = 255;
  r->avi_need = 12;
  if (!w || !h || w > 4096 || h > 4096)
    return fail(r, "Invalid relay canvas size");
  r->payload = io.alloc(io.context, RELAY_JPEG_LIMIT);
  r->cursor = io.alloc(io.context, RELAY_CURSOR_LIMIT * RELAY_CURSOR_LIMIT * 4);
  r->jpeg_work = io.alloc(io.context, 16384);
  if (!r->payload || !r->cursor || !r->jpeg_work) {
    relay_close(r);
    return fail(r, "Not enough memory for relay receiver");
  }
  r->last_rx = r->last_stats = io.now(io.context);
  r->pointer_x = w / 2;
  r->pointer_y = h / 2;
  uint8_t greeting[29] = {'C', 16, 'V', 0, 0,   0,  0, 0,  0, 0,   0, 0,   'A',
                          48,  0,  'N', 2, 'L', 48, 0, 48, 0, 'F', 1, 'S', 1};
  if (rate < 8000 || rate > 48000 || rate % 50 ||
      (channels != 1 && channels != 2)) {
    relay_close(r);
    return fail(r, "Invalid receiver audio format");
  }
  greeting[13] = greeting[18] = greeting[20] = rate / 1000;
  greeting[14] = greeting[19] = greeting[21] = (rate % 1000) / 50;
  greeting[16] = channels;
  put16(greeting + 3, w);
  put16(greeting + 5, h);
  put32(greeting + 8, RELAY_JPEG_LIMIT);
  return queue(r, greeting, 26);
}
void relay_close(relay_t *r) {
  if (r->payload)
    r->io.free(r->io.context, r->payload);
  if (r->cursor)
    r->io.free(r->io.context, r->cursor);
  if (r->jpeg_work)
    r->io.free(r->io.context, r->jpeg_work);
  r->wire_mode = r->input_enabled = r->avi_paused = 0;
  r->payload = r->cursor = 0;
  r->jpeg_work = 0;
}
static int header_check(relay_t *r) {
  unsigned type = r->header[2], w = le16(r->header + 8),
           h = le16(r->header + 10);
  r->payload_size = le32(r->header + 12) >> 10;
  r->payload_used = 0;
  unsigned n = r->payload_size;
  if (type == 0x7e)
    return 0; /* Throughput probe: streamed/discarded, no allocation. */
  if (n > RELAY_JPEG_LIMIT)
    return fail(r, "Relay packet exceeds 1 MB limit");
  if (type == 3) {
    unsigned x = le16(r->header + 4), y = le16(r->header + 6);
    if (!n || !w || !h || x > r->width || y > r->height || w > r->width - x ||
        h > r->height - y)
      return fail(r, "Relay JPEG rectangle outside canvas");
  } else if (type == 0x23) {
    if (w > RELAY_CURSOR_LIMIT || h > RELAY_CURSOR_LIMIT ||
        n != (size_t)w * h * 4)
      return fail(r, "Invalid relay cursor shape");
  } else if (type == 0x10) {
    if ((w != 1 && w != 2) || h < 8000 || h > 48000 || !n || n % (w * 2))
      return fail(r, "Invalid relay PCM format");
  } else if (type != 0 && type != 0x20 && type != 0x21 && type != 0x22 &&
             type != 0x24)
    return fail(r,
                "Unsupported relay stream (JPEG desktop/dashboard required)");
  else if (n)
    return fail(r, "Unexpected relay control payload");
  return 0;
}
static int packet(relay_t *r) {
  unsigned type = r->header[2], x = le16(r->header + 4),
           y = le16(r->header + 6);
  unsigned w = le16(r->header + 8), h = le16(r->header + 10);
  if (type == 3) {
    int64_t before = r->io.now(r->io.context);
    if (relay_jpeg(r, r->payload, r->payload_size, x, y, w, h)) {
      r->skipped++;
      return fail(r, "Invalid or unsupported baseline JPEG from relay");
    }
    r->decode_ms += (unsigned)((r->io.now(r->io.context) - before) / 1000);
    r->frames++;
    r->picture_received = 1;
    r->frame_ready = 1;
    r->changed = 1;
  } else if (type == 0x21)
    r->input_enabled = x != 0;
  else if (type == 0x20)
    r->stats_enabled = x != 0;
  else if (type == 0x22) {
    uint8_t b[3] = {'R', 0, 0};
    put16(b + 1, x);
    return queue(r, b, 3);
  } else if (type == 0x23) {
    for (unsigned i = 0; i < r->payload_size; i++)
      r->cursor[i] = r->payload[i];
    r->cursor_w = w;
    r->cursor_h = h;
    r->hot_x = x;
    r->hot_y = y;
    r->changed = 1;
  } else if (type == 0x24) {
    r->pointer_x = x;
    r->pointer_y = y;
    r->changed = 1;
  } else if (type == 0x10) {
    r->audio_bytes += r->payload_size;
    if (r->io.audio)
      r->io.audio(r->io.context, r->payload, r->payload_size, w, h);
  }
  return 0;
}
/* Incremental RIFF demux for SVP Relay's ffmpeg MJPEG/PCM AVI pipe.
 * LIST bounds are checked; only media/metadata is retained (at most 1 MiB).
 * The relay's endless RIFF/movi sizes are 0xffffffff. */
static int fourcc(const uint8_t *p, const char *s) {
  return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
         p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}
static int avi_meta(relay_t *r) {
  const uint8_t *p = r->payload;
  unsigned n = r->payload_size;
  if (fourcc(r->avi_id, "avih")) {
    if (n < 40 || !le32(p) || le32(p) > 10000000)
      return fail(r, "Invalid AVI timing header");
    r->avi_period = le32(p);
  } else if (fourcc(r->avi_id, "strh")) {
    if (r->avi_current >= 2 || n < 48)
      return fail(r, "Invalid AVI stream header");
    unsigned i = r->avi_current;
    if (fourcc(p, "vids")) {
      if (!fourcc(p + 4, "MJPG") || r->avi_video != 255)
        return fail(r, "AVI requires one MJPEG video stream");
      uint32_t scale = le32(p + 20), rate = le32(p + 24);
      if (!scale || !rate)
        return fail(r, "Invalid AVI video rate");
      uint64_t period = (uint64_t)scale * 1000000 / rate;
      if (period < 8333 || period > 10000000)
        return fail(r, "AVI frame rate outside limits");
      r->avi_period = (unsigned)period;
      r->avi_kind[i] = 1;
      r->avi_video = i;
    } else if (fourcc(p, "auds")) {
      if (r->avi_audio != 255)
        return fail(r, "AVI requires at most one audio stream");
      r->avi_kind[i] = 2;
      r->avi_audio = i;
    } else
      return fail(r, "Unsupported AVI stream type");
  } else if (fourcc(r->avi_id, "strf")) {
    unsigned i = r->avi_current;
    if (i >= 2 || !r->avi_kind[i])
      return fail(r, "AVI format before stream header");
    if (r->avi_kind[i] == 1) {
      if (n < 40 || le32(p) < 40 || !fourcc(p + 16, "MJPG"))
        return fail(r, "AVI video must be MJPEG");
      unsigned w = le32(p + 4), h = le32(p + 8);
      if (!w || !h || w > r->width || h > r->height)
        return fail(r, "AVI picture exceeds panel canvas");
      r->avi_width = w;
      r->avi_height = h;
    } else {
      if (n < 16 || le16(p) != 1 || (le16(p + 2) != 1 && le16(p + 2) != 2) ||
          le32(p + 4) < 8000 || le32(p + 4) > 48000 || le16(p + 14) != 16 ||
          le16(p + 12) != le16(p + 2) * 2)
        return fail(r, "AVI audio requires 16-bit PCM, mono/stereo, 8-48 kHz");
      r->avi_channels = le16(p + 2);
      r->avi_rate = le32(p + 4);
    }
  }
  return 0;
}
static int avi_chunk(relay_t *r) {
  if (!r->avi_retain)
    return 0;
  if (r->avi_retain == 1)
    return avi_meta(r);
  if (!r->avi_started) {
    r->avi_started = 1;
    r->avi_epoch = r->io.now(r->io.context);
    r->avi_next = r->avi_epoch;
  }
  if (r->avi_retain == 2) {
    if (!r->avi_width || !r->avi_height || !r->avi_period)
      return fail(r, "AVI missing MJPEG format/timing");
    int64_t now = r->io.now(r->io.context);
    if (now < r->avi_next)
      return 2; /* Complete frame buffered; wait cooperatively for its time. */
    if (r->payload_size) {
      if (r->avi_frame_count && now - r->avi_next > (int64_t)r->avi_period) {
        r->skipped++;
        /* Backpressure can add a fixed delay. Rebase after dropping this frame
         * so that subsequent pictures do not remain permanently late. */
        r->avi_next = now;
      } else {
        if (relay_jpeg(
                r, r->payload, r->payload_size, (r->width - r->avi_width) / 2,
                (r->height - r->avi_height) / 2, r->avi_width, r->avi_height))
          return fail(r, "Invalid AVI baseline MJPEG picture");
        r->decode_ms += (unsigned)((r->io.now(r->io.context) - now) / 1000);
        r->frames++;
        r->picture_received = r->frame_ready = r->changed = 1;
      }
    }
    r->avi_frame_count++;
    r->avi_next += r->avi_period;
  } else {
    unsigned frame = r->avi_channels * 2;
    if (!frame || !r->avi_rate || r->payload_size % frame)
      return fail(r, "Invalid AVI PCM chunk alignment");
    r->avi_samples += r->payload_size / frame;
    r->audio_bytes += r->payload_size;
    if (r->io.audio && r->payload_size)
      r->io.audio(r->io.context, r->payload, r->payload_size, r->avi_channels,
                  r->avi_rate);
  }
  return 0;
}
static int avi_step(relay_t *r) {
  if (r->avi_stop) {
    if (!r->avi_ending) {
      const uint8_t end[2] = {'E', 0};
      if (queue(r, end, 2))
        return -1;
      r->avi_ending = 1;
    }
    if (flush(r))
      return -1;
    if (r->out_begin != r->out_end)
      return 0;
    r->error = "Media stopped";
    return 1;
  }
  if (r->avi_paused)
    return 0;
  unsigned budget = 128 * 1024;
  while (budget) {
    if (r->avi_eof) {
      int64_t end = r->avi_next;
      if (r->avi_rate) {
        int64_t audio_end =
            r->avi_epoch + (int64_t)(r->avi_samples * 1000000 / r->avi_rate);
        if (audio_end > end)
          end = audio_end;
      }
      if (r->io.now(r->io.context) < end)
        return 0;
      if (!r->avi_ending) {
        const uint8_t done[2] = {'E', 1};
        if (queue(r, done, 2))
          return -1;
        r->avi_ending = 1;
      }
      if (flush(r))
        return -1;
      if (r->out_begin != r->out_end)
        return 0;
      r->error = "Media finished";
      return 1;
    }
    if (r->avi_chunk_active && r->payload_used == r->payload_size) {
      int result = avi_chunk(r);
      if (result < 0)
        return -1;
      if (result == 2)
        return 0;
      r->avi_chunk_active = 0;
      r->avi_pad = r->avi_chunk_pad;
      if (r->changed)
        return 0;
    }
    if (!r->avi_chunk_active && !r->avi_pad) {
      while (r->avi_depth && r->avi_ends[r->avi_depth - 1] &&
             r->avi_offset == r->avi_ends[r->avi_depth - 1]) {
        unsigned depth = --r->avi_depth;
        if (r->avi_lists[depth] == 2)
          r->avi_current = 255;
        if (r->avi_lists[depth] == 1)
          r->avi_movi = 0;
        r->avi_pad = (unsigned)(r->avi_ends[depth] & 1);
        if (r->avi_pad)
          break;
      }
      if (r->avi_depth && r->avi_ends[r->avi_depth - 1] &&
          r->avi_offset > r->avi_ends[r->avi_depth - 1])
        return fail(r, "AVI exceeds RIFF/LIST bounds");
    }
    uint8_t discard[1024];
    void *out;
    unsigned take;
    if (r->avi_pad) {
      out = discard;
      take = 1;
    } else if (r->avi_chunk_active) {
      take = r->payload_size - r->payload_used;
      if (take > budget)
        take = budget;
      if (r->avi_retain)
        out = r->payload + r->payload_used;
      else {
        if (take > sizeof discard)
          take = sizeof discard;
        out = discard;
      }
    } else {
      take = r->avi_need - r->header_used;
      out = r->header + r->header_used;
      if (r->avi_depth && r->avi_ends[r->avi_depth - 1] &&
          r->avi_ends[r->avi_depth - 1] - r->avi_offset < take)
        return fail(r, "Truncated AVI chunk header");
    }
    int n = r->io.recv(r->io.context, out, take);
    if (n == -2)
      break;
    if (n < 0 || (unsigned)n > take)
      return fail(r, "AVI network receive failed");
    if (!n) {
      int bounded = 0;
      for (unsigned i = 0; i < r->avi_depth; i++)
        if (r->avi_ends[i] && r->avi_offset != r->avi_ends[i])
          bounded = 1;
      if (r->avi_chunk_active || r->header_used || r->avi_pad || bounded ||
          !r->avi_started)
        return fail(r, "Truncated AVI stream");
      r->avi_eof = 1;
      continue;
    }
    budget -= (unsigned)n;
    r->bytes += (unsigned)n;
    r->avi_offset += (unsigned)n;
    r->last_rx = r->io.now(r->io.context);
    if (r->avi_pad) {
      r->avi_pad = 0;
      continue;
    }
    if (r->avi_chunk_active) {
      r->payload_used += (unsigned)n;
      continue;
    }
    r->header_used += (unsigned)n;
    if (r->header_used != r->avi_need)
      continue;
    uint32_t size = le32(r->header + 4);
    int list = fourcc(r->header, "LIST"), riff = fourcc(r->header, "RIFF");
    if ((list || riff) && r->avi_need == 8) {
      r->avi_need = 12;
      continue;
    }
    if (list || riff) {
      if (size < 4 || r->avi_depth == 8 ||
          (riff && (r->avi_offset != 12 || !fourcc(r->header + 8, "AVI "))))
        return fail(r, "Invalid AVI RIFF/LIST");
      uint64_t end = size == 0xffffffffu ? 0 : r->avi_offset - 4 + size;
      if (r->avi_depth && r->avi_ends[r->avi_depth - 1] &&
          (!end || end + (size & 1) > r->avi_ends[r->avi_depth - 1]))
        return fail(r, "AVI nested LIST exceeds parent");
      unsigned kind = 0;
      if (list && fourcc(r->header + 8, "movi")) {
        r->avi_movi = 1;
        kind = 1;
      }
      if (list && fourcc(r->header + 8, "strl")) {
        if (r->avi_streams >= 2 || r->avi_movi)
          return fail(r, "Too many AVI streams");
        r->avi_current = r->avi_streams++;
        kind = 2;
      }
      r->avi_ends[r->avi_depth] = end;
      r->avi_lists[r->avi_depth++] = kind;
    } else {
      uint64_t end = r->avi_offset + size;
      if (r->avi_depth && r->avi_ends[r->avi_depth - 1] &&
          end + (size & 1) > r->avi_ends[r->avi_depth - 1])
        return fail(r, "AVI chunk exceeds LIST");
      for (unsigned i = 0; i < 4; i++)
        r->avi_id[i] = r->header[i];
      r->avi_retain = 0;
      if (!r->avi_movi &&
          (fourcc(r->header, "avih") || fourcc(r->header, "strh") ||
           fourcc(r->header, "strf"))) {
        if (size > 4096)
          return fail(r, "AVI metadata exceeds limit");
        r->avi_retain = 1;
      }
      if (r->avi_movi && r->header[0] >= '0' && r->header[0] <= '9' &&
          r->header[1] >= '0' && r->header[1] <= '9') {
        unsigned stream = (r->header[0] - '0') * 10 + r->header[1] - '0';
        if (stream == r->avi_video && r->header[2] == 'd' &&
            (r->header[3] == 'c' || r->header[3] == 'b'))
          r->avi_retain = 2;
        else if (stream == r->avi_audio && r->header[2] == 'w' &&
                 r->header[3] == 'b')
          r->avi_retain = 3;
        else
          return fail(r, "Unknown AVI media stream");
      }
      if (r->avi_retain && size > RELAY_JPEG_LIMIT)
        return fail(r, "AVI chunk exceeds 1 MB limit");
      r->payload_size = size;
      r->payload_used = 0;
      r->avi_chunk_active = 1;
      r->avi_pad = 0; /* Set after payload processing, not before it. */
      r->avi_chunk_pad = size & 1;
    }
    r->header_used = 0;
    r->avi_need = 8;
  }
  if (r->io.now(r->io.context) - r->last_rx > 30000000LL)
    return fail(r, "AVI sender timed out");
  return 0;
}
void relay_media_pause(relay_t *r, int paused) {
  if (r->wire_mode != 2 || r->avi_paused == !!paused)
    return;
  int64_t now = r->io.now(r->io.context);
  if (paused)
    r->avi_pause_at = now;
  else {
    r->avi_epoch += now - r->avi_pause_at;
    r->avi_next += now - r->avi_pause_at;
    r->last_rx = now;
  }
  r->avi_paused = !!paused;
}
void relay_media_stop(relay_t *r) {
  if (r->wire_mode == 2)
    r->avi_stop = 1;
}

static int relay_stats(relay_t *r) {
  int64_t now = r->io.now(r->io.context);
  if (r->stats_enabled && now - r->last_stats >= 1000000) {
    uint8_t b[46] = {'M', 11};
    unsigned values[11] = {(unsigned)((now - r->last_stats) / 1000),
                           r->bytes,
                           r->frames,
                           r->skipped,
                           0,
                           r->frames,
                           r->decode_ms,
                           r->audio_bytes,
                           r->wire_mode == 2 ? r->frames : 0,
                           r->wire_mode == 2 ? r->decode_ms : 0,
                           0};
    for (unsigned i = 0; i < 11; i++)
      put32(b + 2 + i * 4, values[i]);
    if (queue(r, b, sizeof b))
      return -1;
    r->last_stats = now;
    r->bytes = r->frames = r->skipped = r->decode_ms = r->audio_bytes = 0;
  }
  return flush(r);
}
int relay_step(relay_t *r) {
  if (r->error && r->error[0])
    return -1;
  if (flush(r))
    return -1;
  if (r->wire_mode == 2) {
    int result = avi_step(r);
    return result ? result : relay_stats(r);
  }
  unsigned budget = 128 * 1024;
  while (budget) {
    int n;
    unsigned take;
    if (r->header_used < 16) {
      take = (r->wire_mode ? 16 : 4) - r->header_used;
      n = r->io.recv(r->io.context, r->header + r->header_used, take);
      if (n > 0) {
        if ((unsigned)n > take)
          return fail(r, "Invalid network receive count");
        r->header_used += (unsigned)n;
        if (!r->wire_mode && r->header_used == 4) {
          if (fourcc(r->header, "SVPM")) {
            r->stats_enabled = 1;
            r->header_used = 0;
          } else if (fourcc(r->header, "RIFF")) {
            r->wire_mode = 2;
            r->avi_offset = 4;
            r->bytes += 4;
            r->last_rx = r->io.now(r->io.context);
            return avi_step(r);
          } else
            r->wire_mode = 1;
        }
        if (r->header_used == 16 && header_check(r))
          return -1;
      }
    } else {
      take = r->payload_size - r->payload_used;
      if (take > budget)
        take = budget;
      if (r->header[2] == 0x7e) {
        uint8_t discard[1024];
        if (take > sizeof discard)
          take = sizeof discard;
        n = r->io.recv(r->io.context, discard, take);
      } else
        n = r->io.recv(r->io.context, r->payload + r->payload_used, take);
      if (n > 0) {
        if ((unsigned)n > take)
          return fail(r, "Invalid network receive count");
        r->payload_used += (unsigned)n;
      }
    }
    if (n == -2)
      break;
    if (n <= 0)
      return fail(r, "Relay sender disconnected");
    budget -= (unsigned)n < budget ? (unsigned)n : budget;
    r->bytes += (unsigned)n;
    r->last_rx = r->io.now(r->io.context);
    if (r->header_used == 16 && r->payload_used == r->payload_size) {
      if (packet(r))
        return -1;
      r->header_used = 0;
      if (r->changed)
        break; /* Draw/input gets a turn before another picture. */
    }
  }
  int64_t now = r->io.now(r->io.context);
  /* The relay opens TCP before its first Chromium install/start (up to 30 min).
   * Once pictures start, its normal heartbeat must keep the connection alive.
   */
  int64_t idle_limit = r->picture_received ? 30000000LL : 1800000000LL;
  if (now - r->last_rx > idle_limit)
    return fail(r, "Relay sender timed out");
  return relay_stats(r);
}

int relay_touch(relay_t *r, const int *ids, const int *xs, const int *ys,
                unsigned count) {
  if (!r->input_enabled)
    return 0;
  if (count > 5)
    count = 5;
  uint8_t b[27] = {'T', 0};
  b[1] = count;
  for (unsigned i = 0; i < count; i++) {
    b[2 + i * 5] = ids[i];
    put16(b + 3 + i * 5, xs[i]);
    put16(b + 5 + i * 5, ys[i]);
  }
  return queue(r, b, 2 + count * 5);
}
int relay_mouse(relay_t *r, int dx, int dy, int wheel, unsigned buttons) {
  if (!r->input_enabled)
    return 0;
  if (dx > 32767)
    dx = 32767;
  if (dx < -32768)
    dx = -32768;
  if (dy > 32767)
    dy = 32767;
  if (dy < -32768)
    dy = -32768;
  if (wheel > 127)
    wheel = 127;
  if (wheel < -128)
    wheel = -128;
  uint8_t b[7] = {'P', 0, 0, 0, 0, 0, 0};
  put16(b + 1, (unsigned)dx);
  put16(b + 3, (unsigned)dy);
  b[5] = wheel;
  b[6] = buttons;
  return queue(r, b, sizeof b);
}
static unsigned usage_for(int key) {
  if (key >= 'a' && key <= 'z')
    return 4 + key - 'a';
  if (key >= 'A' && key <= 'Z')
    return 4 + key - 'A';
  const char *plain = "1234567890", *shift = "!@#$%^&*()";
  for (unsigned i = 0; i < 10; i++)
    if (key == plain[i] || key == shift[i])
      return 0x1e + i;
  const char *p = "-=[]\\;\x27`,./", *s = "_+{}|:\x22~<>?";
  const unsigned codes[] = {0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x33,
                            0x34, 0x35, 0x36, 0x37, 0x38};
  for (unsigned i = 0; i < 11; i++)
    if (key == p[i] || key == s[i])
      return codes[i];
  if (key >= 135 && key <= 146)
    return 0x3a + key - 135;
  switch (key) {
  case 13:
    return 0x28;
  case 27:
    return 0x29;
  case 8:
  case 127:
    return 0x2a;
  case 9:
    return 0x2b;
  case 32:
    return 0x2c;
  case 132:
    return 0xe2;
  case 133:
    return 0xe0;
  case 134:
    return 0xe1;
  case 147:
    return 0x49;
  case 148:
    return 0x4c;
  case 149:
    return 0x4e;
  case 150:
    return 0x4b;
  case 151:
    return 0x4a;
  case 152:
    return 0x4d;
  case 153:
    return 0x52;
  case 154:
    return 0x4f;
  case 155:
    return 0x51;
  case 156:
    return 0x50;
  default:
    return 0;
  }
}
int relay_key(relay_t *r, int key, int down) {
  if (!r->input_enabled)
    return 0;
  unsigned mask = key == 132 ? 4 : key == 133 ? 2 : key == 134 ? 1 : 0;
  if (mask) {
    if (down)
      r->modifiers |= mask;
    else
      r->modifiers &= ~mask;
  }
  unsigned usage = usage_for(key);
  if (!usage)
    return 0;
  uint8_t b[13] = {'K', 0xff, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  put16(b + 4, usage);
  b[6] = r->modifiers;
  b[7] = !!down;
  if (key >= 32 && key <= 126)
    put32(b + 8, (unsigned)key);
  if (queue(r, b, sizeof b))
    return -1;
  /* Browser dashboards consume X text; VNC consumes K codepoints and ignores X.
   */
  if (down && key >= 32 && key <= 126 && !(r->modifiers & (2u | 4u | 8u))) {
    uint8_t text[5] = {'X', 0, 0, 0, 0};
    put32(text + 1, (unsigned)key);
    return queue(r, text, sizeof text);
  }
  return 0;
}
void relay_draw_cursor(const relay_t *r, uint16_t *canvas) {
  for (unsigned y = 0; y < r->cursor_h; y++)
    for (unsigned x = 0; x < r->cursor_w; x++) {
      int dx = (int)r->pointer_x - (int)r->hot_x + (int)x,
          dy = (int)r->pointer_y - (int)r->hot_y + (int)y;
      if (dx < 0 || dy < 0 || (unsigned)dx >= r->width ||
          (unsigned)dy >= r->height)
        continue;
      const uint8_t *p = r->cursor + ((size_t)y * r->cursor_w + x) * 4;
      unsigned a = p[3];
      if (!a)
        continue;
      uint16_t old = canvas[(size_t)dy * r->width + dx];
      unsigned red = ((p[2] >> 3) * a + (old >> 11) * (255 - a)) / 255;
      unsigned green = ((p[1] >> 2) * a + ((old >> 5) & 63) * (255 - a)) / 255;
      unsigned blue = ((p[0] >> 3) * a + (old & 31) * (255 - a)) / 255;
      canvas[(size_t)dy * r->width + dx] =
          (uint16_t)(red << 11 | green << 5 | blue);
    }
}
