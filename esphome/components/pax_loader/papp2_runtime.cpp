#include "papp2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// This tag completes the opaque C ABI type declared by papp2_abi.h.
struct papp_audio_stream {
  bool used{false};
  bool paused{false};
  bool playing{false};
  uint8_t channels{0};
  uint8_t format{0};
  uint32_t rate{0};
  int32_t gain{256};
  int16_t *ring{nullptr};
  uint32_t capacity{0};
  uint32_t head{0};
  uint32_t tail{0};
  uint32_t step{0};
  uint32_t frac{0x10000};
  int16_t previous[2]{};
  int16_t current[2]{};
  uint32_t underruns{0};
  uint64_t played{0};
  SemaphoreHandle_t space{nullptr};
  SemaphoreHandle_t api_lock{nullptr};
};

namespace {

static const app_services_t *s_services;
static constexpr uint32_t AUDIO_RATE = 48000;
static constexpr uint32_t AUDIO_BLOCK = 240;
static constexpr size_t AUDIO_STREAMS = 4;
static constexpr size_t INPUT_EVENT_CAPACITY = 64;
static constexpr uint32_t THREAD_MAGIC = 0x32524854;  // THR2
static constexpr uint32_t OBJECT_MAGIC = 0x324A424F;  // OBJ2

struct papp_thread {
  uint32_t magic{0};
  bool used{false};
  TaskHandle_t task{nullptr};
  SemaphoreHandle_t done{nullptr};
  papp_thread_fn_t fn{nullptr};
  void *arg{nullptr};
  void *tls{nullptr};
  int exit_code{0};
};

enum : uint8_t { OBJ_MUTEX = 1, OBJ_RECURSIVE_MUTEX, OBJ_SEM, OBJ_QUEUE };
struct papp_sync_object {
  uint32_t magic{OBJECT_MAGIC};
  uint8_t kind{0};
  QueueHandle_t handle{nullptr};
  papp_sync_object *next{nullptr};
};

struct BrowserEntry {
  std::string name;
  std::string path;
  bool directory{false};
};

static SemaphoreHandle_t s_audio_lock;
static papp_audio_stream s_audio_streams[AUDIO_STREAMS];
static TaskHandle_t s_audio_task;
static volatile bool s_audio_shutdown;
static int32_t s_mix[AUDIO_BLOCK * 2];
static int16_t s_output[AUDIO_BLOCK * 2];

static portMUX_TYPE s_thread_mux = portMUX_INITIALIZER_UNLOCKED;
static papp_thread s_threads[PAPP_MAX_THREADS];
static TaskHandle_t s_main_task;
static void *s_main_tls;
static volatile bool s_runtime_stopping;

static portMUX_TYPE s_object_mux = portMUX_INITIALIZER_UNLOCKED;
static papp_sync_object *s_objects;

static papp_input_event_t s_events[INPUT_EVENT_CAPACITY];
static size_t s_event_head, s_event_tail;
static bool s_key_state[256];
static uint16_t s_key_modifiers;
static uint32_t s_previous_buttons;
static bool s_input_connected;

static bool exiting() {
  return s_runtime_stopping || s_services == nullptr || s_services->exit_requested == nullptr ||
         s_services->exit_requested();
}

static TickType_t timeout_slice(uint32_t ms) {
  if (ms == 0) return 0;
  TickType_t ticks = pdMS_TO_TICKS(ms);
  return ticks ? ticks : 1;
}

static uint16_t hid_usage_from_key(int key) {
  if (key >= 'a' && key <= 'z') return static_cast<uint16_t>(0x04 + key - 'a');
  if (key >= 'A' && key <= 'Z') return static_cast<uint16_t>(0x04 + key - 'A');
  if (key >= '1' && key <= '9') return static_cast<uint16_t>(0x1E + key - '1');
  if (key == '0') return 0x27;
  if (key == 13) return 0x28;
  if (key == 27) return 0x29;
  if (key == 127 || key == 8) return 0x2A;
  if (key == 9) return 0x2B;
  if (key == ' ') return 0x2C;
  if (key == '-') return 0x2D;
  if (key == '=') return 0x2E;
  if (key == '[') return 0x2F;
  if (key == ']') return 0x30;
  if (key == '\\') return 0x31;
  if (key == ';') return 0x33;
  if (key == '\'') return 0x34;
  if (key == '`') return 0x35;
  if (key == ',') return 0x36;
  if (key == '.') return 0x37;
  if (key == '/') return 0x38;
  if (key >= 135 && key <= 146) return static_cast<uint16_t>(0x3A + key - 135);
  if (key == PAX_KEY_UP) return 0x52;
  if (key == PAX_KEY_DOWN) return 0x51;
  if (key == PAX_KEY_LEFT) return 0x50;
  if (key == PAX_KEY_RIGHT) return 0x4F;
  if (key == 147) return 0x49;
  if (key == 148) return 0x4C;
  if (key == 149) return 0x4E;
  if (key == 150) return 0x4B;
  if (key == 151) return 0x4A;
  if (key == 152) return 0x4D;
  return 0;
}

static uint16_t modifier_for_key(int key) {
  if (key == 133) return PAPP_MOD_LCTRL;
  if (key == 134) return PAPP_MOD_LSHIFT;
  if (key == 132) return PAPP_MOD_LALT;
  return 0;
}

static void queue_event(const papp_input_event_t &event) {
  const size_t next = (s_event_head + 1) % INPUT_EVENT_CAPACITY;
  if (next == s_event_tail) s_event_tail = (s_event_tail + 1) % INPUT_EVENT_CAPACITY;
  s_events[s_event_head] = event;
  s_event_head = next;
}

static void pump_keyboard_events() {
  if (s_services == nullptr || s_services->input_keyboard_read == nullptr) return;
  pax_keyboard_event_t key{};
  while (s_services->input_keyboard_read(&key)) {
    const uint16_t usage = hid_usage_from_key(key.key);
    const uint16_t mod = modifier_for_key(key.key);
    if (mod) {
      if (key.down) s_key_modifiers |= mod;
      else s_key_modifiers &= static_cast<uint16_t>(~mod);
    }
    if (usage == 0) continue;
    s_key_state[usage] = key.down != 0;
    papp_input_event_t event{};
    event.type = key.down ? PAPP_EV_KEY_DOWN : PAPP_EV_KEY_UP;
    event.device = 0xFF;
    event.code = usage;
    event.mods = s_key_modifiers;
    event.text = key.down && key.key > 0 && key.key < 128 ? static_cast<uint16_t>(key.key) : 0;
    event.time_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    queue_event(event);
  }
}

static uint32_t read_buttons() {
  pax_gamepad_state_t state{};
  if (s_services && s_services->input_gamepad_read) s_services->input_gamepad_read(&state);
  uint32_t out = 0;
  const auto set = [&](int index, uint32_t bit) { if (state.values[index]) out |= bit; };
  set(PAX_INPUT_A, PAPP_BTN_A); set(PAX_INPUT_B, PAPP_BTN_B);
  set(PAX_INPUT_X, PAPP_BTN_X); set(PAX_INPUT_Y, PAPP_BTN_Y);
  set(PAX_INPUT_L, PAPP_BTN_L1); set(PAX_INPUT_R, PAPP_BTN_R1);
  set(PAX_INPUT_SELECT, PAPP_BTN_SELECT); set(PAX_INPUT_START, PAPP_BTN_START);
  set(PAX_INPUT_MENU, PAPP_BTN_HOME);
  set(PAX_INPUT_UP, PAPP_BTN_UP); set(PAX_INPUT_DOWN, PAPP_BTN_DOWN);
  set(PAX_INPUT_LEFT, PAPP_BTN_LEFT); set(PAX_INPUT_RIGHT, PAPP_BTN_RIGHT);
  return out;
}

static bool input_gamepad(int player, papp_gamepad_t *out) {
  if (out == nullptr || player < 0 || player >= PAPP_MAX_PLAYERS) return false;
  std::memset(out, 0, sizeof(*out));
  if (player != 0) return false;
  const uint32_t buttons = read_buttons();
  out->buttons = buttons;
  out->connected = buttons != 0 || s_input_connected;
  s_input_connected = out->connected != 0;
  if (buttons & PAPP_BTN_LEFT) out->lx = -32767;
  else if (buttons & PAPP_BTN_RIGHT) out->lx = 32767;
  if (buttons & PAPP_BTN_UP) out->ly = -32767;
  else if (buttons & PAPP_BTN_DOWN) out->ly = 32767;
  return out->connected != 0;
}

static bool input_key_down(uint16_t usage) {
  pump_keyboard_events();
  return usage < 256 && s_key_state[usage];
}

static uint16_t input_modifiers() {
  pump_keyboard_events();
  return s_key_modifiers;
}

static bool input_poll_event(papp_input_event_t *out) {
  if (out == nullptr) return false;
  pump_keyboard_events();
  const uint32_t buttons = read_buttons();
  const uint32_t changed = buttons ^ s_previous_buttons;
  if (changed != 0) {
    const unsigned bit = __builtin_ctz(changed);
    s_previous_buttons = (s_previous_buttons & ~(1u << bit)) | (buttons & (1u << bit));
    out->type = (buttons & (1u << bit)) ? PAPP_EV_BUTTON_DOWN : PAPP_EV_BUTTON_UP;
    out->device = 0;
    out->code = static_cast<uint16_t>(bit);
    out->time_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    return true;
  }
  if (s_event_tail == s_event_head) return false;
  *out = s_events[s_event_tail];
  s_event_tail = (s_event_tail + 1) % INPUT_EVENT_CAPACITY;
  return true;
}

static void input_flush() {
  if (s_services && s_services->input_keyboard_read) {
    pax_keyboard_event_t ignored{};
    while (s_services->input_keyboard_read(&ignored)) {}
  }
  s_event_head = s_event_tail = 0;
  s_previous_buttons = read_buttons();
  std::memset(s_key_state, 0, sizeof(s_key_state));
  s_key_modifiers = 0;
}

static int input_analog(int channel) {
  if (channel != PAPP_ANALOG_PADDLE || s_services == nullptr || s_services->paddle_read == nullptr) return -1;
  const int raw = s_services->paddle_read();
  return raw < 0 ? -1 : std::max(0, std::min(65535, raw * 65535 / 4095));
}

static const papp_if_inpt_t s_inpt = {
    {PAPP_IF_INPT, PAPP_IF_INPT_VER, sizeof(papp_if_inpt_t)}, input_gamepad, input_key_down,
    input_modifiers, input_poll_event, input_flush, input_analog};

static void audio_render_stream(papp_audio_stream *s, int frames) {
  for (int i = 0; i < frames; ++i) {
    while (s->frac >= 0x10000) {
      const uint32_t tail = s->tail;
      if (tail == __atomic_load_n(&s->head, __ATOMIC_ACQUIRE)) {
        if (s->playing) { ++s->underruns; s->playing = false; }
        return;
      }
      const int16_t *frame = s->ring + (tail % s->capacity) * 2;
      s->previous[0] = s->current[0]; s->previous[1] = s->current[1];
      s->current[0] = frame[0]; s->current[1] = frame[1];
      __atomic_store_n(&s->tail, tail + 1, __ATOMIC_RELEASE);
      ++s->played;
      s->playing = true;
      s->frac -= 0x10000;
    }
    const int32_t t = static_cast<int32_t>(s->frac >> 1);
    const int32_t left = s->previous[0] + (((s->current[0] - s->previous[0]) * t) >> 15);
    const int32_t right = s->previous[1] + (((s->current[1] - s->previous[1]) * t) >> 15);
    s_mix[i * 2] += (left * s->gain) >> 8;
    s_mix[i * 2 + 1] += (right * s->gain) >> 8;
    s->frac += s->step;
  }
}

static void audio_mixer_task(void *) {
  for (;;) {
    if (s_audio_shutdown) break;
    std::memset(s_mix, 0, sizeof(s_mix));
    bool any = false;
    bool playing = false;
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    for (auto &stream : s_audio_streams) {
      if (!stream.used) continue;
      any = true;
      if (stream.paused) continue;
      playing = true;
      const uint32_t before = stream.tail;
      audio_render_stream(&stream, AUDIO_BLOCK);
      if (stream.tail != before && stream.space) xSemaphoreGive(stream.space);
    }
    xSemaphoreGive(s_audio_lock);
    if (!any || !playing) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      continue;
    }
    for (size_t i = 0; i < AUDIO_BLOCK * 2; ++i)
      s_output[i] = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, s_mix[i])));
    if (s_services && s_services->audio_submit)
      s_services->audio_submit(s_output, AUDIO_BLOCK);
  }
  s_audio_task = nullptr;
  if (s_services && s_services->task_delete) s_services->task_delete(nullptr);
  for (;;) vTaskDelay(portMAX_DELAY);
}

