/* Browser implementation of the unchanged PAX service table. */
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include <SDL.h>
#include <dirent.h>
#include <emscripten.h>
#include <emscripten/fiber.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int width = 1024, height = 600, quit, volume = 75, brightness = 100;
static uint16_t *pixels;
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static pax_gamepad_state_t pad;
static pax_keyboard_event_t keys[128];
static unsigned head, tail;
static int tx, ty, touched, wheel;
static char argument[PAX_APP_ARG_MAX], resume[PAX_APP_ARG_MAX], rom[4096];
static app_services_t services;
extern int app_entry(const app_services_t *);

/* Fibers let the app's existing blocking loops and tasks yield to the browser.
 */
typedef struct {
  emscripten_fiber_t fiber;
  void *stack, *async;
  void (*fn)(void *);
  void *arg;
  double wake;
  int used, done;
} Task;
static Task tasks[16];
static emscripten_fiber_t root_fiber;
static unsigned char root_async[65536];
static Task *current;
static void delay(int ms) {
  current->wake = emscripten_get_now() + (ms > 0 ? ms : 1);
  emscripten_fiber_swap(&current->fiber, &root_fiber);
}
static void task_entry(void *arg) {
  Task *t = arg;
  t->fn(t->arg);
  t->done = 1;
  emscripten_fiber_swap(&t->fiber, &root_fiber);
  abort();
}
static int task_create(void (*fn)(void *), const char *name, uint32_t depth,
                       void *arg, int priority, void *out, int core) {
  (void)name;
  (void)priority;
  (void)core;
  for (int i = 0; i < 16; i++)
    if (!tasks[i].used) {
      Task *t = &tasks[i];
      memset(t, 0, sizeof(*t));
      t->used = 1;
      t->fn = fn;
      t->arg = arg;
      size_t size = depth < 1048576 ? 1048576 : depth;
      t->stack = malloc(size);
      t->async = malloc(65536);
      if (!t->stack || !t->async) {
        free(t->stack);
        free(t->async);
        t->used = 0;
        return -1;
      }
      emscripten_fiber_init(&t->fiber, task_entry, t, t->stack, size, t->async,
                            65536);
      if (out)
        *(void **)out = t;
      return 0;
    }
  return -1;
}
static void task_delete(void *handle) {
  Task *t = handle ? handle : current;
  t->done = 1;
  if (t == current) {
    emscripten_fiber_swap(&t->fiber, &root_fiber);
    abort();
  }
}
static int keycode(SDL_Keycode k) {
  switch (k) {
  case SDLK_UP:
    return PAX_KEY_UP;
  case SDLK_DOWN:
    return PAX_KEY_DOWN;
  case SDLK_LEFT:
    return PAX_KEY_LEFT;
  case SDLK_RIGHT:
    return PAX_KEY_RIGHT;
  case SDLK_RETURN:
    return 13;
  case SDLK_BACKSPACE:
    return 127;
  case SDLK_LCTRL:
  case SDLK_RCTRL:
    return 133;
  case SDLK_LALT:
  case SDLK_RALT:
    return 132;
  case SDLK_LSHIFT:
  case SDLK_RSHIFT:
    return 134;
  case SDLK_ESCAPE:
    return 27;
  default:
    return k >= 32 && k <= 126 ? k : 0;
  }
}
static void events(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT)
      quit = 1;
    if (e.type == SDL_MOUSEMOTION) {
      tx = e.motion.x;
      ty = e.motion.y;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
      tx = e.button.x;
      ty = e.button.y;
      touched = e.type == SDL_MOUSEBUTTONDOWN;
    }
    if (e.type == SDL_MOUSEWHEEL)
      wheel += e.wheel.y;
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
      int down = e.type == SDL_KEYDOWN, k = keycode(e.key.keysym.sym);
      if (k && head - tail < 128) {
        keys[head++ % 128] = (pax_keyboard_event_t){.key = k, .down = down};
      }
      if (e.key.keysym.sym == SDLK_ESCAPE)
        quit = down;
      int index = -1;
      switch (e.key.keysym.sym) {
      case SDLK_w:
        index = PAX_INPUT_UP;
        break;
      case SDLK_s:
        index = PAX_INPUT_DOWN;
        break;
      case SDLK_a:
        index = PAX_INPUT_LEFT;
        break;
      case SDLK_d:
        index = PAX_INPUT_RIGHT;
        break;
      case SDLK_j:
        index = PAX_INPUT_A;
        break;
      case SDLK_k:
        index = PAX_INPUT_B;
        break;
      case SDLK_q:
        index = PAX_INPUT_L;
        break;
      case SDLK_e:
        index = PAX_INPUT_R;
        break;
      case SDLK_F1:
        index = PAX_INPUT_START;
        break;
      case SDLK_TAB:
        index = PAX_INPUT_SELECT;
        break;
      }
      if (index >= 0)
        pad.values[index] = down;
    }
  }
}
static uint16_t *framebuffer(void) { return pixels; }
static void flush(void) {
  SDL_UpdateTexture(texture, NULL, pixels, width * 2);
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, NULL);
  SDL_RenderPresent(renderer);
}
static void clear(uint16_t color) {
  for (int i = 0; i < width * height; i++)
    pixels[i] = color;
  flush();
}
static void size_get(int *w, int *h) {
  if (w)
    *w = width;
  if (h)
    *h = height;
}
static int canvas(int w, int h) {
  if (w < 320 || h < 240 || w > 1920 || h > 1080)
    return -1;
  uint16_t *p = calloc((size_t)w * h, 2);
  if (!p)
    return -1;
  SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
  if (!t) {
    free(p);
    return -1;
  }
  free(pixels);
  if (texture)
    SDL_DestroyTexture(texture);
  pixels = p;
  texture = t;
  width = w;
  height = h;
  SDL_RenderSetLogicalSize(renderer, w, h);
  return 0;
}
static void rect(int x, int y, int w, int h, const uint16_t *data) {
  for (int r = 0; r < h; r++)
    if (y + r >= 0 && y + r < height)
      for (int c = 0; c < w; c++)
        if (x + c >= 0 && x + c < width)
          pixels[(y + r) * width + x + c] = data[r * w + c];
}
static void custom_frame(const uint16_t *data, uint16_t w, uint16_t h,
                         float scale, bool swap) {
  if (!w || !h || scale <= 0)
    return;
  int dw = (int)(w * scale), dh = (int)(h * scale);
  int ox = (width - dw) / 2, oy = (height - dh) / 2;
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++) {
      int sx = (int)((x - ox) / scale), sy = (int)((y - oy) / scale);
      if (x >= ox && y >= oy && x < ox + dw && y < oy + dh && sx >= 0 &&
          sy >= 0 && sx < w && sy < h) {
        uint16_t v = data[sy * w + sx];
        pixels[y * width + x] = swap ? (v >> 8) | (v << 8) : v;
      } else
        pixels[y * width + x] = 0;
    }
  flush();
}
static void frame(const uint16_t *data) {
  memcpy(pixels, data, (size_t)width * height * 2);
  flush();
}
static void gamepad(pax_gamepad_state_t *out) { *out = pad; }
static int keyboard(pax_keyboard_event_t *out) {
  if (head == tail)
    return 0;
  *out = keys[tail++ % 128];
  return 1;
}
// clang-format off: these macro bodies contain JavaScript.
EM_JS(int, touch, (int *x, int *y), {
  const queued = Module.paxTouchQueue ?.shift();
  if (queued)
    Module.paxTouch = queued;
  const t = Module.paxTouch || {x : 0, y : 0, down : false};
  if (x)
    HEAP32[x >> 2] = t.x;
  if (y)
    HEAP32[y >> 2] = t.y;
  return t.down ? 1 : 0;
});
static int l3(void) { return quit; }
static int scroll(int *out) {
  *out = wheel;
  wheel = 0;
  return *out != 0;
}
static int mouse(int *dx, int *dy, int *buttons) {
  *dx = *dy = *buttons = 0;
  return 0;
}
static int path_ok(const char *p) {
  if (!p || strncmp(p, "/sd", 3) || (p[3] && p[3] != '/') || strchr(p, '\\') ||
      strchr(p, ':'))
    return 0;
  const char *a = p;
  while (*a) {
    while (*a == '/')
      a++;
    const char *b = a;
    while (*a && *a != '/')
      a++;
    size_t n = a - b;
    if ((n == 1 && *b == '.') || (n == 2 && b[0] == '.' && b[1] == '.'))
      return 0;
  }
  return 1;
}
static void *open_file(const char *p, const char *m) {
  return path_ok(p) ? fopen(p, m) : NULL;
}
static int close_file(void *f) { return fclose(f); }
static size_t read_file(void *p, size_t s, size_t n, void *f) {
  return fread(p, s, n, f);
}
static size_t write_file(const void *p, size_t s, size_t n, void *f) {
  return fwrite(p, s, n, f);
}
static int seek_file(void *f, long o, int w) { return fseek(f, o, w); }
static long tell_file(void *f) { return ftell(f); }
static int make_dir(const char *p) {
  if (!path_ok(p))
    return -1;
  char b[4096];
  if (strlen(p) >= sizeof b)
    return -1;
  strcpy(b, p);
  for (char *c = b + 1; *c; c++)
    if (*c == '/') {
      *c = 0;
      mkdir(b, 0777);
      *c = '/';
    }
  mkdir(b, 0777);
  struct stat st;
  return stat(b, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}
static int listing(const char *p, char *out, int len) {
  if (!path_ok(p))
    return -1;
  DIR *d = opendir(p);
  if (!d)
    return -1;
  int count = 0, used = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", p, e->d_name);
    struct stat st;
    int folder = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    int n = strlen(e->d_name);
    if (used + n + folder + 1 > len)
      continue;
    memcpy(out + used, e->d_name, n);
    used += n;
    if (folder)
      out[used++] = '/';
    out[used++] = 0;
    count++;
  }
  closedir(d);
  return count;
}
static int file_info(const char *p, pax_file_stat_t *out) {
  memset(out, 0, sizeof(*out));
  struct stat st;
  if (!path_ok(p) || stat(p, &st))
    return -1;
  out->size = st.st_size;
  out->mtime = st.st_mtime;
  out->is_dir = S_ISDIR(st.st_mode);
  return 0;
}
static int remove_file(const char *p) {
  return path_ok(p) && strcmp(p, "/sd") ? remove(p) : -1;
}
static int rename_file(const char *a, const char *b) {
  struct stat st;
  return path_ok(a) && path_ok(b) && stat(b, &st) != 0 ? rename(a, b) : -1;
}
static void *caps(size_t n, uint32_t c) {
  (void)c;
  return malloc(n);
}
static int64_t clock_us(void) { return (int64_t)(emscripten_get_now() * 1000); }
static int ui(pax_ui_call_fn_t fn, void *a, uint32_t timeout) {
  (void)timeout;
  return fn(a);
}
static char *rom_get(void) { return rom[0] ? strdup(rom) : NULL; }
static void rom_set(const char *p) {
  snprintf(rom, sizeof rom, "%s", p ? p : "");
}
static int32_t volume_get(void) { return volume; }
static void volume_set(int32_t v) { volume = v; }
static int32_t brightness_get(void) { return brightness; }
static void brightness_set(int32_t v) { brightness = v; }
static int arg_get(char *out, int n) {
  if (n > 0)
    snprintf(out, n, "%s", argument);
  return strlen(argument);
}
static int resume_set(const char *a) {
  if (strlen(a) >= sizeof resume)
    return -1;
  strcpy(resume, a);
  EM_ASM({ Module.paxResume = UTF8ToString($0); }, a);
  return 0;
}
EM_JS(int, handoff, (const char *p, const char *a, int back),
      { return Module.paxOpen(UTF8ToString(p), UTF8ToString(a), back); });
