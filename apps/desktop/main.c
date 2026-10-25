#define PAX_APP_SIDE 1
#include "audio_buffer.h"
#include "font.h"
#include "media_client.h"
#include "psram_app.h"
#include "relay.h"
#include "relay_http.h"
#include "rfb.h"

static const app_services_t *svc;
static rfb_t remote;
static uint16_t *screen;
static unsigned *xmap;
static int sw = 800, sh = 480, sock = -1, quit, back, connected, in_input;
static int selected, shifted, dirty = 1, touch_before, mouse_buttons, mouse_x,
                              mouse_y;
static int vx, vy, vw, vh;
static int64_t menu_since, two_since, last_input, last_draw;
static char host[128], port_text[6] = "5900", password[9];
static const char *status = "Enter your desktop's VNC host and password";
static unsigned prev_pad;
static relay_t relay;
static int relay_active, relay_listener = -1, relay_peer = -1, listen_requested,
                         relay_requested;
static uint16_t *relay_pixels;
static unsigned relay_touch_signature;
static int dashboard_mode, media_mode, relay_muted;
static audio_buffer_t audio_buffer;
static int native_w, native_h;
static int browser_active, browser_requested, browser_action, browser_busy,
    browser_cancel;
static int relay_pump(void);
static void browser_draw(void);
static void browser_inputs(unsigned, int, int, int);
static void browser_run(void);
static void browser_free(void);
static void relay_session(int request_vnc);
static void relay_inputs(int x, int y, int touched);

static size_t length(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}
static void copy(char *out, const char *in, size_t cap) {
  size_t i = 0;
  while (i + 1 < cap && in[i]) {
    out[i] = in[i];
    i++;
  }
  out[i] = 0;
}
static void zero(void *p, size_t n) {
  unsigned char *b = p;
  while (n--)
    *b++ = 0;
}
/* Compiler-generated freestanding memory calls. */
void *memset(void *p, int c, size_t n) {
  unsigned char *b = p;
  while (n--)
    *b++ = (unsigned char)c;
  return p;
}
void *memcpy(void *out, const void *in, size_t n) {
  unsigned char *d = out;
  const unsigned char *s = in;
  while (n--)
    *d++ = *s++;
  return out;
}
static void fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > sw)
    w = sw - x;
  if (y + h > sh)
    h = sh - y;
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      screen[(y + j) * sw + x + i] = c;
}
static void text(int x, int y, const char *s, int scale, uint16_t color) {
  int start = x;
  for (; *s; s++) {
    if (*s == '\n') {
      x = start;
      y += 10 * scale;
      continue;
    }
    unsigned char c = *s;
    if (c >= 'a' && c <= 'z')
      c -= 32;
    unsigned k = 0;
    while (font_chars[k] && font_chars[k] != c)
      k++;
    if (!font_chars[k])
      k = 43; /* question mark */
    if (x + 5 * scale > sw - 8)
      break;
    for (int col = 0; col < 5; col++)
      for (int row = 0; row < 7; row++)
        if (font_bits[k][col] & (1u << row))
          fill(x + col * scale, y + row * scale, scale, scale, color);
    x += 6 * scale;
  }
}
static void button(int x, int y, int w, int h, const char *s, int active) {
  fill(x, y, w, h, active ? 0x43ba : 0x2129);
  int scale = sw >= 640 ? 2 : 1;
  text(x + 8, y + (h - 7 * scale) / 2, s, scale, 0xffff);
}
static const char *rows[4] = {"1234567890.-", "qwertyuiop@:", "asdfghjkl/_?",
                              "zxcvbnm!#$%&"};
static const char *symbols[4] = {"!@#$%^&*()_+", "[]{}<>:;,.?/",
                                 "\\|~`'\"-=    ", "0123456789  "};