static uint32_t audio_output_rate() { return AUDIO_RATE; }

static papp_audio_stream_t *audio_open(const papp_audio_cfg_t *cfg) {
  if (cfg == nullptr || cfg->rate < 4000 || cfg->rate > 192000 ||
      (cfg->channels != 1 && cfg->channels != 2) ||
      (cfg->format != PAPP_AUDIO_S16 && cfg->format != PAPP_AUDIO_U8) || exiting()) return nullptr;
  uint32_t latency = cfg->latency_ms == 0 ? 100 : std::max<uint32_t>(20, std::min<uint32_t>(2000, cfg->latency_ms));
  const uint64_t cap64 = static_cast<uint64_t>(cfg->rate) * latency / 1000;
  if (cap64 == 0 || cap64 > UINT32_MAX || !s_audio_lock) return nullptr;
  xSemaphoreTake(s_audio_lock, portMAX_DELAY);
  papp_audio_stream *stream = nullptr;
  for (auto &candidate : s_audio_streams) if (!candidate.used) { stream = &candidate; break; }
  if (stream == nullptr) { xSemaphoreGive(s_audio_lock); return nullptr; }
  const size_t bytes = static_cast<size_t>(cap64) * 2 * sizeof(int16_t);
  auto *ring = static_cast<int16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (ring == nullptr) ring = static_cast<int16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  SemaphoreHandle_t space = xSemaphoreCreateBinary();
  SemaphoreHandle_t api_lock = xSemaphoreCreateMutex();
  if (ring == nullptr || space == nullptr || api_lock == nullptr) {
    heap_caps_free(ring);
    if (space) vSemaphoreDelete(space);
    if (api_lock) vSemaphoreDelete(api_lock);
    xSemaphoreGive(s_audio_lock);
    return nullptr;
  }
  *stream = papp_audio_stream{};
  stream->used = true;
  stream->channels = cfg->channels;
  stream->format = cfg->format;
  stream->rate = cfg->rate;
  stream->gain = 256;
  stream->ring = ring;
  stream->capacity = static_cast<uint32_t>(cap64);
  stream->step = static_cast<uint32_t>((static_cast<uint64_t>(cfg->rate) << 16) / AUDIO_RATE);
  stream->frac = 0x10000;
  stream->space = space;
  stream->api_lock = api_lock;
  const bool need_task = s_audio_task == nullptr;
  xSemaphoreGive(s_audio_lock);
  if (s_services && s_services->audio_init) s_services->audio_init(AUDIO_RATE);
  if (need_task && s_services && s_services->task_create) {
    if (s_services->task_create(audio_mixer_task, "papp2_audio", 4096, nullptr, 4, &s_audio_task, 0) != 0) {
      xSemaphoreTake(s_audio_lock, portMAX_DELAY);
      stream->used = false;
      xSemaphoreGive(s_audio_lock);
      heap_caps_free(stream->ring);
      vSemaphoreDelete(stream->space); vSemaphoreDelete(stream->api_lock);
      *stream = papp_audio_stream{};
      return nullptr;
    }
  }
  if (s_audio_task) xTaskNotifyGive(s_audio_task);
  return stream;
}