static int app_open(const char *p, const char *a, int back) {
  int result = handoff(p, a ? a : "", back);
  if (!result)
    quit = 1;
  return result;
}
static int auth(char *b, size_t n) {
  if (n)
    b[0] = 0;
  return 0;
}
static int storage(int32_t kind, pax_storage_status_t *out) {
  memset(out, 0, sizeof(*out));
  if (kind != PAX_STORAGE_SD)
    return 0;
  out->available = out->mounted = 1;
  strcpy(out->mount_path, "/sd");
  return 0;
}
static int network(pax_network_config_t *out) {
  memset(out, 0, sizeof(*out));
  out->connected = EM_ASM_INT({ return navigator.onLine ? 1 : 0; });
  return 0;
}
static int status(pax_device_status_t *out) {
  memset(out, 0, sizeof(*out));
  time_t now = time(NULL);
  strftime(out->local_time, sizeof out->local_time, "%H:%M", localtime(&now));
  out->battery_percent = -1;
  return 0;
}
/* Browser HTTP transport only, not arbitrary TCP. Fetch enforces CORS and TLS.
 */
EM_JS(int, resolve, (const char *host, uint32_t *ip), {
  const name = UTF8ToString(host);
  Module.paxHosts = Module.paxHosts || [];
  Module.paxHostIPs = Module.paxHostIPs || {};
  let i = Module.paxHosts.indexOf(name);
  if (i < 0)
    i = Module.paxHosts.push(name) - 1;
  const address = (0xf0000000 | (i + 1)) >>> 0;
  Module.paxHostIPs[address] = name;
  HEAPU32[ip >> 2] = address;
  return 1;
});
EM_JS(int, connect_http, (uint32_t ip, uint16_t port), {
  const host = Module.paxHostIPs
      ?.[ip >>> 0] ||
           [ ip >>> 24, (ip >>> 16) & 255, (ip >>> 8) & 255, ip & 255 ].join(
               '.');
  return Module.paxConnect(host, port, false);
});
EM_JS(int, connect_https, (const char *host, uint16_t port),
      { return Module.paxConnect(UTF8ToString(host), port, true); });
