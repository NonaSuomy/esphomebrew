#include "rfb.h"

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] * 256 + p[1]; }
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static void put16(uint8_t *p, unsigned n) {
  p[0] = n >> 8;
  p[1] = n;
}
static void put32(uint8_t *p, uint32_t n) {
  p[0] = n >> 24;
  p[1] = n >> 16;
  p[2] = n >> 8;
  p[3] = n;
}
static int fail(rfb_t *r, const char *s) {
  r->error = s;
  return -1;
}
static int readn(rfb_t *r, void *p, size_t n) {
  return r->io.read(r->io.context, p, n) < 0
             ? fail(r, "Connection closed or timed out")
             : 0;
}
static int sendn(rfb_t *r, const void *p, size_t n) {
  return r->io.write(r->io.context, p, n) < 0
             ? fail(r, "Could not send to desktop")
             : 0;
}
static int skip(rfb_t *r, size_t n) {
  uint8_t buf[256];
  while (n) {
    size_t k = n < sizeof buf ? n : sizeof buf;
    if (readn(r, buf, k))
      return -1;
    n -= k;
  }
  return 0;
}
static int resize(rfb_t *r, unsigned w, unsigned h) {
  if (!w || !h || w > 4096 || h > 4096 || (size_t)w * h > 8 * 1024 * 1024)
    return fail(r, "Desktop too large (max 8 megapixels / 4096 per side)");
  /* Free first so resizing does not require two desktop buffers in PSRAM. */
  if (r->pixels)
    r->io.free(r->io.context, r->pixels);
  r->pixels = r->io.alloc(r->io.context, (size_t)w * h * 2);
  if (!r->pixels)
    return fail(r, "Not enough PSRAM for this desktop size");
  r->width = w;
  r->height = h;
  r->resized = 1;
  for (size_t i = 0; i < (size_t)w * h; i++)
    r->pixels[i] = 0;
  return 0;
}
void rfb_close(rfb_t *r) {
  if (r->pixels)
    r->io.free(r->io.context, r->pixels);
  r->pixels = 0;
  r->width = r->height = 0;
}
int rfb_start(rfb_t *r, const char *password) {
  uint8_t b[256], reply[16];
  r->error = "";
  r->resized = 0;
  if (readn(r, b, 12))
    return -1;
  if (b[0] != 'R' || b[1] != 'F' || b[2] != 'B' || b[3] != ' ' || b[4] != '0' ||
      b[5] != '0' || b[6] != '3' || b[7] != '.' || b[8] < '0' || b[8] > '9' ||
      b[9] < '0' || b[9] > '9' || b[10] < '0' || b[10] > '9' || b[11] != '\n')
    return fail(r, "Not a VNC/RFB 3.x server (SPICE is unsupported)");
  unsigned minor = (b[8] - '0') * 100 + (b[9] - '0') * 10 + b[10] - '0';
  if (minor < 3)
    return fail(r, "RFB version is too old");
  r->minor = minor >= 8 ? 8 : minor >= 7 ? 7 : 3;
  const char *version = r->minor == 8   ? "RFB 003.008\n"
                        : r->minor == 7 ? "RFB 003.007\n"
                                        : "RFB 003.003\n";
  if (sendn(r, version, 12))
    return -1;
  unsigned security = 0;
  if (r->minor == 3) {
    if (readn(r, b, 4))
      return -1;
    security = be32(b);
  } else {
    if (readn(r, b, 1))
      return -1;
    unsigned count = b[0];
    if (!count)
      return fail(r, "VNC server refused the connection");
    if (readn(r, b, count))
      return -1;
    unsigned none = 0, auth = 0;
    for (unsigned i = 0; i < count; i++) {
      none |= b[i] == 1;
      auth |= b[i] == 2;
    }
    security = auth && password[0] ? 2 : none ? 1 : auth ? 2 : 0;
    if (!security)
      return fail(r,
                  "Enable classic VNC password authentication on the server");
    b[0] = security;
    if (sendn(r, b, 1))
      return -1;
  }
  if (security == 2) {
    if (!password[0])
      return fail(r, "This desktop requires a VNC password");
    if (readn(r, b, 16))
      return -1;
    vnc_response(password, b, reply);
    if (sendn(r, reply, 16))
      return -1;
    for (unsigned i = 0; i < 16; i++)
      reply[i] = 0;
  } else if (security != 1)
    return fail(r, "Unsupported VNC security type");
  if (security == 2 || r->minor == 8) {
    if (readn(r, b, 4))
      return -1;
    if (be32(b))
      return fail(r, "VNC authentication failed");
  }
  b[0] = 1; /* Shared session: keep other viewers connected. */
  if (sendn(r, b, 1) || readn(r, b, 24))
    return -1;
  unsigned w = be16(b), h = be16(b + 2);
  uint32_t n = be32(b + 20);
  if (n > 1024 * 1024)
    return fail(r, "Invalid desktop name length");
  unsigned k = n < sizeof r->name - 1 ? n : sizeof r->name - 1;
  if (readn(r, r->name, k) || skip(r, n - k))
    return -1;
  r->name[k] = 0;
  if (resize(r, w, h))
    return -1;
  /* SetPixelFormat: 16-bit, little endian, true colour RGB565. */
  static const uint8_t format[20] = {0, 0,  0, 0,  16, 16, 0, 1, 0, 31,
                                     0, 63, 0, 31, 11, 5,  0, 0, 0, 0};
  /* Hextile, CopyRect, Raw, DesktopSize. No unsolicited extensions. */
  static const uint8_t enc[20] = {2, 0, 0, 4, 0, 0, 0,   5,   0,   0,
                                  0, 1, 0, 0, 0, 0, 255, 255, 255, 33};
  if (sendn(r, format, sizeof format) || sendn(r, enc, sizeof enc))
    return -1;
  return rfb_request(r, 0);
}
int rfb_request(rfb_t *r, int incremental) {
  uint8_t b[10] = {3, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  b[1] = !!incremental;
  put16(b + 6, r->width);
  put16(b + 8, r->height);
  return sendn(r, b, sizeof b);
}
int rfb_key(rfb_t *r, uint32_t sym, int down) {
  uint8_t b[8] = {4, 0, 0, 0, 0, 0, 0, 0};
  b[1] = !!down;
  put32(b + 4, sym);
  return sendn(r, b, sizeof b);
}
int rfb_pointer(rfb_t *r, unsigned x, unsigned y, unsigned buttons) {
  uint8_t b[6] = {5, 0, 0, 0, 0, 0};
  if (!r->width || !r->height)
    return -1;
  if (x >= r->width)
    x = r->width - 1;
  if (y >= r->height)
    y = r->height - 1;
  b[1] = buttons;
  put16(b + 2, x);
  put16(b + 4, y);
  return sendn(r, b, sizeof b);
}
static int raw(rfb_t *r, unsigned x, unsigned y, unsigned w, unsigned h) {
  uint8_t b[1024];
  for (unsigned row = 0; row < h; row++) {
    for (unsigned col = 0; col < w;) {
      unsigned n = w - col < sizeof b / 2 ? w - col : sizeof b / 2;
      if (readn(r, b, n * 2))
        return -1;
      uint16_t *out = r->pixels + (size_t)(y + row) * r->width + x + col;
      for (unsigned i = 0; i < n; i++)
        out[i] = b[i * 2] | (unsigned)b[i * 2 + 1] << 8;
      col += n;
    }
  }
  return 0;
}
static void fill(rfb_t *r, unsigned x, unsigned y, unsigned w, unsigned h,
                 uint16_t c) {
  for (unsigned j = 0; j < h; j++)
    for (unsigned i = 0; i < w; i++)
      r->pixels[(size_t)(y + j) * r->width + x + i] = c;
}
static int hextile(rfb_t *r, unsigned x, unsigned y, unsigned w, unsigned h) {
  uint8_t b[4];
  uint16_t bg = 0, fg = 0;
  int have_bg = 0, have_fg = 0;
  for (unsigned ty = 0; ty < h; ty += 16)
    for (unsigned tx = 0; tx < w; tx += 16) {
      unsigned tw = w - tx < 16 ? w - tx : 16, th = h - ty < 16 ? h - ty : 16;
      if (readn(r, b, 1))
        return -1;
      unsigned flags = b[0];
      if (flags & 1) {
        if (raw(r, x + tx, y + ty, tw, th))
          return -1;
        continue;
      }
      if (flags & ~31u)
        return fail(r, "Invalid Hextile flags");
      if (flags & 2) {
        if (readn(r, b, 2))
          return -1;
        bg = b[0] | (unsigned)b[1] << 8;
        have_bg = 1;
      }
      if (!have_bg)
        return fail(r, "Missing Hextile background");
      fill(r, x + tx, y + ty, tw, th, bg);
      if (flags & 4) {
        if (readn(r, b, 2))
          return -1;
        fg = b[0] | (unsigned)b[1] << 8;
        have_fg = 1;
      }
      if (flags & 8) {
        if (!(flags & 16) && !have_fg)
          return fail(r, "Missing Hextile foreground");
        if (readn(r, b, 1))
          return -1;
        unsigned count = b[0];
        while (count--) {
          uint16_t c = fg;
          if (flags & 16) {
            if (readn(r, b, 2))
              return -1;
            c = b[0] | (unsigned)b[1] << 8;
          }
          if (readn(r, b, 2))
            return -1;
          unsigned sx = b[0] >> 4, sy = b[0] & 15, sw = (b[1] >> 4) + 1,
                   sh = (b[1] & 15) + 1;
          if (sx + sw > tw || sy + sh > th)
            return fail(r, "Invalid Hextile subrectangle");
          fill(r, x + tx + sx, y + ty + sy, sw, sh, c);
        }
      }
    }
  return 0;
}
int rfb_update(rfb_t *r) {
  uint8_t b[12];
  if (readn(r, b, 1))
    return -1;
  unsigned type = b[0];
  if (type == 2)
    return 0; /* Bell. */
  if (type == 3) {
    if (readn(r, b, 7))
      return -1;
    uint32_t n = be32(b + 3);
    if (n > 1024 * 1024)
      return fail(r, "Desktop clipboard exceeds 1 MB");
    return skip(r, n);
  }
  if (type == 1) {
    if (readn(r, b, 5))
      return -1;
    return skip(r, be16(b + 3) * 6u);
  }
  if (type != 0)
    return fail(r, "Unsupported VNC server message");
  if (readn(r, b, 3))
    return -1;
  unsigned count = be16(b + 1);
  r->resized = 0;
  while (count--) {
    if (readn(r, b, 12))
      return -1;
    unsigned x = be16(b), y = be16(b + 2), w = be16(b + 4), h = be16(b + 6);
    int32_t encoding = (int32_t)be32(b + 8);
    if (encoding == -223) {
      if (resize(r, w, h))
        return -1;
      continue;
    }
    if (x > r->width || y > r->height || w > r->width - x || h > r->height - y)
      return fail(r, "Desktop rectangle outside framebuffer");
    if (encoding == 0) {
      if (raw(r, x, y, w, h))
        return -1;
    } else if (encoding == 5) {
      if (hextile(r, x, y, w, h))
        return -1;
    } else if (encoding == 1) {
      if (readn(r, b, 4))
        return -1;
      unsigned sx = be16(b), sy = be16(b + 2);
      if (sx > r->width || sy > r->height || w > r->width - sx ||
          h > r->height - sy)
        return fail(r, "CopyRect source outside framebuffer");
      /* Reverse the entire traversal when copying towards higher addresses. */
      size_t from = (size_t)sy * r->width + sx, to = (size_t)y * r->width + x;
      for (unsigned j = 0; j < h; j++)
        for (unsigned i = 0; i < w; i++) {
          unsigned row = to > from ? h - 1 - j : j,
                   col = to > from ? w - 1 - i : i;
          r->pixels[to + (size_t)row * r->width + col] =
              r->pixels[from + (size_t)row * r->width + col];
        }
    } else
      return fail(r, "Server used an unrequested VNC encoding");
  }
  if (rfb_request(r, !r->resized))
    return -1;
  return 1;
}
uint32_t rfb_keysym(int key) {
  if (key >= 32 && key <= 126)
    return key;
  if (key >= 135 && key <= 146)
    return 0xffbe + (key - 135);
  switch (key) {
  case 9:
    return 0xff09;
  case 13:
    return 0xff0d;
  case 27:
    return 0xff1b;
  case 8:
  case 127:
    return 0xff08;
  case 132:
    return 0xffe9;
  case 133:
    return 0xffe3;
  case 134:
    return 0xffe1;
  case 147:
    return 0xff63;
  case 148:
    return 0xffff;
  case 149:
    return 0xff56;
  case 150:
    return 0xff55;
  case 151:
    return 0xff50;
  case 152:
    return 0xff57;
  case 153:
    return 0xff52;
  case 154:
    return 0xff53;
  case 155:
    return 0xff54;
  case 156:
    return 0xff51;
  default:
    return 0;
  }
}