static bool audio_valid(papp_audio_stream_t *s) {
  return s >= s_audio_streams && s < s_audio_streams + AUDIO_STREAMS && s->used;
}

static void audio_close(papp_audio_stream_t *s) {
  if (!audio_valid(s)) return;
  xSemaphoreTake(s->api_lock, portMAX_DELAY);
  xSemaphoreTake(s_audio_lock, portMAX_DELAY);
  s->used = false;
  int16_t *ring = s->ring;
  SemaphoreHandle_t space = s->space;
  SemaphoreHandle_t api = s->api_lock;
  *s = papp_audio_stream{};
  xSemaphoreGive(s_audio_lock);
  xSemaphoreGive(api);
  if (ring) heap_caps_free(ring);
  if (space) vSemaphoreDelete(space);
  if (api) vSemaphoreDelete(api);
  if (s_audio_task) xTaskNotifyGive(s_audio_task);
}

static size_t audio_write(papp_audio_stream_t *s, const void *data, size_t frames, uint32_t timeout_ms) {
  if (!audio_valid(s) || data == nullptr || exiting()) return 0;
  xSemaphoreTake(s->api_lock, portMAX_DELAY);
  const TickType_t start = xTaskGetTickCount();
  const TickType_t limit = timeout_ms == PAPP_WAIT_FOREVER ? portMAX_DELAY : timeout_slice(timeout_ms);
  size_t done = 0;
  const auto *src = static_cast<const uint8_t *>(data);
  const size_t bytes_per_sample = s->format == PAPP_AUDIO_U8 ? 1 : 2;
  const size_t bytes_per_frame = bytes_per_sample * s->channels;
  while (done < frames && !exiting()) {
    const uint32_t head = s->head;
    const uint32_t tail = __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE);
    const uint32_t room = s->capacity - (head - tail);
    if (room == 0) {
      if (timeout_ms == 0 || (limit != portMAX_DELAY && xTaskGetTickCount() - start >= limit)) break;
      xSemaphoreTake(s->space, pdMS_TO_TICKS(10));
      continue;
    }
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(frames - done, room));
    for (uint32_t i = 0; i < n; ++i) {
      int16_t left, right;
      const uint8_t *p = src + (done + i) * bytes_per_frame;
      if (s->format == PAPP_AUDIO_U8) {
        left = static_cast<int16_t>((static_cast<int>(p[0]) - 128) << 8);
        right = s->channels == 2 ? static_cast<int16_t>((static_cast<int>(p[1]) - 128) << 8) : left;
      } else {
        int16_t sample[2]{};
        std::memcpy(sample, p, bytes_per_frame);
        left = sample[0]; right = s->channels == 2 ? sample[1] : left;
      }
      s->ring[((head + i) % s->capacity) * 2] = left;
      s->ring[((head + i) % s->capacity) * 2 + 1] = right;
    }
    __atomic_store_n(&s->head, head + n, __ATOMIC_RELEASE);
    done += n;
    if (s_audio_task) xTaskNotifyGive(s_audio_task);
  }
  xSemaphoreGive(s->api_lock);
  return done;
}