EM_JS(int, send_http, (int h, const void *p, int n),
      { return Module.paxSend(h, HEAPU8.slice(p, p + n)); });
EM_JS(int, recv_http, (int h, void *p, int n),
      { return Module.paxRecv(h, p, n, false, HEAPU8); });
EM_JS(int, recv_https, (int h, void *p, int n),
      { return Module.paxRecv(h, p, n, true, HEAPU8); });
EM_JS(int, poll_http, (int h), {
  const s = Module.paxSockets[h];
  return !s ? -1 : s.error ? 4 : 2 | (s.response ? 1 : 0);
});
EM_JS(int, tls_status, (int h), {
  const s = Module.paxSockets[h];
  return !s || s.error ? -1 : 1;
});
EM_JS(void, close_http, (int h), {
  const s = Module.paxSockets[h];
  if (s)
    s.abort.abort();
  delete Module.paxSockets[h];
});
// clang-format on
static void noop(void) {}
static void run_app(void *unused) {
  (void)unused;
  int result = app_entry(&services);
  printf("PAX app returned %d\n", result);
}
EMSCRIPTEN_KEEPALIVE void pax_stop(void) { quit = 1; }
int main(int argc, char **argv) {
  if (argc > 1)
    snprintf(argument, sizeof argument, "%s", argv[1]);
  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
  window = SDL_CreateWindow("PAX Web", 0, 0, width, height, SDL_WINDOW_SHOWN);
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer || canvas(width, height))
    return 1;
  services = (app_services_t){.abi_version = PAX_ABI_VERSION,
                              .display_get_framebuffer = framebuffer,
                              .display_get_emu_buffer = framebuffer,
                              .display_flush = flush,
                              .display_emu_flush = flush,
                              .display_clear = clear,
                              .display_write_frame_rgb565 = frame,
                              .display_write_frame_custom = custom_frame,
                              .display_write_rect = rect,
                              .display_lock = noop,
                              .display_unlock = noop,
                              .input_gamepad_read = gamepad,
                              .input_keyboard_read = keyboard,
                              .touch_read = touch,
                              .input_l3_read = l3,
                              .input_mouse_read = mouse,
                              .input_mouse_scroll_read = scroll,
                              .file_open = open_file,
                              .file_close = close_file,
                              .file_read = read_file,
                              .file_write = write_file,
                              .file_seek = seek_file,
                              .file_tell = tell_file,
                              .file_list_dir = listing,
                              .file_mkdir = make_dir,
                              .file_stat = file_info,
                              .file_remove = remove_file,
                              .file_rename = rename_file,
                              .mem_alloc = malloc,
                              .mem_calloc = calloc,
                              .mem_realloc = realloc,
                              .mem_free = free,
                              .mem_caps_alloc = caps,
                              .log_printf = printf,
                              .log_vprintf = vprintf,
                              .delay_ms = delay,
                              .get_time_us = clock_us,
                              .settings_rom_path_get = rom_get,
                              .settings_rom_path_set = rom_set,
                              .settings_volume_get = volume_get,
                              .settings_volume_set = volume_set,
                              .settings_brightness_get = brightness_get,
                              .settings_brightness_set = brightness_set,
                              .task_create = task_create,
                              .task_delete = task_delete,
                              .display_get_size = size_get,
                              .display_set_canvas = canvas,
                              .ui_call_sync = ui,
                              .app_open = app_open,
                              .app_get_arg = arg_get,
                              .app_set_resume_arg = resume_set,
                              .device_status_get = status,
                              .network_config_get = network,
                              .storage_status_get = storage,
                              .archive_auth_get = auth,
                              .net_resolve = resolve,
                              .net_tcp_connect = connect_http,
                              .net_tcp_send = send_http,
                              .net_tcp_recv = recv_http,
                              .net_poll = poll_http,
                              .net_udp_close = close_http,
                              .net_tls_connect = connect_https,
                              .net_tls_status = tls_status,
                              .net_tls_send = send_http,
                              .net_tls_recv = recv_https,
                              .net_tls_close = close_http};
  emscripten_fiber_init_from_current_context(&root_fiber, root_async,
                                             sizeof root_async);
  if (task_create(run_app, "app", 1048576, NULL, 0, NULL, 0))
    return 1;
  while (!tasks[0].done) {
    events();
    double now = emscripten_get_now();
    for (int i = 0; i < 16; i++)
      if (tasks[i].used && !tasks[i].done && tasks[i].wake <= now) {
        current = &tasks[i];
        emscripten_fiber_swap(&root_fiber, &current->fiber);
      }
    emscripten_sleep(1);
  }
  SDL_Quit();
  for (int i = 0; i < 16; i++) {
    free(tasks[i].stack);
    free(tasks[i].async);
  }
  free(pixels);
  pixels = NULL;
  EM_ASM({ Module.paxFinished(); });
  return 0;
}
