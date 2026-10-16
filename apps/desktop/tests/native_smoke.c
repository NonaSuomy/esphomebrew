/* Runs the complete app against a fragmented in-memory RFB server. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define app_entry desktop_entry
#include "../main.c"
#undef app_entry

static int64_t clock_us;
static unsigned char *incoming;
static size_t incoming_size, incoming_pos, sent;
static unsigned recv_calls, send_calls, flushes;
static int typed, frame_seen, remote_key_sent, pointer_sent, close_seen;
static const char *output_dir;
static uint16_t *display;
static int width = 1024, height = 600;
static void capture(const char *name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s-%dx%d.ppm", output_dir, name, width,
           height);
  FILE *f = fopen(path, "wb");
  assert(f);
  fprintf(f, "P6\n%d %d\n255\n", width, height);
  for (int i = 0; i < width * height; i++) {
    unsigned p = display[i];
    unsigned char rgb[3] = {(p >> 11) * 255 / 31, ((p >> 5) & 63) * 255 / 63,
                            (p & 31) * 255 / 31};
    assert(fwrite(rgb, 1, 3, f) == 3);
  }
  assert(!fclose(f));
}
static int64_t time_now(void) { return clock_us; }
static void delay(int ms) {
  clock_us += (int64_t)ms * 1000;
  assert(clock_us < 30000000);
}
static uint16_t *framebuffer(void) { return display; }
static void flush(void) {
  flushes++;
  if (flushes == 1)
    capture("connection");
  if (connected && remote.pixels && remote.pixels[0] == 0xf800) {
    frame_seen = 1;
    assert(remote.pixels[remote.width - 1] == 0x07e0);
    assert(remote.pixels[(remote.height - 1) * remote.width] == 0x001f);
    capture("desktop");
  }
}
static void size(int *w, int *h) {
  *w = width;
  *h = height;
}
static int canvas(int w, int h) {
  assert(w == width && h == height);
  return 0;
}
static void pad_read(pax_gamepad_state_t *p) { zero(p, sizeof *p); }
static int keys(pax_keyboard_event_t *e) {
  const int initial[] = {13, 13, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd', 13};
  if (sock < 0 && typed < (int)(sizeof initial / sizeof *initial)) {
    e->key = initial[typed++];
    e->down = 1;
    return 1;
  }
  if (frame_seen && remote_key_sent < 2) {
    e->key = 'a';
    e->down = remote_key_sent++ == 0;
    return 1;
  }
  return 0;
}
static int touch(int *x, int *y) {
  *x = width / 4;
  *y = height / 3;
  return frame_seen;
}
static int points(pax_touch_point_t *p, int n) {
  (void)p;
  (void)n;
  return frame_seen ? 2 : 0;
}
static int mouse(int *dx, int *dy, int *buttons) {
  if (!frame_seen || pointer_sent)
    return 0;
  *dx = 5;
  *dy = 6;
  *buttons = 1;
  pointer_sent = 1;
  return 1;
}
static int lookup(const char *name, uint32_t *ip) {
  assert(!strcmp(name, "test-desktop"));
  *ip = 0x7f000001;
  return 1;
}
static int connect_socket(uint32_t ip, uint16_t port) {
  assert(ip == 0x7f000001 && port == 5901);
  return 7;
}
static int poll_socket(int handle) {
  assert(handle == 7);
  return incoming_pos < incoming_size ? 3 : 2;
}
static int recv_socket(int handle, void *p, int n) {
  assert(handle == 7);
  if ((++recv_calls % 7) == 0)
    return -2;
  if (incoming_pos == incoming_size)
    return -2;
  size_t k = (size_t)n;
  if (k > 73)
    k = 73;
  if (k > incoming_size - incoming_pos)
    k = incoming_size - incoming_pos;
  memcpy(p, incoming + incoming_pos, k);
  incoming_pos += k;
  return (int)k;
}
static int send_socket(int handle, const void *p, int n) {
  assert(handle == 7);
  (void)p;
  if ((++send_calls % 5) == 0)
    return 0;
  int k = n < 3 ? n : 3;
  sent += (size_t)k;
  return k;
}
static void close_socket(int handle) {
  assert(handle == 7);
  close_seen = 1;
  quit = 1;
}
static int arg(char *out, int n) {
  const char *s = "test-desktop:5901";
  assert(n > 24);
  copy(out, s, n);
  return (int)strlen(s);
}
static void *no_file(const char *p, const char *mode) {
  (void)p;
  (void)mode;
  return NULL;
}
static int log_message(const char *fmt, ...) {
  (void)fmt;
  return 0;
}
static void byte(unsigned x) { incoming[incoming_size++] = (unsigned char)x; }
static void u16(unsigned n) {
  byte(n >> 8);
  byte(n);
}
static void u32(unsigned n) {
  byte(n >> 24);
  byte(n >> 16);
  byte(n >> 8);
  byte(n);
}
int main(int argc, char **argv) {
  assert(argc == 4);
  output_dir = argv[1];
  width = atoi(argv[2]);
  height = atoi(argv[3]);
  display = calloc((size_t)width * height, 2);
  assert(display);
  incoming = malloc(2 * 800 * 480 + 256);
  assert(incoming);
  const char *greeting = "RFB 003.008\n";
  for (int i = 0; i < 12; i++)
    byte(greeting[i]);
  byte(1);
  byte(2);
  for (int i = 0; i < 16; i++)
    byte(i);
  u32(0);
  u16(800);
  u16(480);
  for (int i = 0; i < 16; i++)
    byte(0);
  u32(4);
  byte('T');
  byte('e');
  byte('s');
  byte('t');
  byte(0);
  byte(0);
  u16(1);
  u16(0);
  u16(0);
  u16(800);
  u16(480);
  u32(0);
  for (unsigned y = 0; y < 480; y++)
    for (unsigned x = 0; x < 800; x++) {
      uint16_t c = y >= 240 ? 0x001f : x >= 400 ? 0x07e0 : 0xf800;
      byte(c);
      byte(c >> 8);
    }
  app_services_t api = {0};
  api.abi_version = PAX_ABI_VERSION;
  api.display_get_framebuffer = framebuffer;
  api.display_get_size = size;
  api.display_set_canvas = canvas;
  api.display_flush = flush;
  api.input_gamepad_read = pad_read;
  api.input_keyboard_read = keys;
  api.touch_read = touch;
  api.touch_read_points = points;
  api.input_mouse_read = mouse;
  api.mem_alloc = malloc;
  api.mem_free = free;
  api.get_time_us = time_now;
  api.delay_ms = delay;
  api.log_printf = log_message;
  api.net_resolve = lookup;
  api.net_tcp_connect = connect_socket;
  api.net_poll = poll_socket;
  api.net_tcp_recv = recv_socket;
  api.net_tcp_send = send_socket;
  api.net_udp_close = close_socket;
  api.file_open = no_file;
  api.app_get_arg = arg;
  assert(desktop_entry(&api) == 0);
  assert(frame_seen && remote_key_sent == 2 && pointer_sent && close_seen);
  assert(!remote.pixels && !password[0] && sent > 80 && recv_calls > 100 &&
         send_calls > 20);
  free(incoming);
  free(display);
  puts("Complete-app smoke: password handshake, fragmented IO, RGB565 display, "
       "input, cancel and cleanup passed");
  return 0;
}