static void audio_pause(papp_audio_stream_t *s, bool pause) {
  if (!audio_valid(s)) return;
  s->paused = pause;
  if (s_audio_task) xTaskNotifyGive(s_audio_task);
}

static void audio_gain(papp_audio_stream_t *s, int percent) {
  if (!audio_valid(s)) return;
  s->gain = std::max(0, std::min(100, percent)) * 256 / 100;
}

static void audio_clear(papp_audio_stream_t *s) {
  if (!audio_valid(s)) return;
  xSemaphoreTake(s->api_lock, portMAX_DELAY);
  const uint32_t head = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE);
  __atomic_store_n(&s->tail, head, __ATOMIC_RELEASE);
  s->playing = false;
  s->frac = 0x10000;
  xSemaphoreGive(s->space);
  xSemaphoreGive(s->api_lock);
}

static void audio_stats(papp_audio_stream_t *s, papp_audio_stats_t *out) {
  if (!out) return;
  std::memset(out, 0, sizeof(*out));
  if (!audio_valid(s)) return;
  const uint32_t head = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE);
  const uint32_t tail = __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE);
  out->queued_frames = head - tail;
  out->capacity_frames = s->capacity;
  out->underruns = s->underruns;
  out->played_frames = s->played;
}

static const papp_if_audi_t s_audi = {
    {PAPP_IF_AUDI, PAPP_IF_AUDI_VER, sizeof(papp_if_audi_t)}, audio_output_rate, audio_open,
    audio_close, audio_write, audio_pause, audio_gain, audio_clear, audio_stats};

static uint8_t thread_priority(uint8_t priority) {
  static constexpr uint8_t values[] = {3, 5, 6};
  const uint8_t bounded = priority <= PAPP_PRIO_HIGH ? priority : static_cast<uint8_t>(PAPP_PRIO_NORMAL);
  return values[bounded];
}

static int thread_wrapper(void *arg) {
  auto *thread = static_cast<papp_thread *>(arg);
  thread->task = xTaskGetCurrentTaskHandle();
  const int result = thread->fn ? thread->fn(thread->arg) : -1;
  thread->exit_code = result;
  xSemaphoreGive(thread->done);
  return result;
}

