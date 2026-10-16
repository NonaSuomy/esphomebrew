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
  /* The owning app calls close before reusing a receiver. */
  uint8_t *raw = (uint8_t *)r;
  for (size_t i = 0; i < sizeof *r; i++)
    raw[i] = 0;
  r->io = io;
  r->pixels = pixels;
  r->width = w;
  r->height = h;
  r->error = "";
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
int relay_step(relay_t *r) {
  if (r->error && r->error[0])
    return -1;
  if (flush(r))
    return -1;
  unsigned budget = 128 * 1024;
  while (budget) {
    int n;
    unsigned take;
    if (r->header_used < 16) {
      take = 16 - r->header_used;
      n = r->io.recv(r->io.context, r->header + r->header_used, take);
      if (n > 0) {
        if ((unsigned)n > take)
          return fail(r, "Invalid network receive count");
        r->header_used += (unsigned)n;
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
  if (now - r->last_rx > 30000000)
    return fail(r, "Relay sender timed out");
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
                           0,
                           0,
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