static int key_y, key_h;
static void draw_home(void) {
  fill(0, 0, sw, sh, 0x1084);
  int scale = sw >= 640 ? 2 : 1;
  text(18, 15, "WORKSPACE / VNC", scale, 0x8e7f);
  if (sw >= 1000)
    button(240, 6, 140, 34, "MEDIA", selected == 10);
  if (sw >= 800)
    button(sw - 598, 6, 140, 34, "VIDEO/AUDIO", selected == 9);
  if (sw >= 640) {
    button(sw - 450, 6, 140, 34, "DASHBOARD", selected == 8);
    button(sw - 302, 6, 140, 34, "LISTEN", selected == 6);
    button(sw - 154, 6, 140, 34, "VIA RELAY", selected == 7);
  }
  text(18, sh - 21, status, 1, 0xc618);
  int fh = (sh - 100) / 8;
  if (fh < 27)
    fh = 27;
  if (fh > 48)
    fh = 48;
  const char *labels[] = {"HOST", "PORT", "PASSWORD"};
  const char *values[] = {host, port_text, password};
  for (int i = 0; i < 3; i++) {
    int y = 48 + i * (fh + 6);
    button(14, y, sw - 28, fh, "", selected == i);
    text(24, y + (fh - 7 * scale) / 2, labels[i], scale, 0x8e7f);
    char hidden[9];
    zero(hidden, sizeof hidden);
    for (size_t n = 0; n < length(password); n++)
      hidden[n] = '*';
    text(sw >= 640 ? 150 : 82, y + (fh - 7 * scale) / 2,
         i == 2 ? hidden : values[i], scale, 0xffff);
  }
  int by = 48 + 3 * (fh + 6), bw = (sw - 42) / 3;
  button(14, by, bw, fh, "CONNECT", selected == 3);
  button(21 + bw, by, bw, fh, "SAVE HOST", selected == 4);
  button(28 + bw * 2, by, bw, fh, "EXIT", selected == 5);
  key_y = by + fh + 10;
  key_h = (sh - 30 - key_y) / 5;
  int kw = (sw - 28) / 12;
  for (int row = 0; row < 4; row++)
    for (int col = 0; col < 12; col++) {
      char c = (shifted == 2 ? symbols : rows)[row][col];
      if (shifted == 1 && c >= 'a' && c <= 'z')
        c -= 32;
      char s[2] = {c, 0};
      button(14 + col * kw, key_y + row * key_h, kw - 3, key_h - 3, s, 0);
    }
  int y = key_y + 4 * key_h;
  button(14, y, (sw - 28) / 4 - 3, key_h - 3,
         shifted == 2   ? "LOWER"
         : shifted == 1 ? "SYMBOLS"
                        : "UPPER",
         0);
  button(14 + (sw - 28) / 4, y, (sw - 28) / 4 - 3, key_h - 3, "SPACE", 0);
  button(14 + (sw - 28) / 2, y, (sw - 28) / 4 - 3, key_h - 3, "BACK", 0);
  button(14 + 3 * (sw - 28) / 4, y, (sw - 28) / 4 - 3, key_h - 3, "NEXT", 0);
  svc->display_flush();
  dirty = 0;
}
static void geometry(void) {
  vw = sw;
  vh = (int)((uint64_t)sw * remote.height / remote.width);
  if (vh > sh) {
    vh = sh;
    vw = (int)((uint64_t)sh * remote.width / remote.height);
  }
  if (vw < 1)
    vw = 1;
  if (vh < 1)
    vh = 1;
  vx = (sw - vw) / 2;
  vy = (sh - vh) / 2;
}
static void draw_remote(void) {
  geometry();
  fill(0, 0, sw, sh, 0);
  for (int x = 0; x < vw; x++)
    xmap[x] = (unsigned)x * remote.width / (unsigned)vw;
  for (int y = 0; y < vh; y++) {
    const uint16_t *row =
        remote.pixels +
        (size_t)((unsigned)y * remote.height / (unsigned)vh) * remote.width;
    uint16_t *out = screen + (vy + y) * sw + vx;
    for (int x = 0; x < vw; x++)
      out[x] = row[xmap[x]];
  }
  /* A local cursor remains visible when the server omits cursor updates. */
  fill(mouse_x - 5, mouse_y, 11, 1, 0xffff);
  fill(mouse_x, mouse_y - 5, 1, 11, 0xffff);
  svc->display_flush();
  last_draw = svc->get_time_us();
  dirty = 0;
}
static void *allocate(void *ctx, size_t n) {
  (void)ctx;
  return svc->mem_alloc(n);
}
static void release(void *ctx, void *p) {
  (void)ctx;
  svc->mem_free(p);
}
static void poll_input(void);
static int transfer(void *ctx, void *buf, size_t n, int sending) {
  (void)ctx;
  unsigned char *p = buf;
  int64_t progress = svc->get_time_us();
  while (n) {
    if (quit || back)
      return -1;
    int k = sending ? svc->net_tcp_send(sock, p, (int)n)
                    : svc->net_tcp_recv(sock, p, (int)n);
    if (k > 0) {
      p += k;
      n -= k;
      progress = svc->get_time_us();
    } else if ((sending && k < 0) || (!sending && k != -2))
      return -1;
    if (svc->get_time_us() - progress > 15000000)
      return -1;
    if (!sending && !in_input && svc->get_time_us() - last_input > 8000) {
      int64_t before = svc->get_time_us();
      poll_input();
      /* Shared menus can pause input polling for an arbitrary time. */
      progress += svc->get_time_us() - before;
    }
    if (k <= 0)
      svc->delay_ms(2);
  }
  return 0;
}
static int receive(void *ctx, void *p, size_t n) {
  return transfer(ctx, p, n, 0);
}
static int transmit(void *ctx, const void *p, size_t n) {
  return transfer(ctx, (void *)p, n, 1);
}
static void pointer(int buttons) {
  if (!connected)
    return;
  geometry();
  int x = mouse_x - vx, y = mouse_y - vy;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x >= vw)
    x = vw - 1;
  if (y >= vh)
    y = vh - 1;
  if (rfb_pointer(&remote, (unsigned)((uint64_t)x * remote.width / vw),
                  (unsigned)((uint64_t)y * remote.height / vh), buttons))
    back = 1;
  dirty = 1;
}
static void type_char(int key) {
  if (selected > 2)
    return;
  char *s = selected == 0 ? host : selected == 1 ? port_text : password;
  size_t cap = selected == 0   ? sizeof host
               : selected == 1 ? sizeof port_text
                               : sizeof password,
         n = length(s);
  if (key == 8 || key == 127) {
    if (n)
      s[n - 1] = 0;
  } else if (key >= 32 && key <= 126 && n + 1 < cap) {
    if (selected == 1 && (key < '0' || key > '9'))
      return;
    s[n] = (char)key;
    s[n + 1] = 0;
  }
  dirty = 1;
}
static unsigned port_number(void) {
  unsigned n = 0;
  for (const char *p = port_text; *p; p++) {
    if (*p < '0' || *p > '9')
      return 0;
    n = n * 10 + *p - '0';
  }
  return n > 65535 ? 0 : n;
}
static void save_host(void) {
  /* Store only the host and port; a password never goes on the SD card. */
  if (!host[0] || !port_number()) {
    status = "Enter a host and a port from 1 to 65535";
    return;
  }
  if (svc->file_mkdir)
    svc->file_mkdir("/sd/roms/desktop");
  void *f = svc->file_open("/sd/roms/desktop/connection.txt", "wb");
  if (!f) {
    status = "Could not save host to SD card";
    return;
  }
  int ok = svc->file_write(host, 1, length(host), f) == length(host) &&
           svc->file_write("\n", 1, 1, f) == 1 &&
           svc->file_write(port_text, 1, length(port_text), f) ==
               length(port_text) &&
           svc->file_write("\n", 1, 1, f) == 1;
  if (svc->file_close(f))
    ok = 0;
  status = ok ? "Host saved. Password stays in memory only."
              : "Could not write the saved host";
}
static int connect_requested;
static void activate(int item) {
  media_mode = item == 9 || item == 10;
  if (item == 10) {
    browser_requested = 1;
    dashboard_mode = 0;
    listen_requested = 1;
  }
  if (item == 3)
    connect_requested = 1;
  if (item == 4)
    save_host();
  if (item == 5)
    quit = 1;
  if (item == 6) {
    dashboard_mode = 0;
    listen_requested = 1;
  }
  if (item == 7) {
    dashboard_mode = 0;
    relay_requested = 1;
  }
  if (item == 9) {
    dashboard_mode = 0;
    listen_requested = 1;
  }
  if (item == 8) {
    dashboard_mode = 1;
    listen_requested = 1;
  }
  dirty = 1;
}
static void home_touch(int x, int y) {
  if (sw >= 640 && y >= 6 && y < 40) {
    if (sw >= 1000 && x >= 240 && x < 380)
      activate(10);
    else if (sw >= 800 && x >= sw - 598 && x < sw - 458)
      activate(9);
    else if (x >= sw - 450 && x < sw - 310)
      activate(8);
    else if (x >= sw - 302 && x < sw - 162)
      activate(6);
    else if (x >= sw - 154 && x < sw - 14)
      activate(7);
    return;
  }
  int fh = (sh - 100) / 8;
  if (fh < 27)
    fh = 27;
  if (fh > 48)
    fh = 48;
  for (int i = 0; i < 3; i++)
    if (y >= 48 + i * (fh + 6) && y < 48 + i * (fh + 6) + fh) {
      selected = i;
      dirty = 1;
      return;
    }
  int by = 48 + 3 * (fh + 6), bw = (sw - 42) / 3;
  if (y >= by && y < by + fh) {
    for (int i = 0; i < 3; i++)
      if (x >= 14 + i * (bw + 7) && x < 14 + i * (bw + 7) + bw) {
        selected = 3 + i;
        activate(selected);
      }
    return;
  }
  if (y < key_y || key_h < 1 || y >= key_y + 5 * key_h || x < 14 ||
      x >= sw - 14)
    return;
  int row = (y - key_y) / key_h;
  if (row < 4) {
    int col = (x - 14) / ((sw - 28) / 12);
    if (col >= 12)
      return;
    char c = (shifted == 2 ? symbols : rows)[row][col];
    if (shifted == 1 && c >= 'a' && c <= 'z')
      c -= 32;
    type_char(c);
  } else {
    int col = (x - 14) * 4 / (sw - 28);
    if (col == 0)
      shifted = (shifted + 1) % 3;
    else if (col == 1)
      type_char(' ');
    else if (col == 2)
      type_char(127);
    else
      selected = (selected + 1) % 3;
    dirty = 1;
  }
}
static void poll_input(void) {
  in_input = 1;
  last_input = svc->get_time_us();
  pax_gamepad_state_t pad;
  zero(&pad, sizeof pad);
  svc->input_gamepad_read(&pad);
  unsigned buttons = 0;
  for (int i = 0; i < PAX_INPUT_MAX; i++)
    if (pad.values[i])
      buttons |= 1u << i;
  unsigned pressed = buttons & ~prev_pad;
  prev_pad = buttons;
  if (pad.values[PAX_INPUT_MENU]) {
    if (!menu_since)
      menu_since = last_input;
    if (last_input - menu_since >= 3000000 || pad.values[PAX_INPUT_X]) {
      quit = 1;
      back = 1;
    }
  } else
    menu_since = 0;
  if (!connected && sock < 0) {
    if (pressed & (1u << PAX_INPUT_DOWN)) {
      selected = (selected + 1) % 11;
      dirty = 1;
    }
    if (pressed & (1u << PAX_INPUT_UP)) {
      selected = (selected + 10) % 11;
      dirty = 1;
    }
    if (pressed & (1u << PAX_INPUT_A))
      activate(selected);
    if (pressed & (1u << PAX_INPUT_B))
      quit = 1;
  }
  int x = 0, y = 0, touched = svc->touch_read ? svc->touch_read(&x, &y) : 0;
  if (relay_active) {
    if (browser_active)
      browser_inputs(pressed, x, y, touched);
    else
      relay_inputs(x, y, touched);
    touch_before = touched;
    in_input = 0;
    return;
  }
  if (connected) {
    pax_touch_point_t points[2];
    int multi = svc->touch_read_points ? svc->touch_read_points(points, 2) : 0;
    if (multi >= 2) {
      if (!two_since)
        two_since = last_input;
      if (last_input - two_since > 1500000)
        back = 1;
    } else
      two_since = 0;
    if (touched && multi < 2) {
      mouse_x = x;
      mouse_y = y;
      pointer(mouse_buttons | 1);
    } else if (touch_before)
      pointer(mouse_buttons);
  } else if (sock < 0 && touched && !touch_before)
    home_touch(x, y);
  touch_before = touched;
  if (svc->input_mouse_read) {
    int dx = 0, dy = 0, mb = 0;
    if (svc->input_mouse_read(&dx, &dy, &mb)) {
      mouse_x += dx;
      mouse_y += dy;
      if (mouse_x < 0)
        mouse_x = 0;
      if (mouse_y < 0)
        mouse_y = 0;
      if (mouse_x >= sw)
        mouse_x = sw - 1;
      if (mouse_y >= sh)
        mouse_y = sh - 1;
      int old = mouse_buttons;
      mouse_buttons = (mb & 1) | ((mb & 2) << 1) | ((mb & 4) >> 1);
      if (connected)
        pointer(mouse_buttons | (touched ? 1 : 0));
      else if (sock < 0 && (mouse_buttons & 1) && !(old & 1))
        home_touch(mouse_x, mouse_y);
    }
  }
  if (connected && svc->input_mouse_scroll_read) {
    int wheel = 0;
    if (svc->input_mouse_scroll_read(&wheel) && wheel) {
      pointer(mouse_buttons | (wheel > 0 ? 8 : 16));
      pointer(mouse_buttons);
    }
  }
  if (svc->input_keyboard_read) {
    pax_keyboard_event_t event;
    int budget = 32;
    while (budget-- && svc->input_keyboard_read(&event)) {
      if (connected) {
        uint32_t sym = rfb_keysym(event.key);
        if (sym && rfb_key(&remote, sym, event.down))
          back = 1;
      } else if (event.down) {
        if (sock >= 0) {
          if (event.key == 27)
            back = 1;
          continue;
        }
        if (event.key == 27)
          quit = 1;
        else if (event.key == 9) {
          selected = (selected + 1) % 11;
          dirty = 1;
        } else if (event.key == 13) {
          if (selected < 2) {
            selected++;
            dirty = 1;
          } else if (selected == 2)
            activate(3);
          else
            activate(selected);
        } else
          type_char(event.key);
      }
    }
  }
  in_input = 0;
}
static void load_host(void) {
  char buf[160];
  void *f = svc->file_open("/sd/roms/desktop/connection.txt", "rb");
  if (!f)
    return;
  size_t n = svc->file_read(buf, 1, sizeof buf - 1, f);
  svc->file_close(f);
  buf[n] = 0;
  char *p = buf;
  while (*p && *p != '\n' && *p != '\r')
    p++;
  if (!*p)
    return;
  *p++ = 0;
  copy(host, buf, sizeof host);
  if (*p == '\n')
    p++;
  char *end = p;
  while (*end && *end != '\n' && *end != '\r')
    end++;
  *end = 0;
  if (length(p) < sizeof port_text)
    copy(port_text, p, sizeof port_text);
  if (!port_number())
    copy(port_text, "5900", sizeof port_text);
}
static void session(void) {
  back = 0;
  connect_requested = 0;
  unsigned port = port_number();
  if (!host[0] || !port) {
    status = "Enter a host and a port from 1 to 65535";
    return;
  }
  status = "Connecting... Hold MENU to cancel";
  dirty = 1;
  draw_home();
  uint32_t ip = 0;
  if (!svc->net_resolve(host, &ip) || !ip) {
    status = "Host lookup failed. Check network and hostname.";
    return;
  }
  sock = svc->net_tcp_connect(ip, (uint16_t)port);
  if (sock < 0) {
    status = "Could not open a network connection";
    return;
  }
  int64_t start = svc->get_time_us();
  int ready = 0;
  while (!quit && !back && svc->get_time_us() - start < 15000000) {
    int events = svc->net_poll(sock);
    if (events < 0 || (events & 4))
      break;
    if (events & 2) {
      ready = 1;
      break;
    }
    poll_input();
    svc->delay_ms(5);
  }
  if (!ready)
    status = "Connection failed. Check VNC host, port and firewall.";
  else {
    if (rfb_start(&remote, password))
      status = remote.error;
    else {
      zero(password, sizeof password);
      connected = 1;
      mouse_x = sw / 2;
      mouse_y = sh / 2;
      geometry();
      draw_remote();
      svc->log_printf("[desktop] Connected to %s (%ux%u)\n", remote.name,
                      remote.width, remote.height);
      while (!quit && !back) {
        poll_input();
        if (quit || back)
          break;
        int events = svc->net_poll(sock);
        if (events < 0 || (events & 4)) {
          status = "Desktop disconnected";
          break;
        }
        if (events & 1) {
          int result = rfb_update(&remote);
          if (result < 0) {
            status = remote.error;
            break;
          }
          if (result > 0)
            dirty = 1;
        }
        if (dirty && svc->get_time_us() - last_draw > 33000)
          draw_remote();
        svc->delay_ms(2);
      }
      if (back)
        status = "Disconnected. Password cleared; enter it to reconnect.";
    }
  }
  connected = 0;
  svc->net_udp_close(sock);
  sock = -1;
  rfb_close(&remote);
  zero(password, sizeof password);
  back = 0;
  two_since = 0;
  dirty = 1;
}
/* The app owns this listener only while relay mode is open. */
static int relay_receive(void *ctx, void *data, size_t n) {
  (void)ctx;
  return svc->net_tcp_recv(relay_peer, data, (int)n);
}
static int relay_send(void *ctx, const void *data, size_t n) {
  (void)ctx;
  return svc->net_tcp_send(relay_peer, data, (int)n);
}
static int64_t relay_now(void *ctx) {
  (void)ctx;
  return svc->get_time_us();
}
static int relay_yield(void *ctx) {
  (void)ctx;
  if (!in_input && svc->get_time_us() - last_input > 8000)
    poll_input();
  return quit || back;
}
static int audio_cancel(void *ctx) {
  return relay_yield(ctx) || relay.avi_stop;
}
static void relay_audio(void *ctx, const uint8_t *pcm, size_t n,
                        unsigned channels, unsigned rate) {
  (void)ctx;
  if (audio_buffer.task) {
    if (audio_buffer_write(&audio_buffer, pcm, n, channels, rate, audio_cancel,
                           0) &&
        !relay.avi_stop)
      back = 1;
    return;
  }
  static unsigned old_rate;
  if (!svc->audio_init || !svc->audio_submit)
    return;
  if (old_rate != rate) {
    svc->audio_init((int)rate);
    old_rate = rate;
  }
  short stereo[512];
  size_t frame = channels * 2;
  while (n && !quit && !back) {
    unsigned count = n / frame;
    if (count > 256)
      count = 256;
    for (unsigned i = 0; i < count; i++) {
      stereo[i * 2] = (int16_t)((unsigned)pcm[i * frame] |
                                (unsigned)pcm[i * frame + 1] << 8);
      stereo[i * 2 + 1] = channels == 2
                              ? (int16_t)((unsigned)pcm[i * frame + 2] |
                                          (unsigned)pcm[i * frame + 3] << 8)
                              : stereo[i * 2];
    }
    if (relay_muted)
      for (unsigned i = 0; i < count * 2; i++)
        stereo[i] = 0;
    svc->audio_submit(stereo, (int)count);
    pcm += count * frame;
    n -= count * frame;
    relay_yield(0);
  }
}
static void relay_inputs(int x, int y, int touched) {
  if (relay_peer < 0) {
    if (svc->input_keyboard_read) {
      pax_keyboard_event_t event;
      while (svc->input_keyboard_read(&event))
        if (event.down && event.key == 27)
          back = 1;
    }
    return;
  }
  pax_touch_point_t points[5];
  int count = svc->touch_read_points ? svc->touch_read_points(points, 5) : 0;
  if (count < 0)
    count = 0;
  if (count > 5)
    count = 5;
  if (!count && touched) {
    points[0] = (pax_touch_point_t){(int16_t)x, (int16_t)y, 0};
    count = 1;
  }
  if (count >= 2) {
    if (!two_since)
      two_since = last_input;
    if (last_input - two_since > 1500000)
      back = 1;
  } else
    two_since = 0;
  int ids[5], xs[5], ys[5];
  unsigned signature = 2166136261u;
  signature = (signature ^ (unsigned)count) * 16777619u;
  for (int i = 0; i < count; i++) {
    ids[i] = points[i].id;
    xs[i] = points[i].x;
    ys[i] = points[i].y;
    if (xs[i] < 0)
      xs[i] = 0;
    if (ys[i] < 0)
      ys[i] = 0;
    if (xs[i] >= sw)
      xs[i] = sw - 1;
    if (ys[i] >= sh)
      ys[i] = sh - 1;
    signature = (signature ^ (unsigned)ids[i]) * 16777619u;
    signature = (signature ^ (unsigned)xs[i]) * 16777619u;
    signature = (signature ^ (unsigned)ys[i]) * 16777619u;
  }
  if (relay.input_enabled && signature != relay_touch_signature) {
    relay_touch_signature = signature;
    if (relay_touch(&relay, ids, xs, ys, (unsigned)count))
      back = 1;
  }
  if (svc->input_mouse_read) {
    int dx = 0, dy = 0, buttons = 0;
    if (svc->input_mouse_read(&dx, &dy, &buttons)) {
      mouse_buttons = buttons;
      if (relay_mouse(&relay, dx, dy, 0, (unsigned)buttons))
        back = 1;
    }
  }
  if (svc->input_mouse_scroll_read) {
    int wheel = 0;
    if (svc->input_mouse_scroll_read(&wheel) && wheel)
      if (relay_mouse(&relay, 0, 0, wheel, (unsigned)mouse_buttons))
        back = 1;
  }
  if (svc->input_keyboard_read) {
    pax_keyboard_event_t event;
    int budget = 32;
    while (budget-- && svc->input_keyboard_read(&event))
      if (relay.wire_mode == 2) {
        if (!event.down)
          continue;
        if (event.key == ' ') {
          relay_media_pause(&relay, !relay.avi_paused);
          audio_buffer_pause(&audio_buffer, relay.avi_paused);
        }
        if (event.key == 27)
          relay_media_stop(&relay);
        if (event.key == 'm' || event.key == 'M') {
          relay_muted = !relay_muted;
          audio_buffer_mute(&audio_buffer, relay_muted);
        }
      } else if (relay_key(&relay, event.key, event.down))
        back = 1;
  }
}
static void relay_wait_screen(void) {
  fill(0, 0, sw, sh, 0x1084);
  text(18, 30,
       media_mode       ? "VIDEO / AUDIO / PORT 5000"
       : dashboard_mode ? "DASHBOARD / PORT 5000"
                        : "SVP RELAY / PORT 5000",
       2, 0x8e7f);
  text(18, 80, "WAITING FOR HOME ASSISTANT", sw >= 640 ? 2 : 1, 0xffff);
  text(18, 125, "Panel address: this ESP32-P4", 1, 0xc618);
  text(18, 150, "Stream port: 5000", 1, 0xc618);
  text(18, 180,
       media_mode ? "Home Assistant: send video to this SVP Relay player."
                  : "Configure the panel in SVP Relay, then send a dashboard",
       1, 0xc618);
  text(18, 200,
       media_mode ? "512x300 video / mono 16 kHz / PCM / buffered audio."
                  : "or request VNC with VIA RELAY on the connection screen.",
       1, 0xc618);
  if (media_mode && sh < 420)
    text(18, 225, "Menu: Pause / Stop / Mute. Space / Esc / M.", 1, 0xc618);
  if (dashboard_mode) {
    text(18, 240, "SVP Relay > this panel > Configure > Settings", 1, 0x8e7f);
    text(18, 265, "Set Dashboard shown while nothing plays to your path/URL.",
         1, 0xc618);
    text(18, 290, "Example only: /lovelace/0. Save while this screen is open.",
         1, 0xc618);
    text(18, 315, "Finish the relay's dashboard-user login setup for HA pages.",
         1, 0xc618);
    text(18, 340, "Shared menu > Workspace: Refresh page (F5) / Browser Back.", 1,
         0xc618);
  }
  if (media_mode && sh >= 420) {
    text(18, 240,
         "Send media to this panel's SVP Relay player in Home Assistant.", 1,
         0x8e7f);
    text(18, 265, "Stream: AVI / MJPEG video + 16-bit PCM audio (Auto or PCM).",
         1, 0xc618);
    text(18, 290, "Start at 5 video frames/s, JPEG q:v 15 for this connection.",
         1, 0xc618);
    text(18, 315, "Shared menu: Pause / Resume, Stop media, Mute / Unmute.", 1,
         0xc618);
    text(18, 340, "USB keyboard: Space pause; Esc stop; M mute.", 1, 0xc618);
  }
  text(18, sh - 42, "Hold MENU 3 s: exit. Shared menu > Workspace: disconnect.",
       1, 0xc618);
  text(18, sh - 21, status, 1, 0xffff);
  svc->display_flush();
  dirty = 0;
}
static int relay_pump(void) {
  if (quit || back)
    return 1;
  poll_input();
  if (quit || back)
    return 1;
  if (relay_peer < 0) {
    int accepted = svc->net_tcp_accept(relay_listener, 0, 0);
    if (accepted >= 0) {
      relay_peer = accepted;
      relay_io_t io = {0,       relay_receive, relay_send,  allocate,
                       release, relay_now,     relay_yield, relay_audio};
      if (relay_open_format(&relay, io, relay_pixels, (unsigned)sw,
                            (unsigned)sh, media_mode ? 16000 : 48000,
                            media_mode ? 1 : 2)) {
        status = relay.error;
        svc->net_udp_close(relay_peer);
        relay_peer = -1;
        back = 1;
        return 1;
      }
      relay_touch_signature = 0;
      status = "Relay connected; waiting for picture";
      dirty = 1;
      svc->log_printf("[desktop] SVP relay sender connected on port 5000\n");
    } else if (accepted != -2) {
      status = "Relay listener failed";
      back = 1;
      return 1;
    }
  }
  if (relay_peer >= 0) {
    int result = relay_step(&relay);
    if (result) {
      audio_buffer_finish(&audio_buffer, result > 0 && !relay.avi_stop);
      status = relay.error;
      svc->net_udp_close(relay_peer);
      relay_peer = -1;
      relay_close(&relay);
      dirty = 1;
    } else if (relay.changed) {
      dirty = 1;
      relay.changed = 0;
    }
  }
  if (dirty && svc->get_time_us() - last_draw > 33000) {
    if (browser_active)
      browser_draw();
    else if (relay_peer >= 0 && relay.frame_ready) {
      for (size_t i = 0; i < (size_t)sw * sh; i++)
        screen[i] = relay_pixels[i];
      relay_draw_cursor(&relay, screen);
      svc->display_flush();
      dirty = 0;
    } else
      relay_wait_screen();
    last_draw = svc->get_time_us();
  }
  return quit || back;
}
static void media_canvas(int small) {
  if (!svc->display_set_canvas)
    return;
  if (small && sw >= 800) {
    native_w = sw;
    native_h = sh;
    if (!svc->display_set_canvas(sw / 2, sh / 2)) {
      sw /= 2;
      sh /= 2;
      if (svc->display_set_scale)
        svc->display_set_scale(2.0f, 2.0f);
    } else
      native_w = native_h = 0;
  } else if (!small && native_w) {
    if (svc->display_set_scale)
      svc->display_set_scale(1.0f, 1.0f);
    svc->display_set_canvas(native_w, native_h);
    sw = native_w;
    sh = native_h;
    native_w = native_h = 0;
  }
  screen = svc->display_get_framebuffer();
}
static void relay_session(int request_vnc) {
  listen_requested = relay_requested = 0;
  back = 0;
  if (!svc->net_tcp_listen || !svc->net_tcp_accept) {
    status = "Update PAX firmware: TCP listener services are missing";
    return;
  }
  if (request_vnc && (!host[0] || !port_number())) {
    status = "Enter the VNC computer host, port and password first";
    return;
  }
  if (media_mode)
    media_canvas(1);
  relay_pixels = svc->mem_alloc((size_t)sw * sh * 2);
  if (!relay_pixels) {
    status = "Not enough memory for relay picture";
    media_canvas(0);
    return;
  }
  zero(relay_pixels, (size_t)sw * sh * 2);
  relay_listener = svc->net_tcp_listen(RELAY_PORT);
  if (relay_listener < 0) {
    svc->mem_free(relay_pixels);
    relay_pixels = 0;
    status = "Cannot listen on port 5000 (already in use?)";
    media_canvas(0);
    return;
  }
  if (audio_buffer_open(&audio_buffer, svc))
    svc->log_printf("[desktop] Audio task unavailable; using direct feed\n");
  audio_buffer_mute(&audio_buffer, relay_muted);
  relay_active = connected = 1;
  browser_active = browser_requested;
  browser_requested = 0;
  if (browser_active)
    browser_action = 1;
  two_since = 0;
  status = "Relay listener ready";
  relay_wait_screen();
  if (request_vnc) {
    const char *error =
        relay_start_vnc(svc, host, port_number(), password, relay_pump);
    zero(password, sizeof password);
    if (error) {
      status = error;
      back = 1;
    }
  }
  while (!relay_pump()) {
    browser_run();
    svc->delay_ms(2);
  }
  if (relay_peer >= 0) {
    if (relay.wire_mode == 2 && !relay.avi_ending) {
      relay_media_stop(&relay);
      for (unsigned i = 0; i < 50 && relay_step(&relay) == 0; i++)
        svc->delay_ms(2);
    }
    svc->net_udp_close(relay_peer);
    relay_peer = -1;
  }
  audio_buffer_close(&audio_buffer);
  relay_close(&relay);
  svc->net_udp_close(relay_listener);
  relay_listener = -1;
  svc->mem_free(relay_pixels);
  relay_pixels = 0;
  relay_active = connected = 0;
  browser_active = browser_action = 0;
  if (!quit)
    media_canvas(0);
  back = 0;
  two_since = 0;
  dirty = 1;
}
#include "media_ui.inc"
static void browser_free(void) {
  if (browser)
    svc->mem_free(browser);
  browser = 0;
}
static void menu_action(void *context, uint32_t action, int32_t direction) {
  (void)context;
  (void)direction;
  if (action == 0) {
    pax_game_menu_page_t page;
    zero(&page, sizeof page);
    copy(page.title, "Workspace", sizeof page.title);
    copy(page.status,
         relay_active ? "SVP Relay receiver"
         : connected  ? "VNC session connected"
                      : "Workspace connection",
         sizeof page.status);
    page.count = relay_active ? (relay.wire_mode == 2 ? 5 : 4)
                 : connected  ? 1
                              : 6;
    page.items[0].action = 1;
    copy(page.items[0].label,
         connected ? "Disconnect desktop" : "Exit Workspace",
         sizeof page.items[0].label);
    if (relay_active && relay.wire_mode == 2) {
      page.items[1].action = 8;
      copy(page.items[1].label,
           relay.avi_paused ? "Resume media" : "Pause media",
           sizeof page.items[1].label);
      page.items[2].action = 9;
      copy(page.items[2].label, "Stop media (return to dashboard)",
           sizeof page.items[2].label);
      page.items[3].action = 10;
      copy(page.items[3].label, relay_muted ? "Unmute media" : "Mute media",
           sizeof page.items[3].label);
    } else if (relay_active) {
      page.items[1].action = 4;
      copy(page.items[1].label, "Refresh page (F5)",
           sizeof page.items[1].label);
      page.items[2].action = 5;
      copy(page.items[2].label, "Browser Back (Alt+Left)",
           sizeof page.items[2].label);
      if (relay_peer < 0 || !relay.input_enabled) {
        page.items[1].flags = PAX_GAME_MENU_ITEM_DISABLED;
        page.items[2].flags = PAX_GAME_MENU_ITEM_DISABLED;
      }
    } else if (!connected) {
      page.items[1].action = 2;
      copy(page.items[1].label, "Listen for SVP Relay",
           sizeof page.items[1].label);
      page.items[2].action = 3;
      copy(page.items[2].label, "Connect VNC via relay",
           sizeof page.items[2].label);
      page.items[3].action = 6;
      copy(page.items[3].label, "Show dashboard", sizeof page.items[3].label);
      page.items[4].action = 7;
      copy(page.items[4].label, "Video / Audio via relay",
           sizeof page.items[4].label);
    }
    if (relay_active || !connected) {
      unsigned index = page.count - 1;
      page.items[index].action = 11;
      copy(page.items[index].label,
           browser_active ? "Show playback" : "Media browser (HA / SD)",
           sizeof page.items[index].label);
    }
    svc->game_menu_set_page(&page);
  } else if (action == 11) {
    if (relay_active) {
      browser_active = !browser_active;
      browser_action = browser_active ? 1 : 0;
      dirty = 1;
    } else if (!connected) {
      browser_requested = 1;
      media_mode = 1;
      dashboard_mode = 0;
      listen_requested = 1;
    }
    svc->game_menu_close();
  } else if (action == 1) {
    if (connected)
      back = 1;
    else
      quit = 1;
    svc->game_menu_close();
  } else if (action == 2 || action == 3 || action == 6 || action == 7) {
    dashboard_mode = action == 6;
    media_mode = action == 7;
    if (action == 3)
      relay_requested = 1;
    else
      listen_requested = 1;
    svc->game_menu_close();
  } else if (action >= 8 && action <= 10 && relay_active && relay_peer >= 0 &&
             relay.wire_mode == 2) {
    if (action == 8) {
      relay_media_pause(&relay, !relay.avi_paused);
      audio_buffer_pause(&audio_buffer, relay.avi_paused);
    }
    if (action == 9)
      relay_media_stop(&relay);
    if (action == 10) {
      relay_muted = !relay_muted;
      audio_buffer_mute(&audio_buffer, relay_muted);
    }
    svc->game_menu_close();
  } else if ((action == 4 || action == 5) && relay_active && relay_peer >= 0 &&
             relay.input_enabled) {
    int error = 0;
    if (action == 4) {
      error = relay_key(&relay, 139, 1) || relay_key(&relay, 139, 0);
    } else {
      const int alt_held = (relay.modifiers & 4u) != 0;
      if (!alt_held)
        error = relay_key(&relay, 132, 1);
      error |= relay_key(&relay, PAX_KEY_LEFT, 1);
      error |= relay_key(&relay, PAX_KEY_LEFT, 0);
      if (!alt_held)
        error |= relay_key(&relay, 132, 0);
    }
    if (error)
      back = 1;
    svc->game_menu_close();
  }
}
__attribute__((section(".text.entry"))) int
app_entry(const app_services_t *services) {
  svc = services;
  if (!svc || svc->abi_version != PAX_ABI_VERSION)
    return -1;
  if (svc->display_get_size && svc->display_set_canvas) {
    svc->display_get_size(&sw, &sh);
    if (svc->display_set_canvas(sw, sh)) {
      sw = 800;
      sh = 480;
    }
  }
  screen = svc->display_get_framebuffer();
  if (!screen)
    return -1;
  if (sw < 320 || sh < 240)
    return -1;
  if (!svc->net_tcp_connect || !svc->net_tcp_recv || !svc->net_tcp_send ||
      !svc->net_poll || !svc->net_resolve || !svc->net_udp_close) {
    status = "Update PAX firmware: TCP network services are missing";
    draw_home();
    svc->delay_ms(5000);
    return -1;
  }
  xmap = svc->mem_alloc((size_t)sw * sizeof *xmap);
  if (!xmap)
    return -1;
  remote.io = (rfb_io_t){0, receive, transmit, allocate, release};
  if (svc->game_menu_register && svc->game_menu_set_page &&
      svc->game_menu_close)
    svc->game_menu_register("Workspace", menu_action, 0);
  load_host();
  if (svc->app_get_arg) {
    char arg[160];
    if (svc->app_get_arg(arg, sizeof arg) > 0) {
      /* Launch argument: hostname:port, deliberately excludes passwords. */
      char *p = arg;
      while (*p && *p != ':')
        p++;
      if (*p) {
        *p++ = 0;
        copy(port_text, p, sizeof port_text);
      }
      copy(host, arg, sizeof host);
      if (length(arg) == 5 && arg[0] == 'm' && arg[1] == 'e' && arg[2] == 'd' &&
          arg[3] == 'i' && arg[4] == 'a') {
        browser_requested = media_mode = listen_requested = 1;
        host[0] = 0;
      }
      static const char dashboard_arg[] = "dashboard";
      unsigned equal = length(arg) == sizeof dashboard_arg - 1;
      for (unsigned i = 0; equal && i < sizeof dashboard_arg - 1; i++)
        equal = arg[i] == dashboard_arg[i];
      if (equal) {
        dashboard_mode = 1;
        listen_requested = 1;
        host[0] = 0;
      }
      if (length(arg) == 5 && arg[0] == 'r' && arg[1] == 'e' && arg[2] == 'l' &&
          arg[3] == 'a' && arg[4] == 'y') {
        listen_requested = 1;
        host[0] = 0;
      }
    }
  }
  while (!quit) {
    if (dirty)
      draw_home();
    poll_input();
    if (connect_requested)
      session();
    if (listen_requested || relay_requested)
      relay_session(relay_requested);

    svc->delay_ms(10);
  }
  if (svc->game_menu_register)
    svc->game_menu_register(0, 0, 0);
  if (sock >= 0)
    svc->net_udp_close(sock);
  rfb_close(&remote);
  zero(password, sizeof password);
  browser_free();
  svc->mem_free(xmap);
  xmap = 0;
  return 0;
}