static papp_thread_t *thread_create(const papp_thread_cfg_t *cfg, papp_thread_fn_t fn, void *arg) {
  if (!fn || !s_services || !s_services->task_create_with_caps || exiting()) return nullptr;
  papp_thread *thread = nullptr;
  portENTER_CRITICAL(&s_thread_mux);
  for (auto &candidate : s_threads) {
    if (!candidate.used) {
      candidate = {};
      candidate.used = true;
      candidate.magic = THREAD_MAGIC;
      thread = &candidate;
      break;
    }
  }
  portEXIT_CRITICAL(&s_thread_mux);
  if (!thread) return nullptr;
  const papp_thread_cfg_t c = cfg ? *cfg : papp_thread_cfg_t{};
  uint32_t stack = c.stack_size ? c.stack_size : 8192;
  stack = std::max<uint32_t>(2048, (stack + 15) & ~15u);
  const int core = c.core == 0 || c.core == 1 ? c.core : -1;
  const uint32_t mem_caps = ((c.flags & PAPP_THREAD_STACK_PSRAM) || stack > 32768) ? PAX_MEM_CAP_SPIRAM : 0;
  thread->fn = fn;
  thread->arg = arg;
  thread->done = xSemaphoreCreateBinary();
  char name[16]{};
  std::snprintf(name, sizeof(name), "%s", c.name ? c.name : "papp_thr");
  const int created = thread->done ? s_services->task_create_with_caps(
      reinterpret_cast<void (*)(void *)>(+[](void *p) { (void)thread_wrapper(p); }), name,
      stack, thread, thread_priority(c.prio), &thread->task, core, mem_caps) : -1;
  if (created != 0 || thread->task == nullptr) {
    if (thread->done) vSemaphoreDelete(thread->done);
    portENTER_CRITICAL(&s_thread_mux); *thread = {}; portEXIT_CRITICAL(&s_thread_mux);
    return nullptr;
  }
  return reinterpret_cast<papp_thread_t *>(thread);
}

static papp_thread *thread_checked(papp_thread_t *opaque) {
  auto *thread = reinterpret_cast<papp_thread *>(opaque);
  return thread >= s_threads && thread < s_threads + PAPP_MAX_THREADS &&
         thread->used && thread->magic == THREAD_MAGIC ? thread : nullptr;
}

static void thread_release(papp_thread *thread) {
  if (thread->done) vSemaphoreDelete(thread->done);
  portENTER_CRITICAL(&s_thread_mux); *thread = {}; portEXIT_CRITICAL(&s_thread_mux);
}

static bool thread_join(papp_thread_t *opaque, uint32_t timeout_ms, int *exit_code) {
  papp_thread *thread = thread_checked(opaque);
  if (!thread || thread->task == xTaskGetCurrentTaskHandle()) return false;
  const TickType_t start = xTaskGetTickCount();
  const TickType_t limit = timeout_ms == PAPP_WAIT_FOREVER ? portMAX_DELAY : timeout_slice(timeout_ms);
  while (!exiting()) {
    if (xSemaphoreTake(thread->done, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (exit_code) *exit_code = thread->exit_code;
      thread_release(thread);
      return true;
    }
    if (limit != portMAX_DELAY && xTaskGetTickCount() - start >= limit) return false;
  }
  return false;
}

static void thread_exit(int code) {
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  for (auto &thread : s_threads) {
    if (thread.used && thread.task == self) {
      thread.exit_code = code;
      if (thread.done) xSemaphoreGive(thread.done);
      if (s_services && s_services->task_delete) s_services->task_delete(nullptr);
      break;
    }
  }
  if (s_services && s_services->papp2_app_exit) s_services->papp2_app_exit(code);
}

static papp_thread_t *thread_self() {
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  for (auto &thread : s_threads)
    if (thread.used && thread.task == self) return reinterpret_cast<papp_thread_t *>(&thread);
  return nullptr;
}

static void thread_yield() { taskYIELD(); }

static void **thread_tls_slot() {
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (self == s_main_task) return &s_main_tls;
  for (auto &thread : s_threads)
    if (thread.used && thread.task == self) return &thread.tls;
  return nullptr;
}

static void thread_app_exit(int code) {
  if (s_services && s_services->papp2_app_exit) s_services->papp2_app_exit(code);
}

static papp_sync_object *object_create(uint8_t kind, QueueHandle_t handle) {
  if (!handle) return nullptr;
  auto *object = static_cast<papp_sync_object *>(heap_caps_calloc(1, sizeof(papp_sync_object), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!object) {
    if (kind == OBJ_QUEUE) vQueueDeleteWithCaps(handle); else vSemaphoreDelete(handle);
    return nullptr;
  }
  object->kind = kind;
  object->handle = handle;
  portENTER_CRITICAL(&s_object_mux);
  object->next = s_objects;
  s_objects = object;
  portEXIT_CRITICAL(&s_object_mux);
  return object;
}

static papp_sync_object *object_check(void *opaque, uint8_t kind) {
  auto *object = static_cast<papp_sync_object *>(opaque);
  if (!object || object->magic != OBJECT_MAGIC ||
      !(object->kind == kind || (kind == OBJ_MUTEX && object->kind == OBJ_RECURSIVE_MUTEX))) return nullptr;
  return object;
}

static void object_destroy(void *opaque, uint8_t kind) {
  auto *object = object_check(opaque, kind);
  if (!object) return;
  portENTER_CRITICAL(&s_object_mux);
  papp_sync_object **cursor = &s_objects;
  while (*cursor && *cursor != object) cursor = &(*cursor)->next;
  if (*cursor == object) *cursor = object->next;
  portEXIT_CRITICAL(&s_object_mux);
  const uint8_t object_kind = object->kind;
  object->magic = 0;
  if (object_kind == OBJ_QUEUE) vQueueDeleteWithCaps(object->handle);
  else vSemaphoreDelete(object->handle);
  heap_caps_free(object);
}

static TickType_t short_wait(uint32_t timeout_ms, TickType_t start, TickType_t limit) {
  if (timeout_ms == 0) return 0;
  if (limit == portMAX_DELAY) return pdMS_TO_TICKS(10);
  const TickType_t elapsed = xTaskGetTickCount() - start;
  if (elapsed >= limit) return 0;
  const TickType_t remaining = limit - elapsed;
  return std::min<TickType_t>(remaining, std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
}

static papp_mutex_t *mutex_create(bool recursive) {
  return reinterpret_cast<papp_mutex_t *>(object_create(recursive ? OBJ_RECURSIVE_MUTEX : OBJ_MUTEX,
      recursive ? xSemaphoreCreateRecursiveMutex() : xSemaphoreCreateMutex()));
}
static void mutex_destroy(papp_mutex_t *m) { object_destroy(m, OBJ_MUTEX); }
static bool mutex_lock(papp_mutex_t *m, uint32_t timeout_ms) {
  auto *o = object_check(m, OBJ_MUTEX); if (!o) return false;
  const TickType_t start = xTaskGetTickCount();
  const TickType_t limit = timeout_ms == PAPP_WAIT_FOREVER ? portMAX_DELAY : timeout_slice(timeout_ms);
  for (;;) {
    const TickType_t wait = short_wait(timeout_ms, start, limit);
    const BaseType_t ok = o->kind == OBJ_RECURSIVE_MUTEX
        ? xSemaphoreTakeRecursive(o->handle, wait) : xSemaphoreTake(o->handle, wait);
    if (ok == pdTRUE) return true;
    if (timeout_ms == 0 || exiting() || (limit != portMAX_DELAY && xTaskGetTickCount() - start >= limit)) return false;
  }
}
static void mutex_unlock(papp_mutex_t *m) {
  auto *o = object_check(m, OBJ_MUTEX); if (!o) return;
  if (o->kind == OBJ_RECURSIVE_MUTEX) xSemaphoreGiveRecursive(o->handle); else xSemaphoreGive(o->handle);
}

static papp_sem_t *sem_create(uint32_t initial, uint32_t max) {
  if (max == 0 || initial > max) return nullptr;
  return reinterpret_cast<papp_sem_t *>(object_create(OBJ_SEM, xSemaphoreCreateCounting(max, initial)));
}
static void sem_destroy(papp_sem_t *s) { object_destroy(s, OBJ_SEM); }
static bool sem_take(papp_sem_t *s, uint32_t timeout_ms) {
  auto *o = object_check(s, OBJ_SEM); if (!o) return false;
  const TickType_t start = xTaskGetTickCount();
  const TickType_t limit = timeout_ms == PAPP_WAIT_FOREVER ? portMAX_DELAY : timeout_slice(timeout_ms);
  for (;;) {
    if (xSemaphoreTake(o->handle, short_wait(timeout_ms, start, limit)) == pdTRUE) return true;
    if (timeout_ms == 0 || exiting() || (limit != portMAX_DELAY && xTaskGetTickCount() - start >= limit)) return false;
  }
}
static void sem_give(papp_sem_t *s) { auto *o = object_check(s, OBJ_SEM); if (o) xSemaphoreGive(o->handle); }

static papp_queue_t *queue_create(uint32_t length, uint32_t item_size) {
  if (!length || !item_size || length > 65536 || item_size > 65536 ||
      static_cast<uint64_t>(length) * item_size > 8 * 1024 * 1024) return nullptr;
  const UBaseType_t caps = static_cast<uint64_t>(length) * item_size > 1024
      ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return reinterpret_cast<papp_queue_t *>(object_create(OBJ_QUEUE, xQueueCreateWithCaps(length, item_size, caps)));
}
static void queue_destroy(papp_queue_t *q) { object_destroy(q, OBJ_QUEUE); }
static bool queue_op(papp_queue_t *q, void *item, uint32_t timeout_ms, bool send) {
  auto *o = object_check(q, OBJ_QUEUE); if (!o || !item) return false;
  const TickType_t start = xTaskGetTickCount();
  const TickType_t limit = timeout_ms == PAPP_WAIT_FOREVER ? portMAX_DELAY : timeout_slice(timeout_ms);
  for (;;) {
    const TickType_t wait = short_wait(timeout_ms, start, limit);
    const BaseType_t ok = send ? xQueueSend(o->handle, item, wait) : xQueueReceive(o->handle, item, wait);
    if (ok == pdTRUE) return true;
    if (timeout_ms == 0 || exiting() || (limit != portMAX_DELAY && xTaskGetTickCount() - start >= limit)) return false;
  }
}
static bool queue_send(papp_queue_t *q, const void *item, uint32_t timeout_ms) {
  return queue_op(q, const_cast<void *>(item), timeout_ms, true);
}
static bool queue_recv(papp_queue_t *q, void *item, uint32_t timeout_ms) { return queue_op(q, item, timeout_ms, false); }
static uint32_t queue_count(papp_queue_t *q) {
  auto *o = object_check(q, OBJ_QUEUE); return o ? uxQueueMessagesWaiting(o->handle) : 0;
}

static const papp_if_thrd_t s_thrd = {
    {PAPP_IF_THRD, PAPP_IF_THRD_VER, sizeof(papp_if_thrd_t)}, thread_create, thread_join,
    thread_exit, thread_self, thread_yield, thread_tls_slot, thread_app_exit,
    mutex_create, mutex_destroy, mutex_lock, mutex_unlock,
    sem_create, sem_destroy, sem_take, sem_give,
    queue_create, queue_destroy, queue_send, queue_recv, queue_count};

static bool extension_matches(const std::string &name, const char *extensions) {
  if (!extensions || !*extensions) return true;
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = name.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  const char *cursor = extensions;
  while (*cursor) {
    while (*cursor == ',' || std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    const char *end = cursor;
    while (*end && *end != ',') ++end;
    std::string candidate(cursor, end);
    while (!candidate.empty() && std::isspace(static_cast<unsigned char>(candidate.back()))) candidate.pop_back();
    std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!candidate.empty() && candidate == ext) return true;
    cursor = end;
  }
  return false;
}

static bool excluded(const char *exclude, const std::string &name) {
  if (!exclude) return false;
  std::string low = name;
  std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return std::tolower(c); });
  const char *cursor = exclude;
  while (*cursor) {
    while (*cursor == ',' || std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    const char *end = cursor; while (*end && *end != ',') ++end;
    std::string candidate(cursor, end);
    std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!candidate.empty() && low == candidate) return true;
    cursor = end;
  }
  return false;
}

static std::string join_path(const std::string &base, const std::string &name) {
  if (base.empty() || base == "/") return "/" + name;
  return base.back() == '/' ? base + name : base + "/" + name;
}

static int ui_menu(const char *title, const char *const *items, int count, int initial) {
  if (!s_services || !s_services->papp2_ui_choose || count < 1 || count > 7) return -1;
  return s_services->papp2_ui_choose(title, "UP/DOWN SELECT  B BACK", items, count, initial);
}

static void ui_message(const char *title, const char *text) {
  const char *items[] = {"OK"};
  if (s_services && s_services->papp2_ui_choose)
    (void)s_services->papp2_ui_choose(title, text, items, 1, 0);
}

static int ui_volume() {
  return s_services && s_services->papp2_ui_volume ? s_services->papp2_ui_volume() : -1;
}

static int ui_pick_file(const papp_pick_cfg_t *cfg, papp_pick_result_t *out) {
  if (!cfg || !out || !cfg->root || !s_services || !s_services->dir_open ||
      !s_services->dir_read || !s_services->dir_close || !s_services->papp2_ui_choose) return PAPP_PICK_CANCEL;
  std::memset(out, 0, sizeof(*out));
  const std::string root(cfg->root);
  std::string current = root;
  size_t offset = 0;
  for (;;) {
    if (exiting()) return PAPP_PICK_CANCEL;
    void *dir = s_services->dir_open(current.c_str());
    if (!dir) {
      ui_message(cfg->title ? cfg->title : "FILES", "COULD NOT OPEN FOLDER");
      return PAPP_PICK_CANCEL;
    }
    std::vector<BrowserEntry> entries;
    char name[256];
    int is_dir = 0;
    while (s_services->dir_read(dir, name, sizeof(name), &is_dir) > 0) {
      if (!name[0] || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0 ||
          ((cfg->flags & PAPP_PICK_EXCLUDE_F) && excluded(cfg->exclude, name))) continue;
      const bool directory = is_dir != 0;
      if ((!directory && !extension_matches(name, cfg->extensions)) ||
          (directory && (cfg->flags & PAPP_PICK_NO_DIRS_F))) continue;
      entries.push_back({name, join_path(current, name), directory});
    }
    s_services->dir_close(dir);
    std::sort(entries.begin(), entries.end(), [](const BrowserEntry &a, const BrowserEntry &b) {
      return a.directory != b.directory ? a.directory > b.directory : a.name < b.name;
    });
    const bool can_parent = current != root && current != "/";
    size_t page_start = offset;
    std::vector<int> choices;
    std::vector<std::string> labels;
    if (page_start > 0) { labels.emplace_back("< PREVIOUS"); choices.push_back(-2); }
    if (can_parent && page_start == 0) { labels.emplace_back("../"); choices.push_back(-1); }
    const size_t rows_for_entries = 8 - labels.size();
    const size_t remaining = entries.size() - std::min(page_start, entries.size());
    const size_t entry_limit = remaining > rows_for_entries ? rows_for_entries - 1 : rows_for_entries;
    const size_t entry_end = std::min(entries.size(), page_start + entry_limit);
    for (size_t i = page_start; i < entry_end; ++i) {
      labels.push_back(entries[i].name + (entries[i].directory ? "/" : ""));
      choices.push_back(static_cast<int>(i));
    }
    const size_t shown_end = entry_end;
    const bool has_next = shown_end < entries.size();
    if (has_next && labels.size() < 8) { labels.emplace_back("NEXT >"); choices.push_back(-3); }
    std::vector<const char *> labels_ptr;
    labels_ptr.reserve(labels.size()); for (const auto &label : labels) labels_ptr.push_back(label.c_str());
    if (labels_ptr.empty()) { ui_message(cfg->title ? cfg->title : "FILES", "NO MATCHING FILES"); return PAPP_PICK_CANCEL; }
    const std::string status = current.substr(0, 60);
    const int selected = s_services->papp2_ui_choose(cfg->title ? cfg->title : "SELECT FILE", status.c_str(),
        labels_ptr.data(), static_cast<int>(labels_ptr.size()), 0);
    if (selected < 0 || static_cast<size_t>(selected) >= choices.size()) return PAPP_PICK_CANCEL;
    const int choice = choices[selected];
    if (choice == -1) {
      const size_t slash = current.find_last_of('/');
      current = slash == 0 ? "/" : current.substr(0, slash);
      offset = 0;
      continue;
    }
    if (choice == -2) { offset = offset > 6 ? offset - 6 : 0; continue; }
    if (choice == -3) { offset = shown_end; continue; }
    const BrowserEntry &entry = entries[static_cast<size_t>(choice)];
    if (entry.directory) { current = entry.path; offset = 0; continue; }

    std::snprintf(out->path, sizeof(out->path), "%s", entry.path.c_str());
    out->action = PAPP_PICK_RUN;
    if (cfg->save_dir && cfg->save_ext && *cfg->save_ext) {
      std::string save_name = entry.name;
      if (cfg->flags & PAPP_PICK_SAVE_NOEXT_F) {
        const size_t dot = save_name.find_last_of('.'); if (dot != std::string::npos) save_name.resize(dot);
      }
      const std::string save = join_path(cfg->save_dir, save_name + cfg->save_ext);
      std::snprintf(out->save_path, sizeof(out->save_path), "%s", save.c_str());
      pax_file_stat_t save_stat{};
      if (s_services->file_stat && s_services->file_stat(save.c_str(), &save_stat) == 0) {
        const char *save_choices[] = {"RESUME", "RESTART", "DELETE SAVE", "CANCEL"};
        const int action = s_services->papp2_ui_choose("SAVE FOUND", entry.name.c_str(), save_choices, 4, 0);
        if (action == 0) { out->action = PAPP_PICK_RESUME; return out->action; }
        if (action == 1) { out->action = PAPP_PICK_RESTART; return out->action; }
        if (action == 2 && s_services->file_remove) { s_services->file_remove(save.c_str()); continue; }
        return PAPP_PICK_CANCEL;
      }
    }
    return out->action;
  }
}

static const papp_if_ui_t s_ui = {
    {PAPP_IF_UI, PAPP_IF_UI_VER, sizeof(papp_if_ui_t)}, ui_pick_file, ui_message, ui_menu, ui_volume};

}  // namespace

void papp2_runtime_begin(const app_services_t *services) {
  s_services = services;
  s_runtime_stopping = false;
  s_main_task = xTaskGetCurrentTaskHandle();
  s_main_tls = nullptr;
  s_audio_shutdown = false;
  s_audio_task = nullptr;
  for (auto &thread : s_threads) thread = {};
  for (auto &stream : s_audio_streams) stream = {};
  std::memset(s_key_state, 0, sizeof(s_key_state));
  s_event_head = s_event_tail = 0;
  s_previous_buttons = 0;
  s_key_modifiers = 0;
  s_input_connected = false;
  if (!s_audio_lock) s_audio_lock = xSemaphoreCreateMutex();
  input_flush();
}

void papp2_runtime_end() {
  s_runtime_stopping = true;
  if (s_audio_task) {
    s_audio_shutdown = true;
    xTaskNotifyGive(s_audio_task);
    if (s_services && s_services->task_delete) s_services->task_delete(s_audio_task);
    s_audio_task = nullptr;
  }
  for (auto &thread : s_threads) {
    if (!thread.used) continue;
    if (thread.task && s_services && s_services->task_delete) s_services->task_delete(thread.task);
    if (thread.done) vSemaphoreDelete(thread.done);
    thread = {};
  }
  for (auto &stream : s_audio_streams) {
    if (stream.ring) heap_caps_free(stream.ring);
    if (stream.space) vSemaphoreDelete(stream.space);
    if (stream.api_lock) vSemaphoreDelete(stream.api_lock);
    stream = {};
  }
  while (s_objects) {
    papp_sync_object *object = s_objects;
    s_objects = object->next;
    if (object->kind == OBJ_QUEUE) vQueueDeleteWithCaps(object->handle);
    else vSemaphoreDelete(object->handle);
    heap_caps_free(object);
  }
  s_main_task = nullptr;
  s_main_tls = nullptr;
  s_services = nullptr;
}

const papp_if_audi_t *papp2_runtime_audi() { return &s_audi; }
const papp_if_inpt_t *papp2_runtime_inpt() { return &s_inpt; }
const papp_if_thrd_t *papp2_runtime_thrd() { return &s_thrd; }
const papp_if_ui_t *papp2_runtime_ui() { return &s_ui; }
