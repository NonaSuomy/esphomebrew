#include "pax_loader.h"
#include "pax_internal.h"
#include "pax_memory.h"
#include "pax_frame_copy.h"
#include "pax_ownership.h"
#include "pax_stick_mouse.h"
#include "pax_volume_icon.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <limits>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/components/mipi_dsi/mipi_dsi.h"

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_mmu_map.h"
#include "esp_vfs_fat.h"
#include "driver/ppa.h"
#include "pax_jpeg.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#ifdef USE_ESP32
#include "esp_task_wdt.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/mmu_types.h"
#include "pngAux.h"
#include "zlib.h"

// psram_app_handle_t is declared by psram_app.h as a pointer to the global
// C-compatible struct tag. Keep the concrete implementation type at global
// scope so the ABI handle and the C++ implementation refer to the same type.
struct psram_app {
  void *code_buf{nullptr};
  size_t code_alloc{0};
  void *exec_ptr{nullptr};
  bool mapped{false};
  pax_header_t header{};
};

#include "pax_wifi_stream.h"
#include "pax_c6_update.h"
#include "pax_firmware_update.h"

namespace esphome {
namespace pax_loader {

static const char *const TAG = "pax_loader";
static const char *http_error_hint(esp_err_t err);
// The canvas (the app's framebuffer, centred on the panel) is runtime state:
// see pax_canvas.h and geometry_. Apps start at canvas::LEGACY_WIDTH x
// LEGACY_HEIGHT (800x480) and may switch with display_set_canvas.
static constexpr int EMU_WIDTH = 320;
static constexpr int EMU_HEIGHT = 240;
static constexpr size_t MMU_PAGE_SIZE = 0x10000;
static constexpr size_t MAX_NETWORK_PAX_SIZE = 16 * 1024 * 1024;
static constexpr size_t HTTP_READ_BUFFER_SIZE = 16 * 1024;
static constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
static constexpr uint8_t HTTP_MAX_REDIRECTIONS = 5;
static constexpr int64_t PAX_CLOSE_GRACE_US = 4000000;
// Large games such as Red Alert can spend several seconds opening/indexing
// CD-sized MIX archives before producing their first frame. Keep the
// black-screen recovery, but don't terminate legitimate cold starts at 5 s.
static constexpr int64_t PAX_NO_FRAME_TIMEOUT_US = 15000000;
static constexpr int64_t REDALERT_NO_FRAME_TIMEOUT_US = 60000000;
static constexpr time_t MIN_VALID_TLS_TIME = 1704067200;  // 2024-01-01 UTC
static constexpr uint32_t NETWORK_PREFERENCE_KEY = 0x504E4554;  // "PNET"
static constexpr uint32_t NETWORK_PREFERENCE_MAGIC = 0x4E455431; // "NET1"
// Not 3232: that is ESPHome's OTA port on ESP32, taken as soon as the YAML has `ota:`.
static constexpr uint16_t SCREEN_STREAM_PORT = 3233;
// This is a diagnostic transport, not the panel's render target.  Keeping it
// below 400 kB/s lets it coexist with Quake's render/audio tasks over the
// board's Ethernet link without holding stale frames in TCP buffers.
static constexpr uint32_t SCREEN_STREAM_INTERVAL_MS = 250;
static constexpr uint16_t SCREEN_STREAM_WIDTH = 100;
static constexpr uint16_t SCREEN_STREAM_HEIGHT = 60;
static constexpr size_t SCREEN_STREAM_FRAME_BYTES = SCREEN_STREAM_WIDTH * SCREEN_STREAM_HEIGHT * sizeof(uint16_t);
static constexpr size_t SCREEN_STREAM_AUDIO_BYTES = 192000;
static constexpr size_t SCREEN_STREAM_AUDIO_PACKET_BYTES = 12288;
static int64_t s_pax_last_dsi_transfer_us = 0;

// FatFs on an SD card is not journaled.  fflush() only drains stdio; it does
// not ask the VFS/FatFs layer to commit the sectors before a reset or card
// eject.  All launcher-owned files go through this barrier before they become
// visible under their final name.
static bool sync_file_(FILE *file, const char *path) {
  if (file == nullptr)
    return false;
  if (std::fflush(file) != 0) {
    ESP_LOGW(TAG, "Flush failed for %s (errno %d)", path != nullptr ? path : "file", errno);
    return false;
  }
  const int fd = fileno(file);
  if (fd < 0 || fsync(fd) != 0) {
    ESP_LOGW(TAG, "Sync failed for %s (errno %d)", path != nullptr ? path : "file", errno);
    return false;
  }
  return true;
}

static void warn_if_not_durable_(const std::string &path);

static esp_err_t commit_upload_(const std::string& path, const std::string& part) {
  esp_err_t err = ESP_OK;
  const std::string backup = path + ".upload.bak";
  struct stat target{};
  const bool had_target = stat(path.c_str(), &target) == 0;
  if (had_target) {
    std::remove(backup.c_str());
    if (std::rename(path.c_str(), backup.c_str()) != 0) {
      ESP_LOGE(TAG, "Cannot stage existing target %s (errno %d)", path.c_str(), errno);
      err = ESP_FAIL;
    }
  }
  if (err == ESP_OK && std::rename(part.c_str(), path.c_str()) != 0) {
    ESP_LOGE(TAG, "Cannot commit %s (errno %d)", path.c_str(), errno);
    err = ESP_FAIL;
    if (had_target && std::rename(backup.c_str(), path.c_str()) != 0)
      ESP_LOGE(TAG, "Could not restore previous target %s (errno %d)", path.c_str(), errno);
  }
  if (err == ESP_OK) {
    warn_if_not_durable_(path);
    if (had_target && std::remove(backup.c_str()) != 0 && errno != ENOENT)
      ESP_LOGW(TAG, "Could not remove old target backup %s (errno %d)", backup.c_str(), errno);
  }
  return err;
}

struct SavedNetworkConfig {
  uint32_t magic;
  int32_t dhcp_enabled;
  uint32_t ipv4;
  uint32_t netmask;
  uint32_t gateway;
  uint32_t dns;
};

static void recover_staged_file_(const std::string &path, const char *backup_suffix) {
  const std::string backup = path + backup_suffix;
  const std::string part = path + ".part";
  struct stat target_info{}, backup_info{};
  const bool target_exists = stat(path.c_str(), &target_info) == 0;
  const bool backup_exists = stat(backup.c_str(), &backup_info) == 0;
  if (!target_exists && backup_exists) {
    if (std::rename(backup.c_str(), path.c_str()) == 0)
      ESP_LOGW(TAG, "Recovered %s from interrupted replacement", path.c_str());
    else
      ESP_LOGW(TAG, "Could not recover %s from %s (errno %d)", path.c_str(), backup.c_str(), errno);
  } else if (target_exists && backup_exists) {
    // The new target was committed; the backup is now only stale metadata.
    if (std::remove(backup.c_str()) != 0 && errno != ENOENT)
      ESP_LOGW(TAG, "Could not remove stale backup %s (errno %d)", backup.c_str(), errno);
  }
  // A .part file has no validity record after a reset, so never promote it.
  if (std::remove(part.c_str()) != 0 && errno != ENOENT)
    ESP_LOGW(TAG, "Could not remove stale staging file %s (errno %d)", part.c_str(), errno);
  const std::string temp = path + ".tmp";
  if (std::remove(temp.c_str()) != 0 && errno != ENOENT)
    ESP_LOGW(TAG, "Could not remove stale temporary file %s (errno %d)", temp.c_str(), errno);
}

static bool sync_path_(const std::string &path) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return false;
  const bool ok = fsync(fd) == 0;
  close(fd);
  return ok;
}

static void warn_if_not_durable_(const std::string &path) {
  if (!sync_path_(path))
    ESP_LOGW(TAG, "Committed %s, but the final durability barrier failed", path.c_str());
}

static std::string launcher_search_lower(const std::string &value) {
  std::string result = value;
  for (char &character : result)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return result;
}

static bool launcher_search_matches(const std::string &query, const std::string &value) {
  return query.empty() || launcher_search_lower(value).find(query) != std::string::npos;
}

// ZIP extraction must not reuse one fixed output name for every archive. If
// the selected archive cannot be replaced (or an older PAX keeps the cached
// path), the emulator will otherwise keep reopening the previous game's ROM.
static std::string launcher_zip_cache_stem(const std::string &app, const std::string &archive) {
  uint32_t hash = 2166136261U;  // FNV-1a, small and deterministic on-device.
  for (const unsigned char character : archive) {
    hash ^= character;
    hash *= 16777619U;
  }
  char suffix[9] = {};
  std::snprintf(suffix, sizeof(suffix), "%08lx", static_cast<unsigned long>(hash));
  return app + "_" + suffix;
}

struct __attribute__((packed)) ScreenStreamHeader {
  char magic[8];
  uint16_t width;
  uint16_t height;
  uint32_t frame_bytes;
  uint32_t sequence;
};

struct __attribute__((packed)) ScreenStreamAudioHeader {
  char magic[8];
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t sample_bits;
  uint32_t audio_bytes;
  uint32_t sequence;
};

// PAXFL01: a status (StreamFileStatus), then `size` bytes of the file or of a
// directory listing ("name<TAB>size" lines, directories end in '/').
struct __attribute__((packed)) StreamFileHeader {
  char magic[8];
  uint32_t status;
  uint32_t size;
  uint32_t sequence;
};
enum StreamFileStatus : uint32_t { FILE_OK = 0, FILE_NOT_FOUND = 1, FILE_TOO_LARGE = 2, FILE_BAD_PATH = 3, FILE_READ_ERROR = 4 };
static constexpr size_t STREAM_FILE_MAX_BYTES = 1024 * 1024;
static constexpr size_t STREAM_LISTING_MAX_BYTES = 64 * 1024;
// Heap log lines while an app runs (log_heap): the first once it has had
// time to load and start its audio, then every 30 s.
static constexpr int64_t HEAP_LOG_FIRST_US = 10 * 1000000LL;
static constexpr int64_t HEAP_LOG_INTERVAL_US = 30 * 1000000LL;
static constexpr int64_t CONTROL_OVERLAY_IDLE_US = 3000000;
static constexpr int CONTROL_OVERLAY_SWIPE_THRESHOLD = 32;
static constexpr int64_t CONTROL_OVERLAY_SWIPE_MAX_US = 900000;

static bool send_screen_stream_bytes(int socket_fd, const void *data, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  while (length != 0) {
    // Keep full-canvas screenshots within the hosted network bridge's
    // bounded transfer buffers, rather than one PSRAM-sized socket write.
    const ssize_t sent = send(socket_fd, bytes, std::min(length, size_t{4096}), MSG_NOSIGNAL);
    if (sent <= 0)
      return false;
    bytes += sent;
    length -= static_cast<size_t>(sent);
  }
  return true;
}

// The shared close, brightness, and volume controls form one centered toolbar.
// The panel's raw draw path is hardware-flipped, so each raw destination is
// the 180-degree counterpart of the desired physical screen position.
static constexpr int CLOSE_BUTTON_SIZE = canvas::CLOSE_SIZE;
alignas(64) static uint16_t close_overlay_buffer[CLOSE_BUTTON_SIZE * CLOSE_BUTTON_SIZE] = {};
static constexpr int GAME_MENU_BUTTON_SIZE = canvas::GAME_MENU_SIZE;
alignas(64) static uint16_t game_menu_button_overlay_buffer[GAME_MENU_BUTTON_SIZE * GAME_MENU_BUTTON_SIZE] = {};
static constexpr int RELOAD_BUTTON_SIZE = canvas::RELOAD_SIZE;
alignas(64) static uint16_t reload_overlay_buffer[RELOAD_BUTTON_SIZE * RELOAD_BUTTON_SIZE] = {};
static constexpr int VOLUME_BUTTON_SIZE = canvas::VOLUME_SIZE;
alignas(64) static uint16_t volume_overlay_buffer[VOLUME_BUTTON_SIZE * VOLUME_BUTTON_SIZE] = {};
static constexpr int VOLUME_SLIDER_WIDTH = canvas::VOLUME_SLIDER_WIDTH;
static constexpr int VOLUME_SLIDER_HEIGHT = canvas::VOLUME_SLIDER_HEIGHT;
alignas(64) static uint16_t volume_slider_overlay_buffer[VOLUME_SLIDER_WIDTH * VOLUME_SLIDER_HEIGHT] = {};
static constexpr int BRIGHTNESS_BUTTON_SIZE = canvas::BRIGHTNESS_SIZE;
static constexpr int BRIGHTNESS_SLIDER_WIDTH = canvas::BRIGHTNESS_SLIDER_WIDTH;
static constexpr int BRIGHTNESS_SLIDER_HEIGHT = canvas::BRIGHTNESS_SLIDER_HEIGHT;
alignas(64) static uint16_t brightness_overlay_buffer[BRIGHTNESS_BUTTON_SIZE * BRIGHTNESS_BUTTON_SIZE] = {};
alignas(64) static uint16_t brightness_slider_overlay_buffer[BRIGHTNESS_SLIDER_WIDTH * BRIGHTNESS_SLIDER_HEIGHT] = {};
// Stable across firmware/configuration hashes, including updates from GitHub.
struct SavedRuntimeSettings { uint32_t magic; uint8_t volume, automatic, reserved[2]; };
static constexpr uint32_t RUNTIME_SETTINGS_KEY=0x50415853U, RUNTIME_SETTINGS_MAGIC=0x50524631U;
static SavedRuntimeSettings saved_runtime_settings{};
static esphome::ESPPreferenceObject runtime_settings_preference;
static int32_t s_brightness = 100;
static constexpr int GAME_MENU_FRAME_WIDTH = 400;
static constexpr int GAME_MENU_FRAME_HEIGHT = 280;
static constexpr size_t GAME_MENU_FRAME_PIXELS = GAME_MENU_FRAME_WIDTH * GAME_MENU_FRAME_HEIGHT;
static constexpr size_t GAME_MENU_PREVIEW_PIXELS = PAX_GAME_MENU_PREVIEW_WIDTH * PAX_GAME_MENU_PREVIEW_HEIGHT;
static constexpr size_t GAME_MENU_ALLOC_PIXELS = GAME_MENU_FRAME_PIXELS + GAME_MENU_PREVIEW_PIXELS;
static constexpr uint8_t GAME_MENU_BASE_ITEM_COUNT = 5;
static constexpr uint8_t GAME_MENU_NES_ITEM_COUNT = GAME_MENU_BASE_ITEM_COUNT + 1;

static const uint8_t *game_menu_glyph(char character) {
  static constexpr uint8_t blank[7] = {};
  static constexpr uint8_t alphabet[26][7] = {
      {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // A
      {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // B
      {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, // C
      {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, // D
      {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
      {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
      {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}, // G
      {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // H
      {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // I
      {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, // J
      {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
      {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // L
      {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
      {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
      {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
      {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
      {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, // Q
      {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // R
      {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E}, // S
      {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
      {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // U
      {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, // V
      {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}, // W
      {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, // X
      {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, // Y
      {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, // Z
  };
  static constexpr uint8_t greater[7] = {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10};
  static constexpr uint8_t digits[10][7] = {
      {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
      {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
      {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
      {14,17,17,15,1,2,12},
  };
  static constexpr uint8_t colon[7] = {0,4,4,0,4,4,0};
  static constexpr uint8_t period[7] = {0,0,0,0,0,4,4};
  static constexpr uint8_t minus[7] = {0,0,0,31,0,0,0};
  if (character >= 'a' && character <= 'z')
    character = static_cast<char>(character - 'a' + 'A');
  if (character >= 'A' && character <= 'Z')
    return alphabet[character - 'A'];
  if (character >= '0' && character <= '9')
    return digits[character - '0'];
  if (character == ':')
    return colon;
  if (character == '.')
    return period;
  if (character == '-')
    return minus;
  if (character == '>')
    return greater;
  return blank;
}

static void game_menu_fill_rect(uint16_t *frame, int stride, int width, int height,
                                int x, int y, int w, int h, uint16_t color) {
  const int left = std::max(0, x);
  const int top = std::max(0, y);
  const int right = std::min(width, x + w);
  const int bottom = std::min(height, y + h);
  if (left >= right || top >= bottom)
    return;
  for (int row = top; row < bottom; ++row)
    std::fill_n(frame + static_cast<size_t>(row) * stride + left, right - left, color);
}

static void game_menu_draw_text(uint16_t *frame, int stride, int width, int height,
                                int x, int y, const char *text, uint16_t color, int scale) {
  if (text == nullptr || scale < 1)
    return;
  for (const char *character = text; *character != '\0'; ++character, x += 6 * scale) {
    const uint8_t *glyph = game_menu_glyph(*character);
    for (int row = 0; row < 7; ++row) {
      for (int column = 0; column < 5; ++column) {
        if ((glyph[row] & (1U << (4 - column))) != 0)
          game_menu_fill_rect(frame, stride, width, height, x + column * scale, y + row * scale,
                              scale, scale, color);
      }
    }
  }
}

static bool overlay_rect_on_panel(const canvas::Geometry &geometry, int raw_x, int raw_y, int width, int height);

// Every child task executes code in the loaded image. Stop ordinary and
// PSRAM-stack tasks before unmapping that image, including forced closes.
struct PaxTaskStart { void (*fn)(void *); void *arg; };
struct PaxTaskRecord { TaskHandle_t handle; bool caps; PaxTaskStart *start;
  unsigned services; unsigned display_leases; bool stopping; };
static constexpr size_t MAX_PAX_TASKS = 32;
static PaxTaskRecord pax_tasks[MAX_PAX_TASKS] = {};
static portMUX_TYPE pax_tasks_mux = portMUX_INITIALIZER_UNLOCKED;
static bool pax_tasks_closing = false;
static TaskHandle_t pax_entry_task = nullptr;
static unsigned pax_entry_services = 0, pax_entry_display_leases = 0;
static unsigned pax_service_calls = 0, pax_display_leases = 0;

static bool remember_pax_task(TaskHandle_t handle, bool caps, PaxTaskStart *start) {
  bool stored = false;
  portENTER_CRITICAL(&pax_tasks_mux);
  for (auto &record : pax_tasks) {
    if (!pax_tasks_closing && record.handle == nullptr) {
      record = {handle, caps, start, 0, 0, false};
      stored = true;
      xTaskNotifyGive(handle);
      break;
    }
  }
  portEXIT_CRITICAL(&pax_tasks_mux);
  return stored;
}

static bool take_pax_task(TaskHandle_t handle, PaxTaskRecord &out) {
  bool found = false;
  portENTER_CRITICAL(&pax_tasks_mux);
  for (auto &record : pax_tasks) {
    if (record.handle == handle) {
      out = record;
      record = {};
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&pax_tasks_mux);
  return found;
}

static void delete_pax_task(TaskHandle_t handle) {
  // Stop the target at an app/firmware boundary. Deleting it while a
  // display, filesystem or audio service holds a mutex strands that mutex.
  for (;;) {
    bool busy = false;
    portENTER_CRITICAL(&pax_tasks_mux);
    for (auto &item : pax_tasks) {
      if (item.handle == handle) {
        item.stopping = true;
        busy = item.services != 0 || item.display_leases != 0;
        break;
      }
    }
    portEXIT_CRITICAL(&pax_tasks_mux);
    if (!busy) break;
    vTaskDelay(1);
  }
  PaxTaskRecord record{};
  if (!take_pax_task(handle, record))
    return;  // Cleanup already claimed this task.
  free(record.start);
  if (record.caps)
    vTaskDeleteWithCaps(handle);
  else
    vTaskDelete(handle);
}

static void pax_child_task(void *) {
  // Do not enter app code until the creator has registered this handle.
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  PaxTaskStart *start = nullptr;
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&pax_tasks_mux);
  for (auto &record : pax_tasks) {
    if (record.handle == self) {
      start = record.start;
      record.start = nullptr;
      break;
    }
  }
  portEXIT_CRITICAL(&pax_tasks_mux);
  if (start != nullptr) {
    auto fn = start->fn;
    void *arg = start->arg;
    free(start);
    fn(arg);
  }
  delete_pax_task(self);
  // If cleanup claimed us, wait until it deletes us; never return a task.
  for (;;) vTaskDelay(portMAX_DELAY);
}

static void delete_registered_pax_tasks() {
  portENTER_CRITICAL(&pax_tasks_mux);
  pax_tasks_closing = true;
  portEXIT_CRITICAL(&pax_tasks_mux);
  for (auto &record : pax_tasks) {
    TaskHandle_t handle;
    portENTER_CRITICAL(&pax_tasks_mux);
    handle = record.handle;
    portEXIT_CRITICAL(&pax_tasks_mux);
    if (handle != nullptr)
      delete_pax_task(handle);
  }
}

// Service thunks make forced termination wait until app tasks leave the
// firmware. New calls then park without acquiring another firmware resource.
// Nested calls and GUI callbacks can finish an already active service.
class PaxServiceCallGuard {
 public:
  PaxServiceCallGuard() {
    for (;;) {
      const TaskHandle_t self = xTaskGetCurrentTaskHandle();
      bool stopping = false;
      unsigned leases = 0;
      portENTER_CRITICAL(&pax_tasks_mux);
      stopping = pax_tasks_closing;
      if (self == pax_entry_task) {
        depth_ = &pax_entry_services;
        leases = pax_entry_display_leases;
      } else {
        for (auto &record : pax_tasks) {
          if (record.handle == self) {
            depth_ = &record.services;
            leases = record.display_leases;
            stopping = stopping || record.stopping;
            break;
          }
        }
      }
      const bool park = depth_ != nullptr && stopping && *depth_ == 0 && leases == 0;
      if (!park) {
        if (depth_ != nullptr) ++*depth_;
        ++pax_service_calls;
      }
      portEXIT_CRITICAL(&pax_tasks_mux);
      if (!park) return;
      vTaskDelay(1);
    }
  }
  ~PaxServiceCallGuard() {
    portENTER_CRITICAL(&pax_tasks_mux);
    if (depth_ != nullptr) --*depth_;
    --pax_service_calls;
    portEXIT_CRITICAL(&pax_tasks_mux);
  }
 private:
  unsigned *depth_ = nullptr;
};

template <auto Fn> struct PaxServiceGate;
template <typename R, typename... Args, R (*Fn)(Args...)>
struct PaxServiceGate<Fn> {
  static R call(Args... args) {
    PaxServiceCallGuard guard;
    return Fn(args...);
  }
};

static bool begin_pax_quiesce() {
  portENTER_CRITICAL(&pax_tasks_mux);
  pax_tasks_closing = true;
  const bool ready = pax_service_calls == 0 && pax_display_leases == 0;
  portEXIT_CRITICAL(&pax_tasks_mux);
  return ready;
}

static void pax_display_lease(int delta) {
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&pax_tasks_mux);
  unsigned *leases = self == pax_entry_task ? &pax_entry_display_leases : nullptr;
  for (auto &record : pax_tasks)
    if (record.handle == self) leases = &record.display_leases;
  if (leases != nullptr && (delta > 0 || *leases != 0)) {
    if (delta > 0) {++*leases; ++pax_display_leases;}
    else {--*leases; --pax_display_leases;}
  }
  portEXIT_CRITICAL(&pax_tasks_mux);
}

// The loader owns memory and FILE handles handed out by its SDK until
// the app returns them. This also reclaims legacy leaks on a forced close.
static ownership::Table<> pax_allocations, pax_files;
static portMUX_TYPE pax_ownership_mux = portMUX_INITIALIZER_UNLOCKED;
static ownership::Record *new_ownership_record(void *pointer, size_t bytes) {
  auto *record=static_cast<ownership::Record *>(heap_caps_malloc(sizeof(ownership::Record), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if(record==nullptr && memory::internal_fits(sizeof(ownership::Record),
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)))
    record=static_cast<ownership::Record *>(heap_caps_malloc(sizeof(ownership::Record), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if(record!=nullptr) *record={pointer,bytes,nullptr};
  return record;
}
static void insert_owned(ownership::Table<> &table, ownership::Record *record) {
  portENTER_CRITICAL(&pax_ownership_mux);
  table.insert(record);
  portEXIT_CRITICAL(&pax_ownership_mux);
}
static ownership::Record *take_owned(ownership::Table<> &table, void *pointer) {
  portENTER_CRITICAL(&pax_ownership_mux);
  auto *record=table.take(pointer);
  portEXIT_CRITICAL(&pax_ownership_mux);
  return record;
}
static void *own_pax_allocation(void *pointer, size_t bytes) {
  if(pointer==nullptr)return nullptr;
  auto *record=new_ownership_record(pointer,bytes);
  if(record==nullptr) {heap_caps_free(pointer);return nullptr;}
  insert_owned(pax_allocations,record);
  return pointer;
}
static FILE *own_pax_file(FILE *file) {
  if(file==nullptr)return nullptr;
  auto *record=new_ownership_record(file,0);
  if(record==nullptr) {std::fclose(file);return nullptr;}
  insert_owned(pax_files,record);
  return file;
}
static void close_pax_files() {
  portENTER_CRITICAL(&pax_ownership_mux);
  auto *files=pax_files.detach();
  portEXIT_CRITICAL(&pax_ownership_mux);
  while(files!=nullptr) {
    auto *next=files->next;
    std::fclose(static_cast<FILE *>(files->pointer));
    heap_caps_free(files);files=next;
  }
}
static void release_pax_resources() {
  jpeg_accelerator.reset();
  portENTER_CRITICAL(&pax_ownership_mux);
  auto *blocks=pax_allocations.detach();
  portEXIT_CRITICAL(&pax_ownership_mux);
  size_t count=0,bytes=0;
  while(blocks!=nullptr) {
    auto *next=blocks->next;bytes+=blocks->bytes;++count;
    heap_caps_free(blocks->pointer);heap_caps_free(blocks);blocks=next;
  }
  if(count!=0) ESP_LOGI("PAX", "Reclaimed %u app allocation(s), %u bytes", static_cast<unsigned>(count),static_cast<unsigned>(bytes));
}

// ── Heap diagnostics ──
// The running app's heap blocks that went to internal RAM because PSRAM was
// full, and its allocations that failed (svc_mem_*); reset when an app starts.
static std::atomic<uint32_t> s_app_heap_internal{0};
static std::atomic<uint32_t> s_app_heap_failed{0};

// Below this, the largest internal DMA-capable block is too small for comfort:
// the speaker's I2S DMA buffers, SD card transfers and lwIP take it in pieces
// of a few KB each, and a TLS session needs more.
static constexpr size_t HEAP_LOW_BLOCK = 16 * 1024;

// One line about the heaps, so the board's log shows whether the drivers still
// have internal DMA-capable RAM: its free bytes, largest free block and lowest
// level since boot, PSRAM's free bytes, and the app's heap counters. A warning
// when the largest block is below HEAP_LOW_BLOCK.
static void log_heap(const char *when, bool inspect_fragmentation = false) {
  constexpr uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
  // Largest-block queries walk every heap allocation with interrupts masked.
  // DSI scanout needs its end-of-frame interrupt to restart DMA promptly.
  // Running-app diagnostics therefore read only the constant-time counters.
  if (!inspect_fragmentation) {
    ESP_LOGI(TAG, "Heap %s: internal DMA RAM %u B free, lowest %u B since boot; "
                  "PSRAM %u B free; app blocks in internal RAM %u, failed %u",
             when, static_cast<unsigned>(heap_caps_get_free_size(dma_caps)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(dma_caps)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(s_app_heap_internal.load()), static_cast<unsigned>(s_app_heap_failed.load()));
    return;
  }
  const size_t largest_dma = heap_caps_get_largest_free_block(dma_caps);
  char line[256];
  std::snprintf(line, sizeof(line),
                "Heap %s: internal DMA RAM %u B free, largest block %u B, lowest %u B since boot; "
                "PSRAM %u B free, largest %u B; app blocks in internal RAM %u, failed %u",
                when, static_cast<unsigned>(heap_caps_get_free_size(dma_caps)), static_cast<unsigned>(largest_dma),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(dma_caps)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
                static_cast<unsigned>(s_app_heap_internal.load()), static_cast<unsigned>(s_app_heap_failed.load()));
  if (largest_dma < HEAP_LOW_BLOCK) {
    ESP_LOGW(TAG, "%s", line);
  } else {
    ESP_LOGI(TAG, "%s", line);
  }
}

// Write an app's frame back from the CPU cache before the PPA reads it. The
// frame is app memory, so its start and size need not be whole cache lines:
// app heap blocks come from PSRAM (pax_memory.h), and Doom's 320x200 screen
// sits 16-byte aligned. A plain C2M msync refuses that ("not aligned with
// cache line size") and leaves the newest lines dirty, so the PPA reads parts
// of an older frame. UNALIGNED writes back the partial lines at both ends too,
// which is safe because nothing is invalidated.
static void sync_app_frame(const uint16_t *buffer, size_t bytes) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(buffer);
  const bool aligned = (address % canvas::CACHE_LINE) == 0 && (bytes % canvas::CACHE_LINE) == 0;
  esp_cache_msync(const_cast<uint16_t *>(buffer), bytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                      (aligned ? 0 : ESP_CACHE_MSYNC_FLAG_UNALIGNED));
}

PaxLoader *PaxLoader::active_ = nullptr;

std::string runtime_path(const char *path) {
  if (path == nullptr)
    return {};
  std::string result(path);
  if (result == "/sd")
    return "/sdcard";
  if (result.rfind("/sd/", 0) == 0)
    return "/sdcard/" + result.substr(4);
  return result;
}

static size_t align_up(size_t value, size_t alignment) {
  if (value > std::numeric_limits<size_t>::max() - (alignment - 1))
    return 0;
  return (value + alignment - 1) & ~(alignment - 1);
}

bool is_network_url(const char *value) {
  if (value == nullptr)
    return false;
  return std::strncmp(value, "http://", 7) == 0 || std::strncmp(value, "https://", 8) == 0;
}

struct RemoteEmulatorAlias {
  const char *alias;
  const char *app;
};

// Keep the common names accepted here in step with the ROM selector's PAX
// names. Unknown safe names are also accepted, so a newly added emulator can
// be launched before its UI picker table is updated.
static const RemoteEmulatorAlias REMOTE_EMULATOR_ALIASES[] = {
    {"gameboy", "gb"},       {"gbc", "gb"},       {"zx", "spectrum"},
    {"spectrum", "spectrum"}, {"a26", "stella"},   {"lynx", "handy"},
    {"pcengine", "pce"},     {"a800", "atari800"}, {"atari800", "atari800"},
    {"supernes", "snes"},    {"megadrive", "genesis"}, {"gen", "genesis"},
    {"a78", "prosystem"},    {"neo", "neogeo"},   {"gamegear", "sms"},
};

static std::string normalized_remote_emulator(const std::string &input) {
  size_t first = input.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  size_t last = input.find_last_not_of(" \t\r\n");
  std::string value = input.substr(first, last - first + 1);
  if (!value.empty() && value.front() == '/') {
    const size_t slash = value.rfind('/');
    value = slash == std::string::npos ? value : value.substr(slash + 1);
  }
  for (char &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (value.size() >= 4 && value.compare(value.size() - 4, 4, ".pax") == 0)
    value.erase(value.size() - 4);
  for (const auto &alias : REMOTE_EMULATOR_ALIASES) {
    if (value == alias.alias)
      return alias.app;
  }
  if (value.empty() || value.size() > 64)
    return {};
  for (const char c : value) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
      return {};
  }
  return value;
}

static bool valid_remote_rom_path(const std::string &path) {
  if (path.size() < 6 || path.size() >= 256 || path.back() == '/' || path.find("..") != std::string::npos ||
      path.find("//") != std::string::npos || path.find('\\') != std::string::npos)
    return false;
  if (!(path.rfind("/sd/", 0) == 0 || path.rfind("/usb0/", 0) == 0))
    return false;
  for (const unsigned char c : path) {
    if (c < 0x20 || c == 0x7f)
      return false;
  }
  return true;
}

static std::vector<std::pair<std::string, std::string>> parse_pax_catalog(const std::string &html,
                                                                             const std::string &base_url);
static std::vector<std::pair<std::string, std::string>> parse_archive_listing(const std::string &html,
                                                                                const std::string &base_url,
                                                                                const std::string &extensions);
static std::string archive_download_base_url(const std::string &url);
static std::string archive_encode_query(const std::string &value);

static std::string archive_search_slug(const std::string &value) {
  std::string result;
  bool separator = false;
  for (const unsigned char character : value) {
    if (std::isalnum(character)) {
      if (separator && !result.empty())
        result.push_back('-');
      result.push_back(static_cast<char>(std::tolower(character)));
      separator = false;
    } else if (!result.empty()) {
      separator = true;
    }
  }
  return result;
}

static std::string archive_search_term(const std::string &query, const std::string &collection) {
  std::string result = query;
  const std::string prefix = "collection:" + launcher_search_lower(collection);
  const std::string lowered = launcher_search_lower(result);
  if (lowered.rfind(prefix, 0) == 0)
    result.erase(0, prefix.size());
  while (!result.empty() && std::isspace(static_cast<unsigned char>(result.front())))
    result.erase(result.begin());
  const std::string and_prefix = "and ";
  if (launcher_search_lower(result).rfind(and_prefix, 0) == 0)
    result.erase(0, and_prefix.size());
  while (!result.empty() && std::isspace(static_cast<unsigned char>(result.front())))
    result.erase(result.begin());
  while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())))
    result.pop_back();
  return result;
}

void PaxLoader::request_launch_rom(const std::string &emulator, const std::string &rom_path) {
  if (this->launched_ || this->launch_pending_ || this->web_storage_busy_) {
    ESP_LOGW(TAG, "Ignoring emulator/ROM launch while a PAX is starting or running");
    return;
  }
  const std::string app_name = normalized_remote_emulator(emulator);
  if (app_name.empty()) {
    ESP_LOGW(TAG, "Ignoring emulator/ROM launch with invalid emulator name: %s", emulator.c_str());
    return;
  }
  if (!valid_remote_rom_path(rom_path)) {
    ESP_LOGW(TAG, "Ignoring emulator/ROM launch with invalid ROM path: %s (use /sd/... or /usb0/...)",
             rom_path.c_str());
    return;
  }

  struct stat rom_stat{};
  const std::string rom_fs_path = runtime_path(rom_path.c_str());
  if (stat(rom_fs_path.c_str(), &rom_stat) != 0 || (!S_ISREG(rom_stat.st_mode) && !S_ISDIR(rom_stat.st_mode))) {
    ESP_LOGW(TAG, "Emulator/ROM launch refused: ROM or game directory is not available: %s", rom_path.c_str());
    return;
  }

  // resolve_app_ prefers an installed copy and then scans every configured
  // storage catalog, including the same folder under /usb0. It can also find a
  // versioned PAX such as nes-0.1.0.pax.
  const std::string app = this->resolve_app_(app_name);
  if (app.empty()) {
    ESP_LOGW(TAG, "Emulator/ROM launch refused: no PAX found for emulator '%s'", app_name.c_str());
    return;
  }

  // Match the on-screen picker: the in-memory setting is what the current app
  // reads, while the sidecar preserves the last choice when the SD is mounted.
  svc_settings_rom_path_set(rom_path.c_str());
  this->write_file("/sd/roms/pax/" + app_name + ".rom", rom_path + "\n");
  ESP_LOGI(TAG, "Remote emulator launch: emulator=%s rom=%s pax=%s", app_name.c_str(), rom_path.c_str(), app.c_str());
  this->request_launch(app);
}

#ifdef PAX_LOADER_USE_LVGL
struct PaxCatalogButtonContext {
  PaxLoader *loader;
  std::string url;
};

static bool launcher_keyboard_visible(lv_obj_t *object) {
  if (object == nullptr || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
    return false;
  const lv_obj_t *screen = lv_obj_get_screen(object);
  if (screen != lv_scr_act() && screen != lv_layer_top())
    return false;
  for (lv_obj_t *parent = lv_obj_get_parent(object); parent != nullptr; parent = lv_obj_get_parent(parent)) {
    if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN))
      return false;
  }
  return true;
}

static void pax_catalog_button_event_cb(lv_event_t *event) {
  if (event == nullptr)
    return;
  auto *context = static_cast<PaxCatalogButtonContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;

  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    ESP_LOGI("pax_loader", "Catalog selection: %s", context->url.c_str());
    context->loader->request_launch_url(context->url);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

// The "◀ name ▶" row at the top of the list: a tap shows the next catalog.
static void pax_catalog_switch_event_cb(lv_event_t *event) {
  if (event != nullptr && lv_event_get_code(event) == LV_EVENT_CLICKED) {
    ESP_LOGI("pax_loader", "Catalog header selected: switching source");
    static_cast<PaxLoader *>(lv_event_get_user_data(event))->next_catalog(1);
  }
}

// Draw status icons directly: Montserrat's built-in symbol subset has Wi-Fi,
// but not Font Awesome's Ethernet glyph (U+F796).
static void pax_network_status_draw(lv_event_t *event) {
  auto *obj = static_cast<lv_obj_t *>(lv_event_get_target(event));
  lv_layer_t *layer = lv_event_get_layer(event);
  lv_area_t area;
  lv_obj_get_coords(obj, &area);
  const bool ethernet = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(obj)) == 2;
  // The Wi-Fi arcs occupy y=10..29 within the drawing. Remove their extra
  // top space so the visible symbol is centered alongside the clock text.
  const int32_t x = area.x1 + 20, y = area.y1 - (ethernet ? 0 : 9);
  const lv_color_t color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
  if (ethernet) {
    lv_draw_rect_dsc_t box;
    lv_draw_rect_dsc_init(&box);
    box.bg_opa = LV_OPA_TRANSP;
    box.border_color = color;
    box.border_width = 2;
    lv_area_t port = {x - 6, y + 3, x + 6, y + 10};
    lv_draw_rect(layer, &box, &port);
    for (int i = -1; i <= 1; ++i) {
      port = {x + i * 12 - 4, y + 22, x + i * 12 + 4, y + 29};
      lv_draw_rect(layer, &box, &port);
    }
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = 2;
    line.p1 = {x, y + 10}; line.p2 = {x, y + 16};
    lv_draw_line(layer, &line);
    line.p1 = {x - 12, y + 16}; line.p2 = {x + 12, y + 16};
    lv_draw_line(layer, &line);
    for (int i = -1; i <= 1; ++i) {
      line.p1 = {x + i * 12, y + 16}; line.p2 = {x + i * 12, y + 22};
      lv_draw_line(layer, &line);
    }
  } else {
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = color;
    arc.width = 2;
    arc.center = {x, y + 27};
    arc.start_angle = 225; arc.end_angle = 315;
    arc.rounded = 1;
    for (unsigned radius = 7; radius <= 17; radius += 5) {
      arc.radius = radius;
      lv_draw_arc(layer, &arc);
    }
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color;
    dot.radius = LV_RADIUS_CIRCLE;
    lv_area_t point = {x - 2, y + 25, x + 2, y + 29};
    lv_draw_rect(layer, &dot, &point);
  }
}

void PaxLoader::update_catalog_status_() {
  if (this->catalog_status_label_ == nullptr) {
    lv_obj_t *top = lv_layer_top();
    if (top == nullptr)
      return;
    // Keep this cluster on LVGL's top layer so it survives page changes. It
    // shares the first toolbar row with the ESPHOMEBREW title; page-specific
    // controls are kept below it or moved into the pinned source bar.
    lv_obj_t *status = lv_obj_create(top);
    // Keep the common status cluster inline with the page title on every
    // launcher page instead of leaving it stranded on the second row.
    // Keep the status glyphs on the same visual baseline as the centered
    // ESPHOMEBREW title (the title starts at y=8 on every launcher page).
    lv_obj_set_pos(status, 760, 8);
    lv_obj_set_size(status, 248, 32);
    lv_obj_set_style_bg_opa(status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(status, LV_OBJ_FLAG_FLOATING);

    lv_obj_t *time_label = lv_label_create(status);
    lv_obj_set_pos(time_label, 0, 0);
    lv_obj_set_size(time_label, 124, 32);
    lv_obj_set_style_text_font(time_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x55D6FF), 0);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_pad_all(time_label, 0, 0);

    lv_obj_t *network_label = lv_label_create(status);
    lv_obj_set_pos(network_label, 132, 0);
    lv_obj_set_size(network_label, 40, 32);
    lv_obj_set_style_text_color(network_label, lv_color_hex(0x55D6FF), 0);
    lv_obj_set_style_text_align(network_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(network_label, 0, 0);
    lv_label_set_text(network_label, "");
    lv_obj_add_event_cb(network_label, pax_network_status_draw, LV_EVENT_DRAW_MAIN, nullptr);

    lv_obj_t *battery_label = lv_label_create(status);
    lv_obj_set_pos(battery_label, 184, 0);
    lv_obj_set_size(battery_label, 40, 32);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(0x55D6FF), 0);
    lv_obj_set_style_text_align(battery_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(battery_label, 0, 0);

    this->catalog_status_label_ = status;
    this->catalog_status_time_label_ = time_label;
    this->catalog_status_network_label_ = network_label;
    this->catalog_status_battery_label_ = battery_label;
  }
  if (this->catalog_status_time_label_ == nullptr || this->catalog_status_network_label_ == nullptr ||
      this->catalog_status_battery_label_ == nullptr)
    return;
  if (this->catalog_volume_font_ != nullptr) {
    lv_obj_set_style_text_font(this->catalog_status_network_label_, this->catalog_volume_font_, 0);
    lv_obj_set_style_text_font(this->catalog_status_battery_label_, this->catalog_volume_font_, 0);
  }
  const time_t now = std::time(nullptr);
  struct tm local{};
  const bool valid_time = now >= MIN_VALID_TLS_TIME && localtime_r(&now, &local) != nullptr;
  const int hour = valid_time ? ((local.tm_hour % 12) == 0 ? 12 : local.tm_hour % 12) : 0;
  const char *ampm = valid_time && local.tm_hour >= 12 ? "PM" : "AM";
  char time_text[24];
  if (valid_time) {
    std::snprintf(time_text, sizeof(time_text), "%d:%02d %s", hour, local.tm_min, ampm);
  } else {
    std::snprintf(time_text, sizeof(time_text), "--:--  --");
  }
  lv_label_set_text(this->catalog_status_time_label_, time_text);

  bool ethernet_connected = false;
#ifdef USE_ETHERNET
  ethernet_connected = ethernet::global_eth_component != nullptr && ethernet::global_eth_component->is_connected();
#endif
  bool wifi_connected = false;
#ifdef USE_WIFI
  wifi_connected = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
#endif
  // Font Awesome 5 solid: fa-ethernet (f796), fa-wifi (f1eb),
  // fa-battery-full (f240). Keep the icon labels on the icon font and the
  // clock on the normal LVGL font; mixing the two fonts in one label is what
  // caused the clock and status text to render as rectangles.
  const char *network_glyph = nullptr;
  if (ethernet_connected) {
    network_glyph = "\xEF\x9E\x96";
  } else if (wifi_connected) {
    network_glyph = "\xEF\x87\xAB";
  } else {
#ifdef USE_ETHERNET
    network_glyph = "\xEF\x9E\x96";
#else
    network_glyph = "\xEF\x87\xAB";
#endif
  }
  // Keep the connection's identity even when disconnected (dimmed below).
  const uintptr_t network_kind = std::strcmp(network_glyph, "\xEF\x9E\x96") == 0 ? 2 : 1;
  if (reinterpret_cast<uintptr_t>(lv_obj_get_user_data(this->catalog_status_network_label_)) != network_kind) {
    lv_obj_set_user_data(this->catalog_status_network_label_, reinterpret_cast<void *>(network_kind));
    lv_obj_invalidate(this->catalog_status_network_label_);
  }
  lv_label_set_text(this->catalog_status_battery_label_, "\xEF\x89\x80");
  const lv_color_t network_color = (ethernet_connected || wifi_connected) ? lv_color_hex(0x55D6FF)
                                                                            : lv_color_hex(0x64748B);
  lv_obj_set_style_text_color(this->catalog_status_network_label_, network_color, 0);
  lv_obj_set_style_text_color(this->catalog_status_battery_label_, lv_color_hex(0x55D6FF), 0);
}

static bool background_file_name_(const char *name) {
  if (name == nullptr)
    return false;
  const std::string value(name);
  const size_t dot = value.rfind('.');
  if (dot == std::string::npos)
    return false;
  std::string ext = value.substr(dot);
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // PNG is decoded by the same PSRAM-safe decoder already used by PAXs and
  // store thumbnails. It is deliberately the first supported background
  // format so a bad JPEG decoder can never take down the launcher.
  return ext == ".png";
}

static std::string background_base_name_(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string PaxLoader::background_settings_path_() const {
  return this->install_dir_ + "/background.txt";
}

void PaxLoader::update_background_label_() {
  if (this->background_label_ == nullptr)
    return;
  const std::string name = this->background_selection_.empty()
                               ? std::string("BACKGROUND: NONE")
                               : std::string("BACKGROUND: ") + background_base_name_(this->background_selection_);
  lv_label_set_text(this->background_label_, name.c_str());
}

void PaxLoader::refresh_backgrounds() {
  this->background_paths_.clear();
  std::vector<std::string> roots;
  roots.push_back(this->data_root_);
  for (const auto &root : this->data_search_) {
    if (std::find(roots.begin(), roots.end(), root) == roots.end())
      roots.push_back(root);
  }
  for (const auto &root : roots) {
    const std::string virtual_folder = root + "/backgrounds";
    const std::string folder = runtime_path(virtual_folder.c_str());
    DIR *dir = opendir(folder.c_str());
    if (dir == nullptr)
      continue;
    while (dirent *entry = readdir(dir)) {
      if (!background_file_name_(entry->d_name) || std::strcmp(entry->d_name, ".") == 0 ||
          std::strcmp(entry->d_name, "..") == 0)
        continue;
      const std::string virtual_path = virtual_folder + "/" + entry->d_name;
      const std::string fs_path = runtime_path(virtual_path.c_str());
      struct stat info{};
      if (stat(fs_path.c_str(), &info) == 0 && S_ISREG(info.st_mode))
        this->background_paths_.push_back(virtual_path);
    }
    closedir(dir);
  }
  std::sort(this->background_paths_.begin(), this->background_paths_.end());
  auto current = std::find(this->background_paths_.begin(), this->background_paths_.end(), this->background_selection_);
  if (current != this->background_paths_.end())
    this->background_index_ = static_cast<size_t>(current - this->background_paths_.begin());
  else if (!this->background_selection_.empty()) {
    this->background_selection_.clear();
    this->background_index_ = 0;
    if (this->background_initialized_)
      this->apply_background_(std::string());
  }
  this->update_background_label_();
  ESP_LOGI(TAG, "Background folder scan: %u PNG file(s)", static_cast<unsigned>(this->background_paths_.size()));
}

void PaxLoader::load_saved_background_() {
  this->background_selection_.clear();
  const std::string path = runtime_path(this->background_settings_path_().c_str());
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return;
  char buffer[256]{};
  const size_t length = std::fread(buffer, 1, sizeof(buffer) - 1, file);
  std::fclose(file);
  buffer[length] = '\0';
  size_t trimmed = std::strlen(buffer);
  while (trimmed > 0 && (buffer[trimmed - 1] == '\n' || buffer[trimmed - 1] == '\r'))
    buffer[--trimmed] = '\0';
  const std::string saved(buffer);
  if (std::find(this->background_paths_.begin(), this->background_paths_.end(), saved) != this->background_paths_.end())
    this->background_selection_ = saved;
}

void PaxLoader::save_background_selection_() {
  const std::string path = runtime_path(this->background_settings_path_().c_str());
  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    ESP_LOGW(TAG, "Could not save background selection to %s (errno %d)", path.c_str(), errno);
    return;
  }
  const std::string value = this->background_selection_ + "\n";
  const bool ok = std::fwrite(value.data(), 1, value.size(), file) == value.size() && sync_file_(file, path.c_str());
  std::fclose(file);
  if (!ok)
    ESP_LOGW(TAG, "Could not commit background selection %s", path.c_str());
}

void PaxLoader::apply_background_(const std::string &path) {
  if (this->background_image_ == nullptr) {
    lv_obj_t *bottom = lv_layer_bottom();
    if (bottom == nullptr)
      return;
    // Pages are transparent when artwork is selected. Keep a solid fallback
    // behind the image so the launcher still has its normal dark background
    // while the first PNG is being decoded.
    lv_obj_set_style_bg_color(bottom, lv_color_hex(0x08111F), 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, 0);
    this->background_image_ = lv_image_create(bottom);
    lv_obj_remove_flag(this->background_image_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(this->background_image_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(this->background_image_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_opa(this->background_image_, LV_OPA_TRANSP, 0);
  }

  if (path.empty()) {
    lv_image_set_src(this->background_image_, nullptr);
    lv_obj_add_flag(this->background_image_, LV_OBJ_FLAG_HIDDEN);
    this->background_pixels_.reset();
    this->background_dsc_ = lv_image_dsc_t{};
    this->update_background_label_();
    return;
  }

  pngObject png{};
  const std::string fs_path = runtime_path(path.c_str());
  if (!loadPngFromFileRaw(fs_path.c_str(), &png, true, true) || png.data == nullptr || png.w == 0 || png.h == 0 ||
      png.w > 4096 || png.h > 4096) {
    if (png.data != nullptr)
      heap_caps_free(png.data);
    ESP_LOGW(TAG, "Background PNG could not be loaded: %s", path.c_str());
    return;
  }

  this->background_pixels_ = std::shared_ptr<uint16_t>(reinterpret_cast<uint16_t *>(png.data),
                                                        [](uint16_t *pixels) { heap_caps_free(pixels); });
  this->background_dsc_ = lv_image_dsc_t{};
  this->background_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
  this->background_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
  this->background_dsc_.header.w = png.w;
  this->background_dsc_.header.h = png.h;
  this->background_dsc_.header.stride = static_cast<uint32_t>(png.w) * sizeof(uint16_t);
  this->background_dsc_.data_size = static_cast<uint32_t>(png.w) * png.h * sizeof(uint16_t);
  this->background_dsc_.data = reinterpret_cast<const uint8_t *>(this->background_pixels_.get());

  const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
  const int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
  lv_image_set_src(this->background_image_, &this->background_dsc_);
  lv_obj_set_size(this->background_image_, screen_w, screen_h);
  lv_obj_align(this->background_image_, LV_ALIGN_CENTER, 0, 0);
  lv_image_set_inner_align(this->background_image_, LV_IMAGE_ALIGN_COVER);
  lv_obj_set_style_opa(this->background_image_, LV_OPA_100, 0);
  lv_obj_clear_flag(this->background_image_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_background(this->background_image_);
  this->update_background_label_();
  ESP_LOGI(TAG, "Background applied: %s (%ux%u)", path.c_str(), static_cast<unsigned>(png.w),
           static_cast<unsigned>(png.h));
}

void PaxLoader::ensure_background_() {
  if (this->background_initialized_)
    return;
  this->background_initialized_ = true;
  this->refresh_backgrounds();
  this->load_saved_background_();
  if (!this->background_selection_.empty()) {
    auto current = std::find(this->background_paths_.begin(), this->background_paths_.end(), this->background_selection_);
    if (current != this->background_paths_.end()) {
      this->background_index_ = static_cast<size_t>(current - this->background_paths_.begin());
      this->apply_background_(this->background_selection_);
    } else {
      this->background_selection_.clear();
    }
  }
  this->update_background_label_();
}

void PaxLoader::next_background() {
  this->ensure_background_();
  if (this->background_paths_.empty()) {
    ESP_LOGW(TAG, "No PNG backgrounds found; add artwork to /sd/backgrounds");
    this->play_ui_sound(UI_SOUND_CLICK);
    return;
  }
  if (this->background_selection_.empty()) {
    this->background_index_ = 0;
  } else {
    auto current = std::find(this->background_paths_.begin(), this->background_paths_.end(), this->background_selection_);
    this->background_index_ = current == this->background_paths_.end()
                                  ? 0
                                  : static_cast<size_t>(current - this->background_paths_.begin() + 1);
    if (this->background_index_ >= this->background_paths_.size()) {
      this->background_selection_.clear();
      this->background_index_ = 0;
      this->apply_background_(std::string());
      this->save_background_selection_();
      this->play_ui_sound(UI_SOUND_CONFIRM);
      return;
    }
  }
  this->background_selection_ = this->background_paths_[this->background_index_];
  this->apply_background_(this->background_selection_);
  this->save_background_selection_();
  this->play_ui_sound(UI_SOUND_CONFIRM);
}

struct PaxRomSelectorButtonContext {
  PaxLoader *loader;
  std::string path;
};

struct PaxRomArchiveButtonContext {
  PaxLoader *loader;
};

static void pax_rom_selector_button_event_cb(lv_event_t *event) {
  if (event == nullptr)
    return;
  auto *context = static_cast<PaxRomSelectorButtonContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    context->loader->play_ui_sound(UI_SOUND_CONFIRM);
    context->loader->select_rom(context->path);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

static void pax_rom_archive_button_event_cb(lv_event_t *event) {
  if (event == nullptr)
    return;
  auto *context = static_cast<PaxRomArchiveButtonContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    context->loader->play_ui_sound(UI_SOUND_CONFIRM);
    context->loader->open_archive_for_current_rom_selector();
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

struct PaxArchiveEntryContext {
  PaxLoader *loader;
  std::string url;
  std::string path;
  uint32_t size;
};

static void pax_archive_entry_event_cb(lv_event_t *event) {
  if (event == nullptr)
    return;
  auto *context = static_cast<PaxArchiveEntryContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    context->loader->play_ui_sound(UI_SOUND_CONFIRM);
    context->loader->request_upload_url(context->url, context->path, context->size, "");
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

struct ArchiveSystemDefinition {
  const char *title;
  const char *zip_file;
  const char *emulator;
  const char *destination;
  const char *extensions;
};

static const ArchiveSystemDefinition ARCHIVE_SYSTEMS[] = {
    {"Nintendo - NES", "Nintendo%20-%20NES.zip", "nes", "/sd/roms/nes", ".nes|.zip|"},
    {"Nintendo - SNES", "Nintendo%20-%20SNES.zip", "snes", "/sd/roms/snes", ".smc|.sfc|.zip|"},
    {"Nintendo - Game Boy", "Nintendo%20-%20Game%20Boy.zip", "gb", "/sd/roms/gb", ".gb|.zip|"},
    {"Nintendo - Game Boy Color", "Nintendo%20-%20Game%20Boy%20Color.zip", "gb", "/sd/roms/gb", ".gbc|.zip|"},
    {"Nintendo - Game Boy Advance", "Nintendo%20-%20Game%20Boy%20Advance.zip", "gba", "/sd/roms/gba", ".gba|.zip|"},
    {"Sega - Genesis", "Sega%20-%20Genesis.zip", "genesis", "/sd/roms/gen", ".md|.gen|.bin|.zip|"},
    {"Sega - Master System", "Sega%20-%20Master%20System.zip", "sms", "/sd/roms/sms", ".sms|.zip|"},
    {"Sega - Game Gear", "Sega%20-%20Game%20Gear.zip", "sms", "/sd/roms/sms", ".gg|.zip|"},
    {"Atari - 2600", "Atari%20-%202600.zip", "stella", "/sd/roms/a26", ".a26|.bin|.zip|"},
    {"NEC - TurboGrafx-16", "NEC%20-%20TurboGrafx-16.zip", "pce", "/sd/roms/pce", ".pce|.zip|"},
    {"Arcade - Mame 2003 Plus", "Arcade%20-%20Mame%202003%20Plus.zip", "mame", "/sd/roms/mame", ".zip|"},
};

struct PaxArchiveSystemButtonContext {
  PaxLoader *loader;
  size_t index;
};

static void pax_archive_system_button_event_cb(lv_event_t *event) {
  if (event == nullptr)
    return;
  auto *context = static_cast<PaxArchiveSystemButtonContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    context->loader->play_ui_sound(UI_SOUND_CONFIRM);
    context->loader->open_archive_system(context->index);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

struct PaxArchiveSearchButtonContext {
  PaxLoader *loader;
  size_t index;
};

static void pax_archive_search_button_event_cb(lv_event_t *event) {
  if (event == nullptr)
    return;
  auto *context = static_cast<PaxArchiveSearchButtonContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    context->loader->play_ui_sound(UI_SOUND_CONFIRM);
    context->loader->select_archive_result(context->index);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}
#endif

#ifdef PAX_LOADER_USE_USB_HIDX
static bool usb_mouse_button_pressed(binary_sensor::BinarySensor *sensor) {
  return sensor != nullptr && sensor->get_state_default(false);
}

#endif

void PaxLoader::setup() {
  if (PaxLoader::active_ != nullptr) {
    ESP_LOGE(TAG, "Only one PAX loader can be active");
    this->mark_failed();
    return;
  }
  if (this->display_ == nullptr) {
    ESP_LOGE(TAG, "A display_id is required");
    this->mark_failed();
    return;
  }

  // The panel: the YAML size, else the display's own. A display that reports
  // less than the 800x480 canvas (or no size at all) is taken to be the
  // 1024x600 panel this loader was written for.
  const bool panel_configured = this->panel_config_w_ > 0 && this->panel_config_h_ > 0;
  int panel_w = panel_configured ? this->panel_config_w_ : this->display_->get_native_width();
  int panel_h = panel_configured ? this->panel_config_h_ : this->display_->get_native_height();
  const bool panel_known = panel_w >= canvas::LEGACY_WIDTH && panel_h >= canvas::LEGACY_HEIGHT;
  if (!panel_known) {
    ESP_LOGW(TAG, "Display reports %dx%d, less than the %dx%d canvas; assuming a %dx%d panel", panel_w, panel_h,
             canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT, canvas::FALLBACK_PANEL_WIDTH, canvas::FALLBACK_PANEL_HEIGHT);
    panel_w = canvas::FALLBACK_PANEL_WIDTH;
    panel_h = canvas::FALLBACK_PANEL_HEIGHT;
  }
  panel_w = std::min(panel_w, canvas::MAX_SIDE);
  panel_h = std::min(panel_h, canvas::MAX_SIDE);
  this->geometry_.panel_w = panel_w;
  this->geometry_.panel_h = panel_h;
  this->geometry_.canvas_w = canvas::LEGACY_WIDTH;
  this->geometry_.canvas_h = canvas::LEGACY_HEIGHT;

  // Frame buffers for the largest canvas, the whole panel (canvas sizes are
  // even), allocated once. Short of PSRAM, every app stays at 800x480.
  if (!this->allocate_frame_buffers_(panel_w - panel_w % 2, panel_h - panel_h % 2) &&
      (panel_w - panel_w % 2 != canvas::LEGACY_WIDTH || panel_h - panel_h % 2 != canvas::LEGACY_HEIGHT)) {
    ESP_LOGW(TAG, "No PSRAM for %dx%d frame buffers; apps are limited to %dx%d", panel_w, panel_h,
             canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT);
    this->allocate_frame_buffers_(canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT);
  }
  // The canvas offered to apps that ask (display_get_size): the YAML one, else
  // the whole panel, or 800x480 when the panel's size is not known.
  int offer_w = this->default_canvas_w_, offer_h = this->default_canvas_h_;
  if (offer_w <= 0 || offer_h <= 0) {
    offer_w = panel_known || panel_configured ? this->max_canvas_w_ : canvas::LEGACY_WIDTH;
    offer_h = panel_known || panel_configured ? this->max_canvas_h_ : canvas::LEGACY_HEIGHT;
  }
  if (!canvas::size_ok(offer_w, offer_h, this->max_canvas_w_, this->max_canvas_h_)) {
    ESP_LOGW(TAG, "Default canvas %dx%d does not fit (even sizes from %dx%d to %dx%d); using %dx%d", offer_w, offer_h,
             canvas::MIN_WIDTH, canvas::MIN_HEIGHT, this->max_canvas_w_, this->max_canvas_h_, this->max_canvas_w_,
             this->max_canvas_h_);
    offer_w = this->max_canvas_w_;
    offer_h = this->max_canvas_h_;
  }
  this->default_canvas_w_ = offer_w;
  this->default_canvas_h_ = offer_h;
  this->emu_buffer_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
      64, 1, EMU_WIDTH * EMU_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  if (this->emu_buffer_ == nullptr) {
    this->emu_buffer_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
        64, 1, EMU_WIDTH * EMU_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  }
  this->stream_framebuffer_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
      64, 1, SCREEN_STREAM_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->stream_frame_packet_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
      64, 1, SCREEN_STREAM_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->stream_audio_buffer_ = static_cast<uint8_t *>(heap_caps_aligned_calloc(
      64, 1, SCREEN_STREAM_AUDIO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->stream_audio_packet_ = static_cast<uint8_t *>(heap_caps_aligned_calloc(
      64, 1, SCREEN_STREAM_AUDIO_PACKET_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->stream_mutex_ = xSemaphoreCreateMutex();
  this->display_mutex_ = xSemaphoreCreateRecursiveMutex();
  this->keyboard_mutex_ = xSemaphoreCreateMutex();
  this->ui_call_done_ = xSemaphoreCreateBinary();
  this->report_mutex_ = xSemaphoreCreateMutex();
  this->handoff_mutex_ = xSemaphoreCreateMutex();
  this->clear_mouse_delta_();

  if (this->framebuffer_ == nullptr || this->rotated_framebuffer_ == nullptr || this->ppa_framebuffer_ == nullptr || this->emu_buffer_ == nullptr ||
      this->display_mutex_ == nullptr ||
      this->keyboard_mutex_ == nullptr ||
      this->ui_call_done_ == nullptr) {
    ESP_LOGE(TAG, "PAX framebuffer allocation failed");
    this->mark_failed();
    return;
  }

  // Register one blocking SRM client for the shared PAX flush path. The
  // native launcher uses the same operation to rotate the landscape canvas;
  // keeping it here avoids walking all 384,000 pixels on the CPU each frame.
  ppa_client_config_t ppa_config = {
      .oper_type = PPA_OPERATION_SRM,
      .max_pending_trans_num = 1,
  };
  ppa_client_handle_t ppa_client = nullptr;
  if (ppa_register_client(&ppa_config, &ppa_client) == ESP_OK) {
    this->ppa_srm_client_ = ppa_client;
    ESP_LOGI(TAG, "PAX hardware SRM flush enabled");
  } else {
    ESP_LOGW(TAG, "PAX hardware SRM unavailable; using software rotation");
  }

  PaxLoader::active_ = this;
  if(esphome::global_preferences){
    runtime_settings_preference=esphome::global_preferences->make_preference<SavedRuntimeSettings>(RUNTIME_SETTINGS_KEY,true);
    SavedRuntimeSettings restored{};
    if(runtime_settings_preference.load(&restored) && restored.magic==RUNTIME_SETTINGS_MAGIC && restored.volume<=100 && restored.automatic<=1){
      this->master_volume_=restored.volume;
      this->set_auto_brightness_enabled(restored.automatic!=0);
      saved_runtime_settings=restored;
      ESP_LOGI(TAG,"Restored volume %u%% and automatic brightness %s",restored.volume,restored.automatic ? "on" : "off");
    }
  }
#ifdef PAX_LOADER_USE_USB_HIDX
  if (this->usb_hidx_ != nullptr) {
    this->game_menu_button_ = this->usb_hidx_->get_gamepad_button_capture_sensor();
    if (this->game_menu_button_ == nullptr) {
      // Register the HID Capture button internally when the YAML has not
      // exposed it as an entity. This keeps the menu available on every USB
      // gamepad without adding a per-PAX input mapping or changing the ABI.
      this->game_menu_capture_fallback_.publish_initial_state(false);
      this->usb_hidx_->register_gamepad_button_capture_sensor(&this->game_menu_capture_fallback_);
      this->game_menu_button_ = &this->game_menu_capture_fallback_;
    }
    this->game_menu_button_->add_on_state_callback([this](bool pressed) {
      portENTER_CRITICAL(&this->game_menu_button_lock_);
      if (pressed && !this->game_menu_capture_down_)
        this->game_menu_toggle_pending_ = true;
      this->game_menu_capture_down_ = pressed;
      portEXIT_CRITICAL(&this->game_menu_button_lock_);
      ESP_LOGI(TAG, "USB gamepad Capture button %s", pressed ? "pressed" : "released");
    });
    ESP_LOGI(TAG, "PAX in-game menu bound to USB gamepad Capture button");
  }
#endif
  if (esphome::global_preferences != nullptr) {
    auto preference = esphome::global_preferences->make_preference<SavedNetworkConfig>(
        NETWORK_PREFERENCE_KEY, true);
    SavedNetworkConfig saved{};
    if (preference.load(&saved) && saved.magic == NETWORK_PREFERENCE_MAGIC) {
      pax_network_config_t config{};
      config.dhcp_enabled = saved.dhcp_enabled;
      config.ipv4 = saved.ipv4;
      config.netmask = saved.netmask;
      config.gateway = saved.gateway;
      config.dns = saved.dns;
      const int result = PaxLoader::svc_network_config_set(&config);
      if (result == 0)
        ESP_LOGI(TAG, "Restored saved %s network configuration",
                 saved.dhcp_enabled ? "DHCP" : "static IPv4");
      else
        ESP_LOGW(TAG, "Could not restore saved network configuration (%d)", result);
    }
  }
  this->load_archive_collection_();
  if (this->stream_framebuffer_ != nullptr && this->stream_frame_packet_ != nullptr &&
      this->stream_audio_buffer_ != nullptr && this->stream_audio_packet_ != nullptr && this->stream_mutex_ != nullptr) {
    // PAX games such as Quake run their main loop at priority 5 on core 0.
    // A lower-priority stream task can be starved indefinitely even though it
    // deliberately sleeps between packets, and at the same priority a game
    // that never blocks (OpenLara at ~35 FPS once the present got fast) still
    // kept screenshots from arriving. One above lets it preempt: it only runs
    // when the network needs it, then yields for 200 ms after every batch.
    // 16 KiB: `readfile` runs FATFS (long-file-name buffers) and stat() here.
    const BaseType_t result = xTaskCreatePinnedToCore(
        &PaxLoader::screen_stream_task_entry_, "pax_stream", 16384, this, 6,
        &this->stream_task_handle_, 0);
    if (result != pdPASS)
      ESP_LOGW(TAG, "Could not start optional screen stream task");
    else
      ESP_LOGI(TAG, "Diagnostic screen stream listening on TCP port %u", SCREEN_STREAM_PORT);
  }
  this->clear_(0x0000);

  if (this->toggle_button_ != nullptr) {
    // The callback latches a short press so it cannot be lost between PAX
    // frames. PAX runs in a worker task while ESPHome keeps servicing USB,
    // GPIO/ADC, touchscreen, audio, and network components.
    // Keep the close request sticky until the PAX has observed it; otherwise
    // a quick press/release between two PAX frames can be lost.
    this->toggle_button_->add_on_state_callback([this](bool state) {
      this->toggle_button_state_ = state;
      if (state) {
        if (this->launched_) {
          this->toggle_close_requested_ = true;
          ESP_LOGI(TAG, "PAX toggle close request latched");
        }
      } else if (!this->launched_) {
        this->toggle_wait_release_ = false;
        this->toggle_close_requested_ = false;
      }
    });
  }
  if (this->launch_button_ != nullptr) {
    // Opening has its own control so a close press cannot immediately start
    // the PAX again. The request is latched by the binary-sensor edge and
    // the level is also checked in loop() as a fallback.
    this->launch_button_->add_on_state_callback([this](bool state) {
      this->launch_button_state_ = state;
      if (state) {
        if (!this->launched_ && !this->launch_wait_release_) {
          this->launch_pending_ = true;
          this->launch_wait_release_ = true;
          ESP_LOGI(TAG, "PAX launch request latched from launch button");
        }
      } else {
        this->launch_wait_release_ = false;
      }
    });
  }
#ifdef PAX_LOADER_USE_USB_HIDX
  // USB keyboard keys and mouse/touchpad motion reach the PAX through
  // usb_hidx's keyboard text sensor and mouse X/Y sensors. Subscribe here so
  // the device YAML only has to declare those sensors, no lambdas.
  if (this->usb_hidx_ != nullptr) {
#ifdef USE_TEXT_SENSOR
    if (auto *keys = this->usb_hidx_->get_keyboard_sensor(); keys != nullptr)
      keys->add_on_state_callback([this](const std::string &key) { this->enqueue_keyboard_text(key); });
#endif
#ifdef USE_SENSOR
    if (auto *mouse_x = this->usb_hidx_->get_mouse_x_sensor(); mouse_x != nullptr)
      mouse_x->add_on_state_callback([this](float dx) { this->enqueue_mouse_delta(dx, 0.0f); });
    if (auto *mouse_y = this->usb_hidx_->get_mouse_y_sensor(); mouse_y != nullptr)
      mouse_y->add_on_state_callback([this](float dy) { this->enqueue_mouse_delta(0.0f, dy); });
    if (auto *mouse_wheel = this->usb_hidx_->get_mouse_wheel_sensor(); mouse_wheel != nullptr)
      mouse_wheel->add_on_state_callback([this](float wheel) { this->enqueue_mouse_wheel(wheel); });
#endif
  }
#endif
  ESP_LOGCONFIG(TAG, "PAX runtime ready");
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
  const std::string mapped = runtime_path(this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  SD path: %s", mapped.c_str());
  ESP_LOGCONFIG(TAG, "  Display: %dx%d panel; apps draw %dx%d unless they pick a canvas (up to %dx%d, offered %dx%d)",
                this->geometry_.panel_w, this->geometry_.panel_h, canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT,
                this->max_canvas_w_, this->max_canvas_h_, this->default_canvas_w_, this->default_canvas_h_);
#ifdef PAX_LOADER_USE_USB_HIDX
  const char *usb_status = this->usb_hidx_ ? "configured" : "disabled";
#else
  const char *usb_status = "disabled";
#endif
  ESP_LOGCONFIG(TAG, "  Touch: %s, audio: %s, USB input: %s", this->touchscreen_ ? "configured" : "disabled",
                this->speaker_ ? "configured" : "disabled", usb_status);

  if (!this->catalog_url_.empty()) {
    // Network may still be negotiating when setup() runs.  Fetch the HTML
    // index after the rest of ESPHome has had time to bring Ethernet/Wi-Fi up.
    this->set_timeout("pax_catalog_initial", 5000, [this]() { this->refresh_catalog(); });
  }

  if (this->autostart_) {
    // Let USB host enumeration complete before an optional autostart.
    // Official Switch Pro controllers need several report/command exchanges
    // after enumeration.  Give that handshake time to finish while the
    // normal ESPHome loop owns USB before the PAX worker starts.
    this->set_timeout("pax_autostart", 5000, [this]() { this->launch_pending_ = true; });
  }
}

// ── Canvas size ──────────────────────────────────────────────────────────────

void PaxLoader::free_frame_buffers_() {
  heap_caps_free(this->framebuffer_);
  heap_caps_free(this->rotated_framebuffer_);
  heap_caps_free(this->ppa_framebuffer_);
  this->framebuffer_ = nullptr;
  this->rotated_framebuffer_ = nullptr;
  this->ppa_framebuffer_ = nullptr;
  this->frame_alloc_bytes_ = 0;
}

// The three canvas buffers, each big enough for a width x height canvas and
// whole cache lines (PPA output and cache syncs work in those).
bool PaxLoader::resize_frame_buffers_(int width, int height) {
  const size_t bytes = canvas::align_bytes(canvas::frame_bytes(width, height));
  if (this->framebuffer_ != nullptr && bytes == this->frame_alloc_bytes_)
    return true;
  auto allocate = [bytes]() {
    return static_cast<uint16_t *>(
        heap_caps_aligned_calloc(canvas::CACHE_LINE, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  };
  // Allocate the complete replacement before retiring any current buffer.
  // A failed resize must leave the app's canvas and framebuffer valid.
  uint16_t *next_frame = allocate();
  uint16_t *next_rotated = allocate();
  uint16_t *next_ppa = allocate();
  if (next_frame == nullptr || next_rotated == nullptr || next_ppa == nullptr) {
    heap_caps_free(next_frame);
    heap_caps_free(next_rotated);
    heap_caps_free(next_ppa);
    return false;
  }
  const size_t previous_bytes = this->frame_alloc_bytes_;
  this->free_frame_buffers_();
  this->framebuffer_ = next_frame;
  this->rotated_framebuffer_ = next_rotated;
  this->ppa_framebuffer_ = next_ppa;
  this->frame_alloc_bytes_ = bytes;
  this->last_frame_direct_ = false;
  this->native_rgb888_frame_ = nullptr;
  ESP_LOGI(TAG, "PAX canvas buffer resize: %u -> %u bytes each (%u total)",
           static_cast<unsigned>(previous_bytes), static_cast<unsigned>(bytes), static_cast<unsigned>(3 * bytes));
  return true;
}

bool PaxLoader::allocate_frame_buffers_(int width, int height) {
  if (!this->resize_frame_buffers_(width, height))
    return false;
  // These limits describe supported canvases, not the current allocation.
  this->max_canvas_w_ = width;
  this->max_canvas_h_ = height;
  return true;
}

bool PaxLoader::apply_canvas_(int width, int height, bool blank_panel) {
  const size_t required = canvas::align_bytes(canvas::frame_bytes(width, height));
  if (required != this->frame_alloc_bytes_ && !this->resize_frame_buffers_(width, height)) {
    ESP_LOGW(TAG, "PAX canvas buffer resize to %dx%d failed", width, height);
    if (required > this->frame_alloc_bytes_)
      return false;
    // Shrinking is optional when allocation is fragmented. The old larger
    // buffers can still represent the requested canvas safely.
  }
  ESP_LOGI(TAG, "PAX canvas apply begin: %dx%d blank=%d buffers=%u", width, height,
           blank_panel ? 1 : 0, static_cast<unsigned>(this->frame_alloc_bytes_));
  this->geometry_.canvas_w = width;
  this->geometry_.canvas_h = height;
  // Nothing of the old canvas may show through: its rows have another stride.
  std::memset(this->framebuffer_, 0, this->frame_alloc_bytes_);
  std::memset(this->rotated_framebuffer_, 0, this->frame_alloc_bytes_);
  // Written back now so no dirty line can later land on PPA output, and
  // dropped from the cache so reads see what the PPA writes.
  esp_cache_msync(this->rotated_framebuffer_, this->frame_alloc_bytes_,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  this->direct_frame_[2] = 0;  // the direct path clears its border again
  this->last_frame_direct_ = false;
  this->native_rgb888_frame_ = nullptr;
  if (blank_panel && this->display_ != nullptr) {
    // A full-panel clear (or another full-canvas PAX) may have changed either
    // DSI scanout buffer since it was last initialized. Do not trust the
    // cached pointers: when the next app uses a smaller canvas, each back
    // buffer must be blackened again before PPA paints the game rectangle,
    // otherwise stale pixels outside that rectangle flash as buffers swap.
    this->initialized_panel_framebuffers_[0] = nullptr;
    this->initialized_panel_framebuffers_[1] = nullptr;
    // The old canvas, its border and the close control may be anywhere: clear
    // the whole panel, in bands of the (now black) rotated buffer.
    const int panel_w = this->geometry_.panel_w, panel_h = this->geometry_.panel_h;
    const int rows = std::max(1, static_cast<int>(this->frame_alloc_bytes_ / sizeof(uint16_t) / panel_w));
    ESP_LOGI(TAG, "PAX canvas panel clear begin: panel=%dx%d rows=%d", panel_w, panel_h, rows);
    for (int y = 0; y < panel_h; y += rows) {
      this->display_->draw_pixels_at(0, y, panel_w, std::min(rows, panel_h - y),
                                     reinterpret_cast<const uint8_t *>(this->rotated_framebuffer_),
                                     display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false);
    }
    ESP_LOGI(TAG, "PAX canvas panel clear complete");
  }
  ESP_LOGI(TAG, "PAX canvas apply complete: %dx%d", width, height);
  return true;
}

// Called by the app (display_set_canvas), normally from its drawing task.
int PaxLoader::set_canvas_(int width, int height) {
  ESP_LOGI(TAG, "PAX canvas request: %dx%d", width, height);
  if (this->framebuffer_ == nullptr || !canvas::size_ok(width, height, this->max_canvas_w_, this->max_canvas_h_)) {
    ESP_LOGW(TAG, "PAX canvas %dx%d refused: even sizes from %dx%d to %dx%d", width, height, canvas::MIN_WIDTH,
             canvas::MIN_HEIGHT, this->max_canvas_w_, this->max_canvas_h_);
    return -1;
  }
  if (this->display_mutex_ != nullptr) {
    ESP_LOGI(TAG, "PAX canvas waiting for display lock");
    if (xSemaphoreTakeRecursive(this->display_mutex_, pdMS_TO_TICKS(500)) != pdTRUE) {
      // Canvas selection happens before the PAX produces its first frame. A
      // dead display lock must not pin the worker forever and then be left
      // locked when the no-frame watchdog reclaims that worker. Refuse this
      // optional resize so the app can continue on its legacy canvas instead.
      ESP_LOGW(TAG, "PAX canvas %dx%d refused: display lock busy", width, height);
      return -1;
    }
    ESP_LOGI(TAG, "PAX canvas acquired display lock");
  }
  const bool change = width != this->geometry_.canvas_w || height != this->geometry_.canvas_h;
  if (change) {
    ESP_LOGI(TAG, "PAX canvas changing from %dx%d", this->geometry_.canvas_w, this->geometry_.canvas_h);
    if (!this->apply_canvas_(width, height, true)) {
      if (this->display_mutex_ != nullptr)
        xSemaphoreGiveRecursive(this->display_mutex_);
      return -1;
    }
  }
  this->canvas_chosen_ = true;
  if (this->display_mutex_ != nullptr)
    xSemaphoreGiveRecursive(this->display_mutex_);
  if (change) {
    const canvas::Geometry &g = this->geometry_;
    ESP_LOGI(TAG, "PAX canvas: %dx%d at (%d,%d) on the %dx%d panel, controls centered at x=%d", width, height,
             g.x(), g.y(), g.panel_w, g.panel_h, g.control_group_x());
  }
  return 0;
}

// Before an app starts (and after one ends): the 800x480 canvas again, with
// no app loop running. Waits only briefly for the display lock: an app task
// that is gone may have died holding it.
bool PaxLoader::restore_legacy_canvas_() {
  this->canvas_chosen_ = false;
  if (this->framebuffer_ == nullptr)
    return false;
  if (this->geometry_.canvas_w == canvas::LEGACY_WIDTH && this->geometry_.canvas_h == canvas::LEGACY_HEIGHT)
    return true;
  const bool locked = this->display_mutex_ != nullptr &&
                      xSemaphoreTakeRecursive(this->display_mutex_, pdMS_TO_TICKS(200)) == pdTRUE;
  if (this->display_mutex_ != nullptr && !locked) {
    ESP_LOGW(TAG, "Cannot restore legacy canvas while display lock is busy");
    return false;
  }
  const bool restored = this->apply_canvas_(canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT, true);
  if (locked)
    xSemaphoreGiveRecursive(this->display_mutex_);
  return restored;
}

// Clear all panel pixels even when the outgoing and next apps both use the
// legacy canvas. Otherwise the old frame can remain visible around a smaller
// canvas (and the PAX close overlay is not cleared by a canvas-only flush).
void PaxLoader::blank_panel_() {
  if (this->display_ == nullptr || this->framebuffer_ == nullptr ||
      this->rotated_framebuffer_ == nullptr)
    return;
  const bool locked = this->display_mutex_ == nullptr ||
                      xSemaphoreTakeRecursive(this->display_mutex_, pdMS_TO_TICKS(200)) == pdTRUE;
  if (!locked) {
    ESP_LOGW(TAG, "Could not acquire display lock to blank the panel during PAX unload");
    return;
  }
  this->apply_canvas_(this->geometry_.canvas_w, this->geometry_.canvas_h, true);
  if (this->display_mutex_ != nullptr)
    xSemaphoreGiveRecursive(this->display_mutex_);
}

bool PaxLoader::prepare_canvas_for_app_(const std::string &source) {
  if (!this->restore_legacy_canvas_())
    return false;
  const std::string app = canvas::app_key(source);
  const std::string setting = this->get_app_canvas(app);
  int w = 0, h = 0;
  const bool from_setting = !setting.empty() && canvas::parse_size(setting, &w, &h) &&
                            canvas::size_ok(w, h, this->max_canvas_w_, this->max_canvas_h_);
  // No setting: the size the app's listing recommends, if it fits.
  const std::string recommended = from_setting ? std::string() : this->listing_recommended_canvas_(source);
  const bool from_listing = !recommended.empty() && canvas::parse_size(recommended, &w, &h) &&
                            canvas::size_ok(w, h, this->max_canvas_w_, this->max_canvas_h_);
  this->offered_canvas_w_ = from_setting || from_listing ? w : this->default_canvas_w_;
  this->offered_canvas_h_ = from_setting || from_listing ? h : this->default_canvas_h_;
  ESP_LOGI(TAG, "PAX canvas for %s: %dx%d until the app picks one; it is offered %dx%d (%s)", app.c_str(),
           canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT, this->offered_canvas_w_, this->offered_canvas_h_,
           from_setting ? "its Screen setting" : from_listing ? "recommended by its listing" : "device default");
  return true;
}

// Runs on the LVGL thread (the main loop). Copies the snapshot into a plain
// PSRAM buffer, dropping any row padding, and frees LVGL's buffer here so the
// stream task never touches LVGL memory.
void PaxLoader::take_menu_shot_() {
#if defined(PAX_LOADER_USE_LVGL) && defined(LV_USE_SNAPSHOT) && LV_USE_SNAPSHOT
  if (this->menu_shot_ready_ || this->lvgl_ == nullptr || !this->lvgl_->is_loop_started())
    return;
  lv_draw_buf_t *shot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  if (shot == nullptr) {
    ESP_LOGW(TAG, "Menu screenshot: snapshot failed (out of memory?)");
    this->menu_shot_ready_ = true;  // send an empty frame rather than hang
    return;
  }
  const uint32_t w = shot->header.w, h = shot->header.h, stride = shot->header.stride;
  auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (pixels != nullptr) {
    for (uint32_t y = 0; y < h; y++)
      std::memcpy(pixels + y * w, shot->data + y * stride, w * sizeof(uint16_t));
  }
  lv_draw_buf_destroy(shot);
  // What the top layer shows (a store detail page, the progress panel) is not
  // part of the screen: blend each visible child on top.
  lv_obj_t *top = lv_layer_top();
  for (uint32_t i = 0; pixels != nullptr && top != nullptr && i < lv_obj_get_child_count(top); i++) {
    lv_obj_t *child = lv_obj_get_child(top, static_cast<int32_t>(i));
    if (child == nullptr || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
      continue;
    lv_draw_buf_t *layer = lv_snapshot_take(child, LV_COLOR_FORMAT_ARGB8888);
    if (layer == nullptr)
      continue;
    lv_area_t area;
    lv_obj_get_coords(child, &area);
    // The snapshot is the object plus its extra draw area on every side.
    const int32_t x0 = area.x1 - (static_cast<int32_t>(layer->header.w) - lv_area_get_width(&area)) / 2;
    const int32_t y0 = area.y1 - (static_cast<int32_t>(layer->header.h) - lv_area_get_height(&area)) / 2;
    for (int32_t y = 0; y < static_cast<int32_t>(layer->header.h); y++) {
      const int32_t sy = y0 + y;
      if (sy < 0 || sy >= static_cast<int32_t>(h))
        continue;
      const uint8_t *src = layer->data + y * layer->header.stride;
      for (int32_t x = 0; x < static_cast<int32_t>(layer->header.w); x++) {
        const int32_t sx = x0 + x;
        const uint8_t *p = src + x * 4;  // B, G, R, A
        const uint32_t a = p[3];
        if (sx < 0 || sx >= static_cast<int32_t>(w) || a == 0)
          continue;
        uint16_t &d = pixels[sy * w + sx];
        const uint32_t dr = (d >> 8) & 0xF8, dg = (d >> 3) & 0xFC, db = (d << 3) & 0xF8;
        const uint32_t r = (p[2] * a + dr * (255 - a)) / 255, g = (p[1] * a + dg * (255 - a)) / 255,
                       b = (p[0] * a + db * (255 - a)) / 255;
        d = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
    }
    lv_draw_buf_destroy(layer);
  }
  this->menu_shot_ = pixels;
  this->menu_shot_w_ = pixels != nullptr ? static_cast<uint16_t>(w) : 0;
  this->menu_shot_h_ = pixels != nullptr ? static_cast<uint16_t>(h) : 0;
  this->menu_shot_ready_ = true;
#endif
}

void PaxLoader::process_ui_call_() {
  if (this->ui_call_done_ == nullptr)
    return;

  pax_ui_call_fn_t callback = nullptr;
  void *arg = nullptr;
  portENTER_CRITICAL(&this->ui_call_lock_);
  if (this->ui_call_state_ == 1) {
    this->ui_call_state_ = 2;
    callback = this->ui_call_callback_;
    arg = this->ui_call_arg_;
  }
  portEXIT_CRITICAL(&this->ui_call_lock_);

  if (callback == nullptr)
    return;

  // This is deliberately outside the critical section: LVGL may take
  // semaphores and perform a complete frame flush here.
  const int result = callback(arg);

  portENTER_CRITICAL(&this->ui_call_lock_);
  this->ui_call_result_ = result;
  this->ui_call_callback_ = nullptr;
  this->ui_call_arg_ = nullptr;
  this->ui_call_state_ = 3;
  portEXIT_CRITICAL(&this->ui_call_lock_);
  xSemaphoreGive(this->ui_call_done_);
}

void PaxLoader::loop() {
  this->update_hardware_();
  SavedRuntimeSettings current{RUNTIME_SETTINGS_MAGIC,static_cast<uint8_t>(this->master_volume_),
                               static_cast<uint8_t>(this->auto_brightness_enabled_),{0,0}};
  if(esphome::global_preferences && memcmp(&current,&saved_runtime_settings,sizeof(current))){
    if(runtime_settings_preference.save(&current))saved_runtime_settings=current;
  }
  pax_wifi::loop();
  pax_c6::loop();
  if (pax_firmware::pending() && this->app_handle_ != nullptr && !this->global_close_requested_)
    this->request_close();
  pax_firmware::loop(this->app_handle_ == nullptr && this->pax_task_handle_ == nullptr &&
                     this->pax_load_task_handle_ == nullptr && !this->pax_loading_);
  static uint32_t last_firmware_state = PAX_FIRMWARE_IDLE, last_firmware_done = 0;
  auto firmware = pax_firmware::snapshot();
  if (firmware.state >= PAX_FIRMWARE_INSTALLING &&
      (firmware.state != last_firmware_state || firmware.done != last_firmware_done)) {
    this->set_progress_(firmware.state != PAX_FIRMWARE_ERROR, firmware.done, firmware.total, "%s", firmware.message);
  }
  last_firmware_state = firmware.state; last_firmware_done = firmware.done;
  // Tiny active profile: refresh independently of the running app and keep the
  // raw overlays in the same theme as the launcher without adding SDK services.
  static int64_t last_theme_check=0;
  int64_t theme_now=esp_timer_get_time();
  if(theme_now-last_theme_check>=2000000){
    last_theme_check=theme_now;char json[2049]={0};pax_theme_profile_t theme=pax_theme_presets[0];
    FILE *file=fopen(runtime_path(PAX_THEME_FILE).c_str(),"rb");
    if(file){size_t n=fread(json,1,sizeof(json)-1,file);bool complete=fgetc(file)==EOF;fclose(file);if(complete && n>0)(void)pax_theme_parse(json,&theme);}
    static pax_theme_profile_t previous={};
    if(memcmp(previous.colors,theme.colors,sizeof(theme.colors))!=0){
      if(this->display_mutex_)xSemaphoreTakeRecursive(this->display_mutex_,portMAX_DELAY);
      overlay_theme::apply(theme);previous=theme;this->game_menu_frame_dirty_=true;
      this->redraw_control_overlay_now_(true);
      if(this->display_mutex_)xSemaphoreGiveRecursive(this->display_mutex_);
    }
  }
  // A running PAX may use ui_call_sync() to execute LVGL work on this task.
  // Service it before any early-return or launch bookkeeping below.
  this->process_ui_call_();
  if (!this->ui_sounds_ready_) {
    this->ui_sounds_ready_ = true;
    ESP_LOGI(TAG, "PAX UI sounds ready: source=%u Hz, volume=%ld%%, speaker=%s",
             UiSoundGenerator::SAMPLE_RATE, static_cast<long>(this->master_volume_),
             this->speaker_ != nullptr ? "attached" : "missing");
    if (this->speaker_ != nullptr && this->master_volume_ > 0) {
      this->audio_sample_rate_ = UiSoundGenerator::SAMPLE_RATE;
      this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 2, UiSoundGenerator::SAMPLE_RATE));
      this->speaker_->set_mute_state(false);
      this->speaker_->set_volume(static_cast<float>(this->master_volume_) / 100.0f);
      // Do not start the I2S DMA engine at boot. UI sounds start it on demand;
      // leaving it stopped keeps DMA-capable internal RAM available to the
      // SDMMC driver and avoids a retry loop before any sound is requested.
    }
  }
  this->update_auto_brightness_();
#ifdef PAX_LOADER_USE_LVGL
  this->ensure_background_();
  this->update_catalog_status_();
#endif
  if (this->menu_shot_wanted_) {
    this->menu_shot_wanted_ = false;
    if (!this->launched_)
      this->take_menu_shot_();
  }
  this->update_progress_ui_();
  this->poll_info_fetch_();
  if (!this->launched_ && (this->catalog_deferred_ || this->info_deferred_)) {
    // Store fetches held back while an app ran (refresh_catalog, start_info_fetch_).
    const bool catalog = this->catalog_deferred_;
    this->catalog_deferred_ = false;
    this->info_deferred_ = false;
    if (catalog) {
      this->refresh_catalog();  // fetches the listings again when it finishes
    } else {
      this->catalog_generation_--;  // same catalog: keep its generation
      this->start_info_fetch_();
    }
  }
  this->poll_install_();
  if (this->catalog_loading_ && this->catalog_done_) {
    if (this->pax_catalog_task_handle_ != nullptr) {
      vTaskDelete(this->pax_catalog_task_handle_);
      this->pax_catalog_task_handle_ = nullptr;
    }
    this->catalog_loading_ = false;
    if (this->catalog_fetch_url_ != this->catalog_url_) {
      // Another catalog was chosen while this one loaded: show that one instead.
      this->refresh_catalog();
    } else if (this->catalog_result_ == ESP_OK) {
      this->catalog_entries_ = parse_pax_catalog(this->catalog_html_, this->catalog_url_);
      // The parsed catalog keeps only name/URL pairs. Release the full HTML
      // before starting per-app metadata work; on a large store this is a
      // useful PSRAM saving even when the rich store view is enabled.
      this->catalog_html_.clear();
      this->catalog_html_.shrink_to_fit();
      ESP_LOGI(TAG, "PAX catalog found %u application(s)",
               static_cast<unsigned>(this->catalog_entries_.size()));
      for (const auto &entry : this->catalog_entries_)
        ESP_LOGI(TAG, "  PAX: %s -> %s", entry.first.c_str(), entry.second.c_str());
      this->start_info_fetch_();
#ifdef PAX_LOADER_USE_LVGL
      this->catalog_ui_pending_ = true;
#endif
    } else {
      const esp_err_t error = static_cast<esp_err_t>(this->catalog_result_);
      ESP_LOGW(TAG, "PAX catalog request failed: %s (%s, 0x%x)", http_error_hint(error), esp_err_to_name(error),
               this->catalog_result_);
#ifdef PAX_LOADER_USE_LVGL
      this->catalog_ui_pending_ = true;  // replace "Loading..." with the empty list
#endif
    }
  }
  if (this->archive_mode_ == ARCHIVE_VIEW_SEARCH && this->archive_search_pending_ && !this->archive_loading_ &&
      this->archive_search_due_us_ > 0 && esp_timer_get_time() >= this->archive_search_due_us_)
    this->start_archive_search_();
  if (this->archive_loading_ && this->archive_done_) {
    if (this->archive_task_handle_ != nullptr) {
      vTaskDelete(this->archive_task_handle_);
      this->archive_task_handle_ = nullptr;
    }
    this->archive_loading_ = false;
    if (this->archive_result_ == ESP_OK && this->archive_mode_ == ARCHIVE_VIEW_SEARCH) {
      this->archive_search_results_.clear();
      json::parse_json(this->archive_html_, [this](JsonObject root) -> bool {
        JsonArray docs = root["response"]["docs"].as<JsonArray>();
        for (JsonObject doc : docs) {
          const char *identifier = doc["identifier"] | "";
          if (identifier == nullptr || *identifier == '\0')
            continue;
          ArchiveSearchResult result;
          result.identifier = identifier;
          result.title = doc["title"] | identifier;
          if (result.title.empty())
            result.title = result.identifier;
          this->archive_search_results_.push_back(std::move(result));
          if (this->archive_search_results_.size() >= 20)
            break;
        }
        return true;
      });
      const std::string wanted_identifier = archive_search_slug(this->archive_search_term_);
      const std::string wanted_title = launcher_search_lower(this->archive_search_term_);
      std::stable_sort(this->archive_search_results_.begin(), this->archive_search_results_.end(),
                       [&wanted_identifier, &wanted_title](const ArchiveSearchResult &left,
                                                           const ArchiveSearchResult &right) {
                         auto rank = [&wanted_identifier, &wanted_title](const ArchiveSearchResult &item) {
                           if (launcher_search_lower(item.identifier) == wanted_identifier)
                             return 0;
                           if (launcher_search_lower(item.title) == wanted_title)
                             return 1;
                           return 2;
                         };
                         return rank(left) < rank(right);
                       });
      ESP_LOGI(TAG, "Archive.org search found %u collection(s)",
               static_cast<unsigned>(this->archive_search_results_.size()));
    } else if (this->archive_result_ == ESP_OK) {
      std::string destination = this->archive_system_dest_;
      std::string extensions = this->archive_system_exts_;
      if (destination.empty() && !this->archive_sources_.empty()) {
        const auto &source = this->archive_sources_[this->archive_index_ < this->archive_sources_.size() ? this->archive_index_ : 0];
        destination = source.destination;
        extensions = source.extensions;
      }
      const auto parsed = parse_archive_listing(this->archive_html_, this->archive_fetch_url_, extensions);
      this->archive_entries_.clear();
      for (const auto &entry : parsed) {
        ArchiveEntry item;
        item.label = entry.first;
        item.url = entry.second;
        item.path = destination;
        if (!item.path.empty() && item.path.back() != '/')
          item.path += '/';
        // ROM sets commonly use spaces and parentheses in filenames. Keep
        // the readable member name on screen, but use the loader's portable
        // filename alphabet on the card so the same path can be passed to
        // every emulator.
        std::string safe_name = item.label;
        for (char &character : safe_name) {
          if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '.' || character == '_' ||
                character == '-'))
            character = '_';
        }
        item.path += safe_name;
        // data::safe_target rejects traversal and unusual absolute paths. Keep
        // the same guard here so a malformed HTML listing never reaches the
        // SD writer.
        if (item.path.rfind("/sd/", 0) != 0 || !data::safe_target(item.path.substr(4)))
          continue;
        this->archive_entries_.push_back(std::move(item));
      }
      ESP_LOGI(TAG, "Archive.org source found %u member(s)",
               static_cast<unsigned>(this->archive_entries_.size()));
    } else {
      const esp_err_t error = static_cast<esp_err_t>(this->archive_result_);
      ESP_LOGW(TAG, "Archive.org member list request failed: %s (%s, 0x%x)", http_error_hint(error),
               esp_err_to_name(error), this->archive_result_);
    }
#ifdef PAX_LOADER_USE_LVGL
    this->archive_ui_pending_ = true;
#endif
  }
#ifdef PAX_LOADER_USE_LVGL
  // While a PAX runs, the rebuild waits: LVGL is paused under the app.
  if (this->catalog_ui_pending_ && !this->launched_ && this->catalog_container_ != nullptr &&
      this->lvgl_ != nullptr && this->lvgl_->is_loop_started()) {
    this->update_catalog_ui_();
    this->catalog_ui_pending_ = false;
  }
  if (!this->launched_ && this->lvgl_ != nullptr &&
      this->lvgl_->is_loop_started()) {
    this->handle_launcher_controls_();
  }
  if (this->archive_ui_pending_ && !this->launched_ && this->archive_container_ != nullptr &&
      this->lvgl_ != nullptr && this->lvgl_->is_loop_started()) {
    this->update_archive_ui_();
    this->archive_ui_pending_ = false;
  }
#endif

  // After a PAX exits, wait for a real release before accepting the next
  // press. The state callback above latches short presses, while this level
  // check clears the guard even when no new HID report has arrived yet.
  if (!this->launched_ && this->toggle_button_ != nullptr) {
    const bool pressed = this->toggle_button_->get_state_default(false);
    if (!pressed) {
      this->toggle_wait_release_ = false;
      this->toggle_close_requested_ = false;
    }
    this->toggle_button_state_ = pressed;
  }

  if (!this->launched_ && this->launch_button_ != nullptr) {
    const bool pressed = this->launch_button_->get_state_default(false);
    if (!pressed)
      this->launch_wait_release_ = false;
    if (pressed && !this->launch_button_state_ && !this->launch_wait_release_) {
      this->launch_pending_ = true;
      this->launch_wait_release_ = true;
    }
    this->launch_button_state_ = pressed;
  }

  // Poll this independently of the app. Some small PAXs do not call the
  // optional touch service themselves, but the shared close control must
  // still work for them.
  if (this->launched_) {
    this->poll_game_menu_();
    this->poll_exit_combo_();
    this->poll_close_button_();
    this->snapshot_touches_();
#ifdef PAX_LOADER_USE_LVGL
    // ESPHome's LVGL resumes itself on any touch or key while paused when
    // `resume_on_input` is on (its default), and then repaints the launcher
    // (the black shield) over the running app. Apps that redraw every frame
    // hide it; NetSurf, which only pushes changed frames, went black on the
    // first tap. Pause it again at once; `lvgl: resume_on_input: false` in
    // the YAML avoids even the one repainted frame.
    if (this->pax_task_handle_ != nullptr && !this->pax_loading_ && this->lvgl_ != nullptr &&
        this->lvgl_->is_loop_started() && !this->lvgl_->is_paused()) {
      this->lvgl_->set_paused(true, false);
      if (!this->lvgl_resume_warned_) {
        this->lvgl_resume_warned_ = true;
        ESP_LOGW(TAG, "LVGL resumed by touch/key input during a PAX and was paused again; "
                      "set 'resume_on_input: false' under lvgl: to stop it repainting over apps");
      }
    }
#endif
  }

#ifdef PAX_LOADER_USE_LVGL
  // The loader's black shield is a real LVGL object, but LVGL stays paused
  // while PAXs draw directly to the panel. Retire the message on the loop
  // task as soon as the new app has submitted its first frame; the PAX's
  // frame replaces the physical pixels beneath it without waking LVGL.
  if (this->loading_message_ != nullptr && this->launched_ && this->pax_task_handle_ != nullptr &&
      this->pax_frame_count_ > 0) {
    this->hide_loading_message_();
    ESP_LOGI(TAG, "PAX first frame received; hiding loading message");
  }
#endif

  if (this->launched_) {
    if (this->pax_loading_) {
      if (!this->pax_load_done_)
        return;

      if (this->pax_load_task_handle_ != nullptr) {
        vTaskDelete(this->pax_load_task_handle_);
        this->pax_load_task_handle_ = nullptr;
      }
      this->pax_loading_ = false;

      if (this->pax_load_result_ != ESP_OK || this->pax_load_handle_ == nullptr) {
        const bool data_failed = this->pax_load_data_failed_;
        const esp_err_t error = static_cast<esp_err_t>(this->pax_load_result_);
        ESP_LOGE(TAG, "Network PAX %s failed: %s (%s, 0x%x)", data_failed ? "data download" : "load",
                 http_error_hint(error), esp_err_to_name(error), this->pax_load_result_);
        // A data failure already set a specific status line; keep it on screen.
        if (!data_failed) {
          this->set_progress_(false, 0, 0, "App load: %.58s (%s)", http_error_hint(error), esp_err_to_name(error));
        }
        this->pax_load_handle_ = nullptr;
        this->launched_ = false;
        this->begin_report_(this->pax_load_source_);
        this->send_report_(data_failed ? "data_failed" : "load_failed", this->pax_load_result_,
                           this->pax_load_source_);
        this->continue_chain_();  // an app handed over to that could not load: back to its caller
        return;
      }

      this->set_progress_(false, 0, 0, "%s", "");
      psram_app_handle_t app = this->pax_load_handle_;
      this->pax_load_handle_ = nullptr;
      const std::string source = this->pax_load_source_;
      if (!this->start_loaded_app_(app, source)) {
        this->launched_ = false;
        this->continue_chain_();
      }
      return;
    }
    if (this->pax_task_done_) {
      this->finish_app_();
    } else if (this->global_close_requested_ && this->pax_task_handle_ != nullptr &&
               this->close_requested_us_ > 0 &&
               esp_timer_get_time() - this->close_requested_us_ >= PAX_CLOSE_GRACE_US) {
      // A few legacy PAXs do not consume the shared Menu/X/L3 close request.
      // Stop the entry worker, then finish_app_ stops every registered child
      // before unmapping the image and restoring audio, sockets and canvas.
      if (!begin_pax_quiesce()) return;
      ESP_LOGW(TAG, "PAX did not return after the close request; stopping its worker and restoring the dashboard");
      // finish_app_ closes SDK FILEs before deleting their Newlib owners.
      this->pax_task_result_ = -2;
      this->pax_task_done_ = true;
      this->finish_app_();
    } else if (!this->global_close_requested_ && this->pax_task_handle_ != nullptr &&
               this->pax_started_us_ > 0 && this->pax_frame_count_ == 0 &&
               esp_timer_get_time() - this->pax_started_us_ >=
                   (this->running_source_.find("redalert") != std::string::npos
                        ? REDALERT_NO_FRAME_TIMEOUT_US
                        : PAX_NO_FRAME_TIMEOUT_US)) {
      // A running emulator that never submits a frame is otherwise just a
      // black screen with no way to tell whether it initialized successfully.
      // Request the normal close sequence; the grace timeout above provides a
      // guaranteed return even if the app also ignores input.
      ESP_LOGW(TAG, "PAX has produced no video frame after %lld ms; returning to the dashboard",
               static_cast<long long>((this->running_source_.find("redalert") != std::string::npos
                                            ? REDALERT_NO_FRAME_TIMEOUT_US
                                            : PAX_NO_FRAME_TIMEOUT_US) / 1000));
      this->begin_close_();
    } else if (this->pax_task_handle_ != nullptr && esp_timer_get_time() >= this->next_heap_log_us_) {
      this->next_heap_log_us_ = esp_timer_get_time() + HEAP_LOG_INTERVAL_US;
      log_heap("while the app runs");
    }
    return;
  }

  if (!this->launch_pending_ || this->path_.empty() || this->web_storage_busy_)
    return;
  ESP_LOGI(TAG, "Processing launch request: %s", this->path_.c_str());
  this->launch_pending_ = false;
  this->set_progress_(false, 0, 0, "%s", "");  // drop the previous launch's status
  // app_get_arg: what an app hand-off passed along; nothing for a launch from
  // the menu, a button or remotely, which also forgets the hand-off chain.
  if (this->handoff_mutex_ != nullptr)
    xSemaphoreTake(this->handoff_mutex_, portMAX_DELAY);
  if (!this->chain_launch_) {
    this->chain_.reset();
    this->next_arg_.clear();
  }
  this->launch_arg_ = this->next_arg_;
  this->resume_arg_ = this->next_arg_;
  this->running_source_ = this->path_;
  if (this->handoff_mutex_ != nullptr)
    xSemaphoreGive(this->handoff_mutex_);
  this->next_arg_.clear();
  this->chain_launch_ = false;
  this->launched_ = true;
  this->control_overlay_offset_ = 0;
  this->control_overlay_rendered_offset_ = 0;
  // Keep the toolbar hidden until the user deliberately swipes it down.
  this->control_overlay_last_touch_us_ = 0;
  this->control_overlay_rendered_ = false;
  this->control_overlay_repaint_pending_ = false;
  this->toggle_close_requested_ = false;
  this->global_close_requested_ = false;
  this->clear_keyboard_queue_();
  this->clear_mouse_delta_();
  if (!this->launch_()) {
    this->launched_ = false;
    this->continue_chain_();
  }
}

void PaxLoader::dump_config() {
  ESP_LOGCONFIG(TAG, "PAX loader");
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  Autostart: %s", YESNO(this->autostart_));
  ESP_LOGCONFIG(TAG, "  Network PAXs: enabled with pax_loader.launch_url");
  ESP_LOGCONFIG(TAG, "  App data: %s into %s", this->download_data_ ? "download missing files" : "off",
                this->data_root_.c_str());
  for (const auto &root : this->data_search_)
    ESP_LOGCONFIG(TAG, "  App data also looked for in: %s", root.c_str());
  if (!this->catalog_url_.empty()) {
    ESP_LOGCONFIG(TAG, "  PAX catalog: %s", this->catalog_url_.c_str());
  }
  for (const auto &catalog : this->catalogs_)
    ESP_LOGCONFIG(TAG, "  PAX library source: %s -> %s", catalog.name.c_str(), catalog.url.c_str());
}

void PaxLoader::select_catalog_index(size_t index) {
  if (index >= this->catalogs_.size())
    return;
  this->catalog_index_ = index;
  this->catalog_url_ = this->catalogs_[index].url;
  ESP_LOGI(TAG, "PAX library: %s (%s)", this->catalogs_[index].name.c_str(), this->catalog_url_.c_str());
  this->catalog_entries_.clear();
  this->start_info_fetch_();
#ifdef PAX_LOADER_USE_LVGL
  this->catalog_ui_pending_ = true;
#endif
  this->refresh_catalog();
}

void PaxLoader::select_catalog(const std::string &name) {
  auto lower = [](std::string text) {
    for (char &c : text)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
  };
  for (size_t i = 0; i < this->catalogs_.size(); i++) {
    if (lower(this->catalogs_[i].name) == lower(name)) {
      this->select_catalog_index(i);
      return;
    }
  }
  ESP_LOGW(TAG, "No PAX catalog named %s", name.c_str());
}

void PaxLoader::next_catalog(int step) {
  const int count = static_cast<int>(this->catalogs_.size());
  if (count < 2)
    return;
#ifdef PAX_LOADER_USE_LVGL
  // Switching from the source/count bar rebuilds the list asynchronously.
  // Remember that focus was on the bar so the rebuild does not drop the user
  // onto the first app tile and require navigating back up to switch again.
  if (this->catalog_header_ != nullptr) {
    if (lv_group_t *group = lv_group_get_default())
      this->catalog_header_focus_pending_ = lv_group_get_focused(group) == this->catalog_header_;
  }
#endif
  const int next = ((static_cast<int>(this->catalog_index_) + step) % count + count) % count;
  this->select_catalog_index(static_cast<size_t>(next));
}

// A folder catalog (a URL starting with '/', such as /sd/roms/pax/): the
// .pax files in that folder under every data search root, so the same folder
// on the SD card and a USB stick are listed together.
std::vector<std::pair<std::string, std::string>> PaxLoader::catalog_folders_(const std::string &path) const {
  std::string folder = path;
  if (folder.empty() || folder.back() != '/')
    folder += '/';
  std::vector<std::pair<std::string, std::string>> folders;  // root label, folder
  for (const auto &root : this->data_search_) {
    if (folder.rfind(root + "/", 0) == 0) {
      const std::string rest = folder.substr(root.size());
      for (const auto &other : this->data_search_)
        folders.emplace_back(other.substr(1), other + rest);
      break;
    }
  }
  if (folders.empty())
    folders.emplace_back("", folder);
  return folders;
}

void PaxLoader::list_catalog_folder_() {
  std::string folder = this->catalog_url_;
  if (folder.back() != '/')
    folder += '/';
  const std::vector<std::pair<std::string, std::string>> folders = this->catalog_folders_(folder);

  std::vector<std::pair<std::string, std::string>> entries;
  for (size_t i = 0; i < folders.size(); i++) {
    DIR *dir = opendir(runtime_path(folders[i].second.c_str()).c_str());
    if (dir == nullptr)
      continue;
    while (dirent *entry = readdir(dir)) {
      std::string file = entry->d_name;
      std::string lower = file;
      for (char &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (lower.size() <= 4 || lower.compare(lower.size() - 4, 4, ".pax") != 0)
        continue;
      std::string label = file.substr(0, file.size() - 5);
      if (i > 0)
        label += " (" + folders[i].first + ")";  // not on the first root, e.g. "(usb0)"
      entries.emplace_back(label, folders[i].second + file);
    }
    closedir(dir);
  }
  std::sort(entries.begin(), entries.end());
  this->catalog_entries_ = std::move(entries);
  ESP_LOGI(TAG, "PAX folder catalog %s: %u application(s)", folder.c_str(),
           static_cast<unsigned>(this->catalog_entries_.size()));
  this->start_info_fetch_();
#ifdef PAX_LOADER_USE_LVGL
  this->catalog_ui_pending_ = true;
#endif
}

void PaxLoader::refresh_catalog() {
  if (this->catalog_url_.empty()) {
    ESP_LOGW(TAG, "Cannot refresh PAX catalog without catalog_url");
    return;
  }
  if (this->catalog_url_[0] == '/') {
    this->list_catalog_folder_();
    return;
  }
  if (this->catalog_loading_) {
    // The fetch that finishes checks whether it is still for the catalog being
    // shown, and fetches the current one if not.
    ESP_LOGD(TAG, "PAX catalog request already in progress");
    return;
  }
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Cannot refresh PAX catalog while the device is offline");
    return;
  }
  if (this->launched_) {
    // As with the listings (start_info_fetch_): no store TLS while an app
    // loads or runs. loop() refreshes once it has ended.
    this->catalog_deferred_ = true;
    ESP_LOGI(TAG, "PAX catalog refresh waits until the running app ends");
    return;
  }

  this->catalog_html_.clear();
  this->catalog_fetch_url_ = this->catalog_url_;
  this->catalog_done_ = false;
  this->catalog_result_ = -1;
  this->catalog_loading_ = true;
  const BaseType_t task_result = xTaskCreatePinnedToCoreWithCaps(
      &PaxLoader::pax_catalog_task_entry_, "pax_catalog", 16384, this, 4,
      &this->pax_catalog_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (task_result != pdPASS) {
    this->catalog_loading_ = false;
    this->pax_catalog_task_handle_ = nullptr;
    ESP_LOGE(TAG, "Could not create PAX catalog task");
    return;
  }
  ESP_LOGI(TAG, "PAX catalog request started: %s", this->catalog_url_.c_str());
}

void PaxLoader::show_archive_search() {
  this->play_ui_sound(UI_SOUND_CHIME);
  this->archive_mode_ = ARCHIVE_VIEW_SEARCH;
  this->archive_search_pending_ = false;
  this->archive_search_due_us_ = 0;
  this->archive_loading_ = false;
  if (this->archive_task_handle_ != nullptr) {
    vTaskDelete(this->archive_task_handle_);
    this->archive_task_handle_ = nullptr;
  }
#ifdef PAX_LOADER_USE_LVGL
  this->archive_ui_pending_ = true;
#endif
}

void PaxLoader::start_archive_search_() {
  this->archive_search_pending_ = false;
  this->archive_search_due_us_ = 0;
  if (this->archive_search_query_.size() < 2) {
    this->archive_search_results_.clear();
    this->archive_result_ = -1;
#ifdef PAX_LOADER_USE_LVGL
    this->archive_ui_pending_ = true;
#endif
    return;
  }
  if (this->archive_loading_) {
    this->archive_search_pending_ = true;
    return;
  }
  if (!network::is_connected()) {
    this->archive_result_ = ESP_ERR_INVALID_STATE;
#ifdef PAX_LOADER_USE_LVGL
    this->archive_ui_pending_ = true;
#endif
    ESP_LOGW(TAG, "Cannot search Archive.org while the device is offline");
    return;
  }
  if (this->launched_ || this->pax_loading_ || this->install_task_handle_ != nullptr || this->web_storage_busy_) {
    this->archive_search_pending_ = true;
    this->archive_search_due_us_ = esp_timer_get_time() + 1000000;
    return;
  }
  this->archive_search_term_ = archive_search_term(this->archive_search_query_, this->archive_search_collection_);
  const std::string slug = archive_search_slug(this->archive_search_term_);
  if (this->archive_search_term_.empty() || slug.empty()) {
    this->archive_result_ = ESP_ERR_INVALID_ARG;
#ifdef PAX_LOADER_USE_LVGL
    this->archive_ui_pending_ = true;
#endif
    return;
  }
  std::string phrase = this->archive_search_term_;
  for (char &character : phrase) {
    if (character == '"' || character == '\\')
      character = ' ';
  }
  this->archive_fetch_url_ =
      "https://archive.org/advancedsearch.php?q=" +
      archive_encode_query("collection:" + this->archive_search_collection_ + " AND (identifier:" + slug +
                           " OR title:\"" + phrase + "\" OR description:\"" + phrase + "\")") +
      "&fl%5B%5D=identifier&fl%5B%5D=title&rows=20&page=1&output=json";
  this->archive_html_.clear();
  this->archive_search_results_.clear();
  this->archive_done_ = false;
  this->archive_result_ = -1;
  this->archive_loading_ = true;
#ifdef PAX_LOADER_USE_LVGL
  this->archive_ui_pending_ = true;
#endif
  if (xTaskCreatePinnedToCoreWithCaps(&PaxLoader::archive_task_entry_, "archive_search", 16384, this, 4,
                                      &this->archive_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->archive_loading_ = false;
    this->archive_task_handle_ = nullptr;
    this->archive_result_ = ESP_ERR_NO_MEM;
    ESP_LOGE(TAG, "Could not create Archive.org search task");
    return;
  }
  ESP_LOGI(TAG, "Archive.org search started: %s", this->archive_search_query_.c_str());
}

void PaxLoader::show_archive_systems() {
  this->play_ui_sound(UI_SOUND_CHIME);
  this->archive_mode_ = ARCHIVE_VIEW_SYSTEMS;
  this->archive_search_pending_ = false;
  this->archive_search_due_us_ = 0;
  this->archive_loading_ = false;
  if (this->archive_task_handle_ != nullptr) {
    vTaskDelete(this->archive_task_handle_);
    this->archive_task_handle_ = nullptr;
  }
#ifdef PAX_LOADER_USE_LVGL
  this->archive_ui_pending_ = true;
#endif
}

void PaxLoader::open_archive_system(size_t system_index) {
  if (system_index >= sizeof(ARCHIVE_SYSTEMS) / sizeof(ARCHIVE_SYSTEMS[0]))
    return;
  const auto &system = ARCHIVE_SYSTEMS[system_index];
  this->archive_mode_ = ARCHIVE_VIEW_GAMES;
  this->archive_system_name_ = system.title;
  this->archive_system_dest_ = system.destination;
  this->archive_system_exts_ = system.extensions;

  std::string base_url = this->archive_collection_base_;
  if (base_url.empty() && !this->archive_sources_.empty()) {
    const std::string configured_base = archive_download_base_url(this->archive_sources_[0].url);
    if (!configured_base.empty())
      base_url = configured_base;
  }
  if (base_url.empty())
    base_url = "https://archive.org/download/ultimate-rom-collection/";
  std::string zip_url = base_url + system.zip_file;
  if (zip_url.back() != '/')
    zip_url += '/';
  this->archive_fetch_url_ = zip_url;

  if (this->archive_loading_) {
    ESP_LOGD(TAG, "Archive.org request already in progress");
    return;
  }
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Cannot refresh Archive.org source while the device is offline");
#ifdef PAX_LOADER_USE_LVGL
    this->archive_ui_pending_ = true;
#endif
    return;
  }
  if (this->launched_ || this->pax_loading_ || this->install_task_handle_ != nullptr || this->web_storage_busy_) {
    ESP_LOGW(TAG, "Archive.org refresh waits until the running storage operation ends");
    return;
  }

  this->archive_html_.clear();
  this->archive_entries_.clear();
  this->archive_done_ = false;
  this->archive_result_ = -1;
  this->archive_loading_ = true;
#ifdef PAX_LOADER_USE_LVGL
  this->archive_ui_pending_ = true;
#endif
  if (xTaskCreatePinnedToCoreWithCaps(&PaxLoader::archive_task_entry_, "archive_list", 16384, this, 4,
                                      &this->archive_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->archive_loading_ = false;
    this->archive_task_handle_ = nullptr;
    this->archive_result_ = ESP_ERR_NO_MEM;
    ESP_LOGE(TAG, "Could not create Archive.org list task");
    return;
  }
  ESP_LOGI(TAG, "Archive.org member list request started: %s (%s)", system.title, this->archive_fetch_url_.c_str());
}

void PaxLoader::select_archive_result(size_t result_index) {
  if (result_index >= this->archive_search_results_.size()) {
    // The first button on the search page is a shortcut to the saved default.
    if (result_index != SIZE_MAX || this->archive_collection_base_.empty())
      return;
    this->show_archive_systems();
    return;
  }
  const auto &result = this->archive_search_results_[result_index];
  if (result.identifier.empty())
    return;
  this->archive_collection_base_ = "https://archive.org/download/" + archive_encode_query(result.identifier) + "/";
  this->archive_collection_name_ = result.title.empty() ? result.identifier : result.title;
  this->archive_search_query_.clear();
  this->save_archive_collection_(this->archive_collection_base_, this->archive_collection_name_);
  this->show_archive_systems();
}

void PaxLoader::open_archive_for_current_rom_selector() {
  this->archive_return_to_rom_selector_ = true;
#ifdef PAX_LOADER_USE_LVGL
  if (this->archive_container_ != nullptr)
    lv_scr_load(lv_obj_get_screen(this->archive_container_));
#endif
  // Let the user search for a collection first. The saved default can still
  // be opened with one tap, and the ROM selector remains the return target.
  this->show_archive_search();
}

void PaxLoader::back_from_archive() {
  this->play_ui_sound(UI_SOUND_BACK);
#ifdef PAX_LOADER_USE_LVGL
  // Treat the Archive.org picker as a directory stack.  B first leaves the
  // current child view (games -> systems -> search); only B from the root
  // search view returns to the page that opened the picker.
  if (this->archive_mode_ == ARCHIVE_VIEW_GAMES) {
    this->show_archive_systems();
    return;
  }

  if (this->archive_mode_ == ARCHIVE_VIEW_SYSTEMS) {
    this->show_archive_search();
    return;
  }

  if (this->archive_return_to_rom_selector_) {
    this->archive_return_to_rom_selector_ = false;
    if (this->rom_selector_container_ != nullptr) {
      lv_scr_load(lv_obj_get_screen(this->rom_selector_container_));
      this->refresh_rom_selector();
    }
    return;
  }

  if (this->catalog_container_ != nullptr) {
    lv_scr_load(lv_obj_get_screen(this->catalog_container_));
  }
#endif
}

void PaxLoader::refresh_archive() {
  if (this->archive_mode_ == ARCHIVE_VIEW_SEARCH) {
    this->archive_search_pending_ = this->archive_search_query_.size() >= 2;
    this->archive_search_due_us_ = esp_timer_get_time();
#ifdef PAX_LOADER_USE_LVGL
    this->archive_ui_pending_ = true;
#endif
    return;
  }
  if (this->archive_mode_ == ARCHIVE_VIEW_SYSTEMS) {
    this->show_archive_systems();
    return;
  }
  if (this->archive_fetch_url_.empty()) {
    this->show_archive_systems();
    return;
  }
  if (this->archive_loading_) {
    ESP_LOGD(TAG, "Archive.org request already in progress");
    return;
  }
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Cannot refresh Archive.org source while the device is offline");
    return;
  }
  if (this->launched_ || this->pax_loading_ || this->install_task_handle_ != nullptr || this->web_storage_busy_) {
    ESP_LOGW(TAG, "Archive.org refresh waits until the running storage operation ends");
    return;
  }

  this->archive_html_.clear();
  this->archive_entries_.clear();
  this->archive_done_ = false;
  this->archive_result_ = -1;
  this->archive_loading_ = true;
#ifdef PAX_LOADER_USE_LVGL
  this->archive_ui_pending_ = true;
#endif
  if (xTaskCreatePinnedToCoreWithCaps(&PaxLoader::archive_task_entry_, "archive_list", 16384, this, 4,
                                      &this->archive_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->archive_loading_ = false;
    this->archive_task_handle_ = nullptr;
    this->archive_result_ = ESP_ERR_NO_MEM;
    ESP_LOGE(TAG, "Could not create Archive.org list task");
    return;
  }
  ESP_LOGI(TAG, "Archive.org member list request started: %s", this->archive_fetch_url_.c_str());
}

bool PaxLoader::launch_() {
  if (pax_firmware::busy()) { this->launched_ = false; return false; }
  const std::string source = this->path_;
#ifdef PAX_LOADER_USE_LVGL
  // Evict the launcher before reading/mapping the next PAX. This gives the
  // loader and its bootstrap task the reclaimed heap during file/URL loading,
  // not only after the executable has already consumed that memory.
  this->release_launcher_runtime_();
#endif
  if (is_network_url(source.c_str())) {
    if (!network::is_connected()) {
      ESP_LOGE(TAG, "Cannot load network PAX while the device is offline: %s", source.c_str());
      this->launched_ = false;
      return false;
    }

    // Download in its own task. HTTP/TLS setup and a several-megabyte PAX
    // transfer must not block the ESPHome loop, USB host, LVGL, or watchdog.
    this->pax_load_source_ = source;
    this->pax_load_handle_ = nullptr;
    this->pax_load_done_ = false;
    this->pax_load_result_ = -1;
    this->pax_loading_ = true;
    const BaseType_t task_result = xTaskCreatePinnedToCoreWithCaps(
        &PaxLoader::pax_load_task_entry_, "pax_http", 20480, this, 5,
        &this->pax_load_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_result != pdPASS) {
      ESP_LOGE(TAG, "Could not create network PAX loader task");
      this->pax_loading_ = false;
      this->launched_ = false;
      return false;
    }
    ESP_LOGI(TAG, "Network PAX download started: %s", source.c_str());
    return true;
  }

  const std::string path = runtime_path(source.c_str());
  ESP_LOGI(TAG, "Opening selected PAX: %s", path.c_str());
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "PAX file not found: %s", path.c_str());
    this->launched_ = false;
    return false;
  }

  psram_app_handle_t app = nullptr;
  esp_err_t err = psram_app_load(path.c_str(), &app);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "PAX load failed: %s (0x%x)", esp_err_to_name(err), err);
    this->launched_ = false;
    return false;
  }

  return this->start_loaded_app_(app, path);
}

bool PaxLoader::start_loaded_app_(psram_app_handle_t app, const std::string &source) {
  if (app == nullptr)
    return false;

  portENTER_CRITICAL(&pax_tasks_mux);
  pax_tasks_closing = false;
  pax_entry_task = nullptr;
  pax_entry_services = pax_entry_display_leases = 0;
  portEXIT_CRITICAL(&pax_tasks_mux);
  this->running_app_source_ = source;
  ESP_LOGI(TAG, "Starting PAX in worker task: %s", source.c_str());
  this->begin_report_(source);
  if (!this->prepare_canvas_for_app_(source)) {
    ESP_LOGE(TAG, "App start refused: cannot allocate legacy canvas");
    this->running_app_source_.clear();
    this->set_progress_(false, 0, 0, "%s", "App start failed: canvas memory unavailable");
    this->send_report_("start_failed", -1, source);
    psram_app_unload(app);
    return false;
  }
#ifdef PAX_LOADER_USE_USB_MIDI
  // The app gets the notes played from now on, not what piled up before.
  if (this->usb_midi_ != nullptr) {
    uint8_t stale[64];
    while (this->usb_midi_->read(stale, sizeof(stale)) > 0) {
    }
  }
#endif

#ifdef PAX_LOADER_USE_LVGL
  // LVGL must not draw over the PAX framebuffer. Pause it from the ESPHome
  // loop task; the PAX worker never calls LVGL directly.
  if (this->lvgl_ != nullptr && this->lvgl_->is_loop_started()) {
    // Direct-rendered PAXs share the launcher LVGL display but have no LVGL
    // object of their own. Put a black clickable object over the whole
    // launcher, top layer included (store detail page, side menu): a release
    // cannot reach a button underneath, and the panel around the canvas is
    // black instead of a frozen store or launcher page.
    if (this->touch_modal_shield_ == nullptr) {
      lv_obj_t *top = lv_layer_top();
      if (top != nullptr) {
        this->touch_modal_shield_ = lv_obj_create(top);
        lv_obj_remove_style_all(this->touch_modal_shield_);
        lv_obj_set_size(this->touch_modal_shield_, LV_PCT(100), LV_PCT(100));
        lv_obj_center(this->touch_modal_shield_);
        lv_obj_set_style_bg_color(this->touch_modal_shield_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(this->touch_modal_shield_, LV_OPA_COVER, 0);
        lv_obj_move_foreground(this->touch_modal_shield_);
        lv_obj_add_flag(this->touch_modal_shield_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            this->touch_modal_shield_,
            [](lv_event_t *event) { lv_event_stop_bubbling(event); }, LV_EVENT_ALL, nullptr);
      }
    }
    // Discard any launcher touch that was held during the transition. The
    // PAX owns the touch surface while it is running; a stale LVGL press
    // must not be replayed against a launcher button after it closes.
    lv_indev_reset(nullptr, nullptr);
    // PAXs may call lv_timer_handler() on the shared LVGL instance. Disable
    // the launcher input devices explicitly; pausing ESPHome's LVGL loop
    // alone does not stop those devices from being processed by the PAX.
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
      lv_indev_enable(indev, false);
    }
    // The PAX canvas does not cover the whole panel: whatever LVGL last drew
    // around it stays visible while the app runs. Hide the finished download
    // bar and redraw once, so the panel shows only the black shield.
    this->set_progress_(false, 0, 0, "%s", "");
    this->update_progress_ui_();
    if (source.find("launcher_ui") == std::string::npos)
      this->show_loading_message_();
    lv_refr_now(nullptr);
    this->lvgl_->set_paused(true, false);
  }
#endif

  this->app_handle_ = app;
  this->pax_task_done_ = false;
  this->pax_task_result_ = -1;
  this->pax_frame_count_ = 0;
  this->pax_started_us_ = esp_timer_get_time();
  s_app_heap_internal = 0;
  s_app_heap_failed = 0;
  this->audio_trouble_logged_ = false;
  this->next_heap_log_us_ = esp_timer_get_time() + HEAP_LOG_FIRST_US;
  log_heap("before the app starts", true);
  // The worker performs esp_mmu_map()/esp_mmu_unmap() around the PAX entry
  // point. Those routines briefly disable the caches, so the current task's
  // stack must be in internal RAM; IDF asserts otherwise. Keep this bootstrap
  // stack deliberately small because LVGL, SD, I2S and USB can fragment the
  // largest internal block below 16 KiB even while PSRAM is plentiful. The
  // PAX's large buffers and app-created tasks still use PSRAM through the
  // service API.
  constexpr uint32_t PAX_BOOTSTRAP_STACK = 8192;
  const BaseType_t task_result = xTaskCreatePinnedToCore(
      &PaxLoader::pax_task_entry_, "pax_main", PAX_BOOTSTRAP_STACK, this, 5,
      &this->pax_task_handle_, 0);
  this->pax_task_uses_caps_ = false;
  if (task_result != pdPASS) {
    ESP_LOGE(TAG, "Could not create PAX worker task (internal free=%u B, largest=%u B)",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    this->app_handle_ = nullptr;
    this->pax_task_handle_ = nullptr;
    this->pax_task_uses_caps_ = false;
    this->launched_ = false;
    this->running_app_source_.clear();
    this->set_progress_(false, 0, 0, "%s", "App start failed: not enough internal memory");
    this->send_report_("start_failed", -1, source);
    psram_app_unload(app);
    // start_loaded_app_ has already paused LVGL, installed the black touch
    // shield, and prepared the app canvas. Use the same complete unwind as a
    // normal PAX exit so a failed task allocation cannot strand the user on
    // a black screen.
    this->restore_legacy_canvas_();
    this->restore_lvgl_();
    return false;
  }
  portENTER_CRITICAL(&pax_tasks_mux);
  pax_entry_task = this->pax_task_handle_;
  portEXIT_CRITICAL(&pax_tasks_mux);
  ESP_LOGI(TAG, "PAX worker task started on core 0; ESPHome loop remains active");
  return true;
}

void PaxLoader::enqueue_keyboard_event_(int key, bool down) {
  if (!this->launched_ || key <= 0 || this->keyboard_mutex_ == nullptr)
    return;

  // USB HIDX and the PAX worker can arrive here on different cores.  The
  // critical section is tiny; wait briefly instead of silently dropping a
  // character when the reader currently owns the mutex.
  if (xSemaphoreTake(this->keyboard_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
    ESP_LOGW(TAG, "USB keyboard queue lock timeout; dropped key=%d", key);
    return;
  }
  const size_t next = (this->keyboard_head_ + 1) % KEYBOARD_QUEUE_SIZE;
  if (next == this->keyboard_tail_) {
    // Drop the oldest event rather than blocking the ESPHome loop task.
    this->keyboard_tail_ = (this->keyboard_tail_ + 1) % KEYBOARD_QUEUE_SIZE;
    ESP_LOGW(TAG, "USB keyboard event queue full; dropped oldest event");
  }
  this->keyboard_queue_[this->keyboard_head_] = {key, down ? 1 : 0};
  this->keyboard_head_ = next;
  xSemaphoreGive(this->keyboard_mutex_);
  ESP_LOGD(TAG, "Queued PAX keyboard key=%d %s", key, down ? "down" : "up");
}

void PaxLoader::enqueue_keyboard_tap_(int key) {
  // The USB text sensor reports characters, not a complete HID key lifecycle.
  // A tap gives keyboard-oriented apps a complete press/release pair.
  this->enqueue_keyboard_event_(key, true);
  this->enqueue_keyboard_event_(key, false);
}

void PaxLoader::enqueue_keyboard_text(const std::string &text) {
  if (text.empty())
    return;

  // When the launcher is visible, arrows remain menu navigation. Once a PAX
  // is running, they are keyboard taps like every other keyboard key; they do
  // not become synthetic gamepad input.
  static const char *const arrows[4] = {"Up", "Right", "Down", "Left"};  // PAX_INPUT_UP..LEFT order
  static const int keyboard_keys[4] = {PAX_KEY_UP, PAX_KEY_RIGHT, PAX_KEY_DOWN, PAX_KEY_LEFT};
  for (int i = 0; i < 4; i++) {
    if (text != arrows[i])
      continue;
    if (this->launched_)
      this->enqueue_keyboard_tap_(keyboard_keys[i]);
    else
      this->arrow_tap_until_us_[i] = esp_timer_get_time() + ARROW_TAP_US;
    return;
  }

  int key = 0;
  if (text == "Backspace")
    key = 127;
  else if (text == "Tab")
    key = 9;
  else if (text == "Enter")
    key = 13;
  else if (text == "Escape")
    key = 27;
  else if (text == "Left Alt" || text == "Right Alt")
    key = 132;
  else if (text == "Left Ctrl" || text == "Right Ctrl")
    key = 133;
  else if (text == "Left Shift" || text == "Right Shift")
    key = 134;
  else if (text == "F1")
    key = 135;
  else if (text == "F2")
    key = 136;
  else if (text == "F3")
    key = 137;
  else if (text == "F4")
    key = 138;
  else if (text == "F5")
    key = 139;
  else if (text == "F6")
    key = 140;
  else if (text == "F7")
    key = 141;
  else if (text == "F8")
    key = 142;
  else if (text == "F9")
    key = 143;
  else if (text == "F10")
    key = 144;
  else if (text == "F11")
    key = 145;
  else if (text == "F12")
    key = 146;
  else if (text == "Insert")
    key = 147;
  else if (text == "Delete")
    key = 148;
  else if (text == "Page Down")
    key = 149;
  else if (text == "Page Up")
    key = 150;
  else if (text == "Home")
    key = 151;
  else if (text == "End")
    key = 152;
  else if (text.size() == 1)
    key = static_cast<unsigned char>(text[0]);

  if (key != 0)
    this->enqueue_keyboard_tap_(key);
}

void PaxLoader::enqueue_mouse_delta(float dx, float dy) {
  if (!std::isfinite(dx) || !std::isfinite(dy))
    return;

  // HID mouse reports are int8 deltas, but keep the public callback float so
  // the same path also works for touchpads and scaled HIDX reports.
  const int32_t x = static_cast<int32_t>(std::lround(std::max(-32768.0f, std::min(32767.0f, dx))));
  const int32_t y = static_cast<int32_t>(std::lround(std::max(-32768.0f, std::min(32767.0f, dy))));
  if (x == 0 && y == 0)
    return;

  portENTER_CRITICAL(&this->mouse_input_lock_);
  const int64_t next_x = static_cast<int64_t>(this->mouse_dx_accum_) + x;
  const int64_t next_y = static_cast<int64_t>(this->mouse_dy_accum_) + y;
  this->mouse_dx_accum_ = static_cast<int32_t>(std::max<int64_t>(-32768, std::min<int64_t>(32767, next_x)));
  this->mouse_dy_accum_ = static_cast<int32_t>(std::max<int64_t>(-32768, std::min<int64_t>(32767, next_y)));
  portEXIT_CRITICAL(&this->mouse_input_lock_);
}

void PaxLoader::enqueue_mouse_wheel(float wheel) {
  if (!std::isfinite(wheel))
    return;
  const int32_t amount = static_cast<int32_t>(std::lround(std::max(-32768.0f, std::min(32767.0f, wheel))));
  if (amount == 0)
    return;
  portENTER_CRITICAL(&this->mouse_input_lock_);
  const int64_t next = static_cast<int64_t>(this->mouse_wheel_accum_) + amount;
  this->mouse_wheel_accum_ = static_cast<int32_t>(std::max<int64_t>(-32768, std::min<int64_t>(32767, next)));
  portEXIT_CRITICAL(&this->mouse_input_lock_);
}

void PaxLoader::clear_keyboard_queue_() {
  if (this->keyboard_mutex_ == nullptr)
    return;
  if (xSemaphoreTake(this->keyboard_mutex_, portMAX_DELAY) == pdTRUE) {
    this->keyboard_head_ = 0;
    this->keyboard_tail_ = 0;
    xSemaphoreGive(this->keyboard_mutex_);
  }
}

void PaxLoader::clear_mouse_delta_() {
  portENTER_CRITICAL(&this->mouse_input_lock_);
  this->mouse_dx_accum_ = 0;
  this->mouse_dy_accum_ = 0;
  this->mouse_wheel_accum_ = 0;
  portEXIT_CRITICAL(&this->mouse_input_lock_);
}

int PaxLoader::read_mouse_scroll_(int *wheel) {
  this->wait_for_game_menu_();
  if (this->game_menu_quarantine_) {
    if (wheel != nullptr)
      *wheel = 0;
    return 0;
  }
  int32_t amount = 0;
  portENTER_CRITICAL(&this->mouse_input_lock_);
  amount = this->mouse_wheel_accum_;
  this->mouse_wheel_accum_ = 0;
  portEXIT_CRITICAL(&this->mouse_input_lock_);
  if (wheel != nullptr)
    *wheel = static_cast<int>(amount);
  return amount != 0 ? 1 : 0;
}

int PaxLoader::read_mouse_(int *dx, int *dy, int *buttons) {
  this->wait_for_game_menu_();
  if (this->game_menu_quarantine_) {
    if (dx != nullptr) *dx = 0;
    if (dy != nullptr) *dy = 0;
    if (buttons != nullptr) *buttons = 0;
    return 0;
  }
  int32_t x = 0;
  int32_t y = 0;
  portENTER_CRITICAL(&this->mouse_input_lock_);
  x = this->mouse_dx_accum_;
  y = this->mouse_dy_accum_;
  this->mouse_dx_accum_ = 0;
  this->mouse_dy_accum_ = 0;
  portEXIT_CRITICAL(&this->mouse_input_lock_);

  // USB HIDX reports movement in physical panel directions. PAXs draw in
  // the unrotated source canvas, while this loader presents the framebuffer
  // rotated 180 degrees. Normalize once at the service boundary so ScummVM
  // and every other PAX sees the same visible-screen direction as the native
  // RetroESP32-P4 launcher.
  int canvas_x = static_cast<int>(x);
  int canvas_y = static_cast<int>(y);
  pax_mouse_delta_to_canvas(&canvas_x, &canvas_y);

  if (dx != nullptr)
    *dx = canvas_x;
  if (dy != nullptr)
    *dy = canvas_y;

  int state = 0;
#ifdef PAX_LOADER_USE_USB_HIDX
  if (this->usb_hidx_ != nullptr) {
    if (this->usb_hidx_->get_mouse_left_sensor() != nullptr &&
        this->usb_hidx_->get_mouse_left_sensor()->get_state_default(false))
      state |= 0x01;
    if (this->usb_hidx_->get_mouse_right_sensor() != nullptr &&
        this->usb_hidx_->get_mouse_right_sensor()->get_state_default(false))
      state |= 0x02;
    if (this->usb_hidx_->get_mouse_middle_sensor() != nullptr &&
        this->usb_hidx_->get_mouse_middle_sensor()->get_state_default(false))
      state |= 0x04;
  }
#endif
  // In Red Alert, let a gamepad operate the same pointer as the analog sticks:
  // A selects with left-click and B is the right mouse button/context action.
  // Keep these mapped only for the RTS so other emulators retain their usual
  // face-button semantics.
  if (this->running_source_.find("redalert") != std::string::npos) {
    if (this->buttons_[PAX_INPUT_A] != nullptr && this->buttons_[PAX_INPUT_A]->get_state_default(false))
      state |= 0x01;
    if (this->buttons_[PAX_INPUT_B] != nullptr && this->buttons_[PAX_INPUT_B]->get_state_default(false))
      state |= 0x02;
  }
  if (buttons != nullptr)
    *buttons = state;

  // Return a sample every frame while USB HIDX is configured, even when the
  // delta is zero, so mouse-button releases are visible to Quake.
#ifdef PAX_LOADER_USE_USB_HIDX
  return this->usb_hidx_ != nullptr ? 1 : 0;
#else
  return 0;
#endif
}

int PaxLoader::read_keyboard_(pax_keyboard_event_t *event) {
  this->wait_for_game_menu_();
  if (this->game_menu_quarantine_)
    return 0;
  if (event == nullptr || this->keyboard_mutex_ == nullptr)
    return 0;
  if (xSemaphoreTake(this->keyboard_mutex_, 0) != pdTRUE)
    return 0;
  if (this->keyboard_head_ == this->keyboard_tail_) {
    xSemaphoreGive(this->keyboard_mutex_);
    return 0;
  }
  *event = this->keyboard_queue_[this->keyboard_tail_];
  this->keyboard_tail_ = (this->keyboard_tail_ + 1) % KEYBOARD_QUEUE_SIZE;
  xSemaphoreGive(this->keyboard_mutex_);
  return 1;
}

void PaxLoader::pax_task_entry_(void *arg) {
  portENTER_CRITICAL(&pax_tasks_mux);
  pax_entry_task = xTaskGetCurrentTaskHandle();
  portEXIT_CRITICAL(&pax_tasks_mux);
  auto *self = static_cast<PaxLoader *>(arg);
  self->pax_task_result_ = psram_app_run(self->app_handle_);
  self->pax_task_done_ = true;
  // Cleanup is owned by ESPHome's loop task. Do not return from a FreeRTOS
  // task, because the port's task-exit path treats that as an abort.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void PaxLoader::pax_load_task_entry_(void *arg) {
  auto *self = static_cast<PaxLoader *>(arg);
  const std::string url = self->pax_load_source_;
  // Fetch the app's data files first, so it never starts without them.
  const esp_err_t data_result = self->sync_app_data_(url);
  self->pax_load_data_failed_ = data_result != ESP_OK;
  if (data_result != ESP_OK) {
    self->pax_load_result_ = data_result;
  } else {
    self->pax_load_result_ = psram_app_load_url(url.c_str(), &self->pax_load_handle_);
  }
  self->pax_load_done_ = true;
  // Cleanup is owned by ESPHome's loop task. Keeping this task alive until
  // the loop deletes it avoids the ESP-IDF task-exit abort path used by the
  // PAX worker as well.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void PaxLoader::pax_catalog_task_entry_(void *arg) {
  auto *self = static_cast<PaxLoader *>(arg);
  const std::string url = self->catalog_fetch_url_;
  self->catalog_result_ = fetch_http_text(url.c_str(), &self->catalog_html_);
  self->catalog_done_ = true;
  // As with the PAX loader task, the ESPHome loop deletes this task after it
  // has consumed the result. Returning from an IDF task is not safe on this
  // target and can trigger the abort path.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void PaxLoader::archive_task_entry_(void *arg) {
  auto *self = static_cast<PaxLoader *>(arg);
  // A large ROM ZIP can have thousands of indexed members. Keep the listing
  // bounded, but allow enough room for a normal NES/SNES contents page.
  self->archive_result_ = fetch_http_text(self->archive_fetch_url_.c_str(), &self->archive_html_, 2 * 1024 * 1024);
  self->archive_done_ = true;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

#ifdef PAX_LOADER_USE_LVGL
void PaxLoader::restore_catalog_header_focus_() {
  if (!this->catalog_header_focus_pending_ || this->catalog_header_ == nullptr)
    return;
  this->catalog_header_focus_pending_ = false;
  lv_obj_add_state(this->catalog_header_, LV_STATE_FOCUSED);
  lv_group_focus_obj(this->catalog_header_);
  this->log_focus_("catalog header retained");
}

static void add_archive_focus_target_(lv_obj_t *object) {
  if (object == nullptr)
    return;
  if (lv_group_t *group = lv_group_get_default())
    lv_group_add_obj(group, object);
}

void PaxLoader::set_catalog_selection_(uint16_t index) {
  if (this->catalog_container_ == nullptr)
    return;
  if (this->store_ui_) {
    // Store view: the tiles sit in one grid object under the switcher row.
    if (this->catalog_tiles_.empty()) {
      this->catalog_selection_ = 0;
      return;
    }
    index = std::min<uint16_t>(index, static_cast<uint16_t>(this->catalog_tiles_.size() - 1));
    this->catalog_selection_ = index;
    for (lv_obj_t *tile : this->catalog_tiles_)
      lv_obj_remove_state(tile, LV_STATE_FOCUSED);
    lv_obj_add_state(this->catalog_tiles_[index], LV_STATE_FOCUSED);
    lv_group_focus_obj(this->catalog_tiles_[index]);
    lv_obj_scroll_to_view_recursive(this->catalog_tiles_[index], LV_ANIM_ON);
    return;
  }

  // `index` counts apps; the switcher row, when present, comes first.
  const uint32_t children = lv_obj_get_child_count(this->catalog_container_);
  const uint32_t count = children > this->catalog_header_rows_ ? children - this->catalog_header_rows_ : 0;
  if (count == 0) {
    this->catalog_selection_ = 0;
    return;
  }
  if (index >= count)
    index = static_cast<uint16_t>(count - 1);
  this->catalog_selection_ = index;

  for (uint32_t i = 0; i < children; i++) {
    lv_obj_t *child = lv_obj_get_child(this->catalog_container_, static_cast<int32_t>(i));
    if (child != nullptr)
      lv_obj_remove_state(child, LV_STATE_FOCUSED);
  }
  lv_obj_t *selected =
      lv_obj_get_child(this->catalog_container_, static_cast<int32_t>(index + this->catalog_header_rows_));
  if (selected != nullptr) {
    lv_obj_add_state(selected, LV_STATE_FOCUSED);
    lv_group_focus_obj(selected);
    lv_obj_scroll_to_view(selected, LV_ANIM_OFF);
  }
}

void PaxLoader::handle_launcher_controls_() {
  // The launcher sees exactly what an app would: gamepad buttons, the ADC
  // ladder, sticks, the fire/touch buttons (as A), held keyboard keys
  // (arrows/WASD, Space/Enter/Z, X, Backspace...) and arrow taps. Detect edges
  // here instead of treating a held HID report or ADC value as repeated
  // presses every ESPHome loop iteration.
  pax_gamepad_state_t input;
  // USB HID navigation is fed to LVGL's keypad indevs when configured. Keep
  // only the physical controls here so an A/D-pad report cannot be processed
  // once by LVGL and a second time by this compatibility handler.
  this->read_input_(&input, !this->lvgl_keypad_navigation_);
  uint8_t direction = 0;
  for (uint8_t i = 0; i < 4; i++) {  // PAX_INPUT_UP, RIGHT, DOWN, LEFT
    if (input.values[PAX_INPUT_UP + i])
      direction |= 1U << i;
  }
  const bool a = input.values[PAX_INPUT_A] != 0;
  // Menu (the keyboard's Escape) also goes back.
  const bool b = input.values[PAX_INPUT_B] != 0 || input.values[PAX_INPUT_MENU] != 0;
  const bool l = input.values[PAX_INPUT_L] != 0;
  const bool r = input.values[PAX_INPUT_R] != 0;
  const bool select = input.values[PAX_INPUT_SELECT] != 0;
  const bool touch = false;  // read_input_ already counts the touch button as A
  const bool active = direction != 0 || a || b || l || r || select || touch;
  auto remember = [&]() {
    this->launcher_direction_state_ = direction;
    this->launcher_a_state_ = a;
    this->launcher_b_state_ = b;
    this->launcher_l_state_ = l;
    this->launcher_r_state_ = r;
    this->launcher_select_state_ = select;
    this->launcher_touch_state_ = touch;
  };

  // Do not let a controller that is held during cold boot select or scroll a
  // PAX before the user has released it once. This is the same protection
  // used for the normal launch button and avoids the old phantom-input loop.
  if (!this->launcher_input_armed_) {
    if (!active) {
      ESP_LOGD(TAG, "Launcher controls armed after input release");
    }
    this->launcher_input_armed_ = !active;
    remember();
    return;
  }

  uint8_t newly_pressed = direction & static_cast<uint8_t>(~this->launcher_direction_state_);
  // The ROM picker is often a long alphabetical list. A held trigger gets a
  // short acceleration ramp there, while ordinary launcher navigation keeps
  // its edge-triggered behavior so a held d-pad cannot run through buttons.
  if (this->rom_selector_page_active_()) {
    const uint32_t now = millis();
    if (direction == 0) {
      this->launcher_repeat_direction_ = 0;
      this->launcher_repeat_next_ms_ = 0;
    } else if (newly_pressed != 0) {
      this->launcher_repeat_direction_ = newly_pressed;
      this->launcher_repeat_next_ms_ = now + 280;
    } else if (direction == this->launcher_repeat_direction_ &&
               this->launcher_repeat_next_ms_ != 0 &&
               static_cast<int32_t>(now - this->launcher_repeat_next_ms_) >= 0) {
      newly_pressed = direction;
      this->launcher_repeat_next_ms_ = now + 65;
    } else if (direction != this->launcher_repeat_direction_) {
      this->launcher_repeat_direction_ = direction;
      this->launcher_repeat_next_ms_ = now + 280;
    }
  } else {
    this->launcher_repeat_direction_ = 0;
    this->launcher_repeat_next_ms_ = 0;
  }
#ifdef PAX_LOADER_USE_LVGL
  // A visible search keyboard owns controller navigation while it is open.
  // Handle it before the page-specific routes below; otherwise Archive (and
  // the non-grid library) interprets D-pad presses as page navigation, moves
  // focus off the keyboard, and its on_defocus handler hides it.
  lv_group_t *keyboard_group = lv_group_get_default();
  lv_obj_t *keyboard_focused = keyboard_group != nullptr ? lv_group_get_focused(keyboard_group) : nullptr;
  if (keyboard_focused != nullptr && lv_obj_check_type(keyboard_focused, &lv_keyboard_class) &&
      launcher_keyboard_visible(keyboard_focused)) {
    uint32_t key = 0;
    if (newly_pressed & (1U << 0))
      key = LV_KEY_UP;
    else if (newly_pressed & (1U << 1))
      key = LV_KEY_RIGHT;
    else if (newly_pressed & (1U << 2))
      key = LV_KEY_DOWN;
    else if (newly_pressed & (1U << 3))
      key = LV_KEY_LEFT;
    if (key != 0) {
      lv_group_send_data(keyboard_group, key);
      this->play_ui_sound(UI_SOUND_CLICK);
    }
    if (a && !this->launcher_a_state_)
      lv_group_send_data(keyboard_group, LV_KEY_ENTER);
    if (b && !this->launcher_b_state_) {
      lv_obj_t *textarea = lv_keyboard_get_textarea(keyboard_focused);
      lv_obj_send_event(keyboard_focused, LV_EVENT_CANCEL, nullptr);
      if (textarea != nullptr)
        lv_group_focus_obj(textarea);
      this->play_ui_sound(UI_SOUND_BACK);
    }
    this->log_focus_("keyboard input");
    remember();
    return;
  }

  // The recovery/quick launcher has a focus group but no catalog container.
  // Its three buttons still need D-pad/left-stick navigation and A/select.
  if (this->catalog_container_ == nullptr) {
    lv_group_t *group = lv_group_get_default();
    if (group != nullptr) {
      if (newly_pressed & ((1U << 0) | (1U << 3)))
        lv_group_focus_prev(group);
      else if (newly_pressed & ((1U << 1) | (1U << 2)))
        lv_group_focus_next(group);
      lv_obj_t *focused = lv_group_get_focused(group);
      if (newly_pressed && focused != nullptr) {
        lv_obj_t *label = lv_obj_get_child(focused, 0);
        ESP_LOGI(TAG, "Quick launcher focus: %s", label != nullptr && lv_obj_check_type(label, &lv_label_class)
                                                    ? lv_label_get_text(label) : "control");
        this->play_ui_sound(UI_SOUND_CLICK);
      }
      if (a && !this->launcher_a_state_ && focused != nullptr) {
        ESP_LOGI(TAG, "Quick launcher controller activate");
        lv_obj_send_event(focused, LV_EVENT_CLICKED, nullptr);
      }
    }
    remember();
    return;
  }

  // Archive.org is a separate LVGL page, so the store controller handler does
  // not see it. Handle its controller focus and B/back action here; otherwise
  // the touch Back button works while a physical controller appears dead.
  const bool archive_page_active = this->archive_container_ != nullptr &&
                                   lv_scr_act() == lv_obj_get_screen(this->archive_container_);
  if (archive_page_active) {
    if (b && !this->launcher_b_state_) {
      this->back_from_archive();
      remember();
      return;
    }
    if (newly_pressed != 0) {
      const uint8_t spatial_direction = (newly_pressed & (1U << 0)) != 0
                                            ? 0
                                            : (newly_pressed & (1U << 1)) != 0 ? 1
                                            : (newly_pressed & (1U << 2)) != 0 ? 2 : 3;
      if (this->focus_store_spatial_(spatial_direction))
        this->play_ui_sound(UI_SOUND_CLICK);
    }
    if (a && !this->launcher_a_state_)
      this->activate_store_focus_();
    remember();
    return;
  }
#endif
  if (this->rom_selector_page_active_()) {
    // The ROM selector is a separate LVGL screen, so it does not go through
    // handle_store_controls_(). Keep its top toolbar navigation consistent
    // with the library/detail pages: Left/Right moves between the visible
    // controls instead of being swallowed by the ROM-list handler.
    lv_group_t *rom_group = lv_group_get_default();
    lv_obj_t *rom_focused = rom_group != nullptr ? lv_group_get_focused(rom_group) : nullptr;
    const bool compact_volume = rom_focused == this->catalog_volume_slider_;
    const bool compact_brightness = rom_focused == this->catalog_brightness_slider_;
    if (compact_volume || compact_brightness) {
      if (b && !this->launcher_b_state_) {
        if (compact_volume)
          this->toggle_catalog_volume_slider();
        else
          this->toggle_catalog_brightness_slider();
        this->play_ui_sound(UI_SOUND_BACK);
        this->log_focus_("compact slider closed");
        remember();
        return;
      }
      if ((newly_pressed & ((1U << 1) | (1U << 3))) != 0) {
        lv_obj_t *slider = compact_volume ? this->catalog_volume_slider_ : this->catalog_brightness_slider_;
        const int32_t min_value = compact_volume ? 0 : 5;
        const int32_t current_value = lv_slider_get_value(slider);
        const bool right = (newly_pressed & (1U << 1)) != 0;
        const int32_t next_value = std::max(min_value, std::min<int32_t>(100, current_value + (right ? 1 : -1)));
        lv_slider_set_value(slider, next_value, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
        this->log_focus_("compact slider adjust");
        remember();
        return;
      }
    }

    if (b && !this->launcher_b_state_) {
      this->back_from_rom_selector();
      remember();
      return;
    }

    const uint8_t horizontal = newly_pressed & ((1U << 1) | (1U << 3));
    const uint8_t vertical = newly_pressed & ((1U << 0) | (1U << 2));
    if (horizontal != 0 && vertical == 0) {
      const uint8_t toolbar_direction = (horizontal & (1U << 1)) != 0 ? 1 : 3;
      if (this->focus_store_spatial_(toolbar_direction))
        this->play_ui_sound(UI_SOUND_CLICK);
    }

    if (!this->rom_selector_buttons_.empty() && (newly_pressed & ((1U << 0) | (1U << 2))) != 0) {
      const uint16_t count = static_cast<uint16_t>(this->rom_selector_buttons_.size());
      uint16_t next = this->rom_selector_selection_;
      const bool up = (newly_pressed & (1U << 0)) != 0;
      const bool at_edge = up ? next == 0 : next + 1 >= count;
      if (!at_edge) {
        next = up ? static_cast<uint16_t>(next - 1) : static_cast<uint16_t>(next + 1);
        this->set_rom_selector_selection_(next);
        ESP_LOGI(TAG, "NES ROM selection: %u/%u", static_cast<unsigned>(next + 1), static_cast<unsigned>(count));
      } else {
        const uint8_t toolbar_direction = up ? 0 : 2;
        if (this->focus_store_spatial_(toolbar_direction))
          this->play_ui_sound(UI_SOUND_CLICK);
      }
    }
    const bool rom_select_pressed = (a && !this->launcher_a_state_) || (touch && !this->launcher_touch_state_);
    if (rom_select_pressed) {
      lv_group_t *group = lv_group_get_default();
      lv_obj_t *focused = group != nullptr ? lv_group_get_focused(group) : nullptr;
      const bool focused_rom = !this->rom_selector_buttons_.empty() &&
                               this->rom_selector_selection_ < this->rom_selector_buttons_.size() &&
                               focused == this->rom_selector_buttons_[this->rom_selector_selection_];
      if (focused_rom) {
        ESP_LOGI(TAG, "NES ROM physical select: %u/%u", static_cast<unsigned>(this->rom_selector_selection_ + 1),
                 static_cast<unsigned>(this->rom_selector_buttons_.size()));
        lv_obj_send_event(focused, LV_EVENT_CLICKED, nullptr);
      } else {
        this->activate_store_focus_();
      }
    }
    remember();
    return;
  }
  if (this->store_ui_) {
    // Grid and detail page: d-pad moves, A opens / presses, B goes back, L/R switch
    // sources, Select opens the side menu.
    this->handle_store_controls_(newly_pressed, (a && !this->launcher_a_state_) || (touch && !this->launcher_touch_state_),
                                 b && !this->launcher_b_state_, l && !this->launcher_l_state_,
                                 r && !this->launcher_r_state_, select && !this->launcher_select_state_);
    remember();
    return;
  }
  // Left/right switch catalogs (up/down still move through the list).
  if (this->catalogs_.size() > 1 && (newly_pressed & ((1U << 1) | (1U << 3))) != 0 &&
      (newly_pressed & ((1U << 0) | (1U << 2))) == 0) {
    this->next_catalog((newly_pressed & (1U << 1)) != 0 ? 1 : -1);
  } else if (!this->catalog_entries_.empty() && newly_pressed != 0) {
    const uint32_t count = this->catalog_entries_.size();
    uint16_t next = this->catalog_selection_;
    if ((newly_pressed & (1U << 0)) != 0) {
      if (next == 0) {
        if (this->focus_store_spatial_(0))
          this->play_ui_sound(UI_SOUND_CLICK);
      } else {
        next = static_cast<uint16_t>(next - 1);
        this->set_catalog_selection_(next);
        ESP_LOGI(TAG, "PAX catalog selection: %u/%u (up)", static_cast<unsigned>(next + 1),
                 static_cast<unsigned>(count));
      }
    } else if ((newly_pressed & (1U << 2)) != 0) {
      if (next + 1 >= count) {
        if (this->focus_store_spatial_(2))
          this->play_ui_sound(UI_SOUND_CLICK);
      } else {
        next = static_cast<uint16_t>(next + 1);
        this->set_catalog_selection_(next);
        ESP_LOGI(TAG, "PAX catalog selection: %u/%u (down)", static_cast<unsigned>(next + 1),
                 static_cast<unsigned>(count));
      }
    }
  }

  const bool select_pressed = (a && !this->launcher_a_state_) || (touch && !this->launcher_touch_state_);
  if (select_pressed) {
    lv_group_t *group = lv_group_get_default();
    lv_obj_t *focused = group != nullptr ? lv_group_get_focused(group) : nullptr;
    lv_obj_t *selected = nullptr;
    if (this->catalog_container_ != nullptr && !this->catalog_entries_.empty())
      selected = lv_obj_get_child(this->catalog_container_,
                                  static_cast<int32_t>(this->catalog_selection_ + this->catalog_header_rows_));
    if (selected != nullptr && focused == selected) {
      ESP_LOGI(TAG, "PAX catalog physical select: %u/%u", static_cast<unsigned>(this->catalog_selection_ + 1),
               static_cast<unsigned>(this->catalog_entries_.size()));
      lv_obj_send_event(selected, LV_EVENT_CLICKED, nullptr);
    } else
      this->activate_store_focus_();
  }

  remember();
}

void PaxLoader::ensure_catalog_volume_control_() {
  if (this->catalog_volume_button_ != nullptr && this->catalog_volume_slider_ != nullptr)
    return;

  lv_obj_t *layer = lv_layer_top();
  if (layer == nullptr)
    return;
  const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
  constexpr int32_t button_w = 44;
  // Match the YAML top-row buttons: page pad_top is 4 and widget y is 4, so screen Y is 8.
  constexpr int32_t toolbar_top = 8;
  constexpr int32_t button_h = 32;
  constexpr int32_t slider_w = 160;
  constexpr int32_t slider_h = 18;
  constexpr int32_t gap = 10;
  // Position the volume button left of the compact Refresh (screen x:908) and Back (screen x:962)
  // utility icons: at screen_w-170=854 on a 1024-wide panel. Width 44 leaves a 10px gap to Refresh.
  // The slider opens left of the favorites utility button (screen x:624..784), avoiding any overlap.
  const int32_t button_x = std::max<int32_t>(0, screen_w - 170);
  const int32_t slider_x = std::max<int32_t>(0, screen_w - 400);

  this->catalog_volume_slider_ = lv_slider_create(layer);
  lv_obj_set_size(this->catalog_volume_slider_, slider_w, slider_h);
  lv_obj_set_pos(this->catalog_volume_slider_, slider_x,
                 toolbar_top + (button_h - slider_h) / 2);
  lv_slider_set_range(this->catalog_volume_slider_, 0, 100);
  lv_slider_set_value(this->catalog_volume_slider_, this->master_volume_, LV_ANIM_OFF);
  lv_obj_add_flag(this->catalog_volume_slider_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(this->catalog_volume_slider_, lv_color_hex(0x334155), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(this->catalog_volume_slider_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(this->catalog_volume_slider_, lv_color_hex(0x55D6FF), LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(this->catalog_volume_slider_, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_bg_color(this->catalog_volume_slider_, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(this->catalog_volume_slider_, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(this->catalog_volume_slider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(this->catalog_volume_slider_, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_add_event_cb(
      this->catalog_volume_slider_,
      [](lv_event_t *event) {
        lv_event_stop_bubbling(event);
        if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED)
          return;
        auto *loader = static_cast<PaxLoader *>(lv_event_get_user_data(event));
        if (loader != nullptr)
          loader->set_master_volume(
              lv_slider_get_value(static_cast<const lv_obj_t *>(lv_event_get_target(event))));
      },
      LV_EVENT_VALUE_CHANGED, this);
  if (lv_group_t *group = lv_group_get_default())
    lv_group_add_obj(group, this->catalog_volume_slider_);

  this->catalog_volume_button_ = lv_btn_create(layer);
  lv_obj_set_style_bg_color(this->catalog_volume_button_, lv_color_make(30, 41, 59), LV_PART_MAIN);
  lv_obj_set_style_border_color(this->catalog_volume_button_, lv_color_make(71, 85, 105), LV_PART_MAIN);
  lv_obj_set_style_border_width(this->catalog_volume_button_, 1, LV_PART_MAIN);
  lv_obj_set_style_bg_color(this->catalog_volume_button_, lv_color_hex(0x14527A), LV_STATE_FOCUSED);
  lv_obj_set_style_border_color(this->catalog_volume_button_, lv_color_hex(0x55D6FF), LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(this->catalog_volume_button_, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_height(this->catalog_volume_button_, button_h, LV_PART_MAIN);
  lv_obj_set_style_radius(this->catalog_volume_button_, 8, LV_PART_MAIN);
  lv_obj_set_style_width(this->catalog_volume_button_, button_w, LV_PART_MAIN);
  lv_obj_set_style_x(this->catalog_volume_button_, button_x, LV_PART_MAIN);
  lv_obj_set_style_y(this->catalog_volume_button_, toolbar_top, LV_PART_MAIN);
  lv_obj_add_flag(this->catalog_volume_button_, LV_OBJ_FLAG_CLICKABLE);
  // U+F028 is the same speaker glyph used by the running-PAX overlay and
  // the YAML volume page.
  lv_obj_t *volume_label = lv_label_create(this->catalog_volume_button_);
  lv_obj_set_style_align(volume_label, LV_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(volume_label, lv_color_make(203, 213, 225), LV_PART_MAIN);
  if (this->catalog_volume_font_ != nullptr)
    lv_obj_set_style_text_font(volume_label, this->catalog_volume_font_, LV_PART_MAIN);
  lv_label_set_text(volume_label, "\xEF\x80\xA8");
  lv_obj_add_event_cb(
      this->catalog_volume_button_,
      [](lv_event_t *event) {
        lv_event_stop_bubbling(event);
        if (lv_event_get_code(event) != LV_EVENT_CLICKED)
          return;
        auto *loader = static_cast<PaxLoader *>(lv_event_get_user_data(event));
        if (loader != nullptr)
          loader->toggle_catalog_volume_slider();
      },
      LV_EVENT_CLICKED, this);
  if (lv_group_t *group = lv_group_get_default())
    lv_group_add_obj(group, this->catalog_volume_button_);
}

void PaxLoader::raise_catalog_volume_control_() {
  if (this->catalog_volume_slider_ != nullptr)
    lv_obj_move_foreground(this->catalog_volume_slider_);
  if (this->catalog_brightness_slider_ != nullptr)
    lv_obj_move_foreground(this->catalog_brightness_slider_);
  if (this->catalog_volume_button_ != nullptr)
    lv_obj_move_foreground(this->catalog_volume_button_);
}

void PaxLoader::set_catalog_volume_control_visible_(bool visible) {
  if (this->catalog_volume_button_ == nullptr || this->catalog_volume_slider_ == nullptr)
    return;
  if (visible) {
    lv_obj_remove_flag(this->catalog_volume_button_, LV_OBJ_FLAG_HIDDEN);
    // Do not reopen the slider just because the information page was closed.
    lv_obj_add_flag(this->catalog_volume_slider_, LV_OBJ_FLAG_HIDDEN);
    if (this->catalog_brightness_slider_ != nullptr)
      lv_obj_add_flag(this->catalog_brightness_slider_, LV_OBJ_FLAG_HIDDEN);
    this->raise_catalog_volume_control_();
  } else {
    // The information page is also on lv_layer_top(). Hide both controls so
    // they cannot paint over or intercept its action buttons.
    lv_obj_add_flag(this->catalog_volume_button_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(this->catalog_volume_slider_, LV_OBJ_FLAG_HIDDEN);
    if (this->catalog_brightness_slider_ != nullptr)
      lv_obj_add_flag(this->catalog_brightness_slider_, LV_OBJ_FLAG_HIDDEN);
  }
}

void PaxLoader::ensure_catalog_brightness_control_() {
  if (this->catalog_brightness_slider_ != nullptr)
    return;

  lv_obj_t *layer = lv_layer_top();
  if (layer == nullptr)
    return;
  const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
  constexpr int32_t button_h = 32;
  constexpr int32_t slider_w = 160;
  constexpr int32_t slider_h = 18;
  constexpr int32_t toolbar_top = 8;
  const int32_t slider_x = std::max<int32_t>(0, screen_w - 400);
  int32_t level = 100;
  if (this->catalog_brightness_number_ != nullptr && std::isfinite(this->catalog_brightness_number_->state))
    level = std::max<int32_t>(5, std::min<int32_t>(100, static_cast<int32_t>(this->catalog_brightness_number_->state)));

  this->catalog_brightness_slider_ = lv_slider_create(layer);
  lv_obj_set_size(this->catalog_brightness_slider_, slider_w, slider_h);
  lv_obj_set_pos(this->catalog_brightness_slider_, slider_x,
                 toolbar_top + (button_h - slider_h) / 2);
  lv_slider_set_range(this->catalog_brightness_slider_, 5, 100);
  lv_slider_set_value(this->catalog_brightness_slider_, level, LV_ANIM_OFF);
  lv_obj_add_flag(this->catalog_brightness_slider_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(this->catalog_brightness_slider_, lv_color_hex(0x334155), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(this->catalog_brightness_slider_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(this->catalog_brightness_slider_, lv_color_hex(0x55D6FF), LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(this->catalog_brightness_slider_, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_bg_color(this->catalog_brightness_slider_, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(this->catalog_brightness_slider_, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(this->catalog_brightness_slider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(this->catalog_brightness_slider_, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_add_event_cb(
      this->catalog_brightness_slider_,
      [](lv_event_t *event) {
        lv_event_stop_bubbling(event);
        if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED)
          return;
        auto *loader = static_cast<PaxLoader *>(lv_event_get_user_data(event));
        if (loader == nullptr)
          return;
        loader->set_brightness_level(
            lv_slider_get_value(static_cast<const lv_obj_t *>(lv_event_get_target(event))));
      },
      LV_EVENT_VALUE_CHANGED, this);
  if (lv_group_t *group = lv_group_get_default())
    lv_group_add_obj(group, this->catalog_brightness_slider_);
}

void PaxLoader::toggle_catalog_volume_slider() {
  this->ensure_catalog_volume_control_();
  if (this->catalog_volume_slider_ == nullptr)
    return;
  if (PaxLoader::active_ != nullptr)
    PaxLoader::active_->play_ui_sound(UI_SOUND_CONFIRM);
  if (this->catalog_brightness_slider_ != nullptr)
    lv_obj_add_flag(this->catalog_brightness_slider_, LV_OBJ_FLAG_HIDDEN);
  const bool show = lv_obj_has_flag(this->catalog_volume_slider_, LV_OBJ_FLAG_HIDDEN);
  if (show) {
    if (lv_group_t *group = lv_group_get_default()) {
      lv_obj_t *focused = lv_group_get_focused(group);
      if (focused != nullptr && focused != this->catalog_volume_slider_)
        this->catalog_slider_return_focus_ = focused;
    }
    lv_obj_remove_flag(this->catalog_volume_slider_, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(this->catalog_volume_slider_);
    lv_obj_move_foreground(this->catalog_volume_slider_);
  } else {
    lv_obj_add_flag(this->catalog_volume_slider_, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(this->catalog_slider_return_focus_ != nullptr ? this->catalog_slider_return_focus_
                                                                      : this->catalog_volume_button_);
    this->catalog_slider_return_focus_ = nullptr;
  }
  this->raise_catalog_volume_control_();
}

void PaxLoader::toggle_catalog_brightness_slider() {
  this->ensure_catalog_volume_control_();
  this->ensure_catalog_brightness_control_();
  if (this->catalog_brightness_slider_ == nullptr)
    return;
  if (PaxLoader::active_ != nullptr)
    PaxLoader::active_->play_ui_sound(UI_SOUND_CONFIRM);
  if (this->catalog_volume_slider_ != nullptr)
    lv_obj_add_flag(this->catalog_volume_slider_, LV_OBJ_FLAG_HIDDEN);
  const bool show = lv_obj_has_flag(this->catalog_brightness_slider_, LV_OBJ_FLAG_HIDDEN);
  if (show) {
    if (lv_group_t *group = lv_group_get_default()) {
      lv_obj_t *focused = lv_group_get_focused(group);
      if (focused != nullptr && focused != this->catalog_brightness_slider_)
        this->catalog_slider_return_focus_ = focused;
    }
    if (this->catalog_brightness_number_ != nullptr && std::isfinite(this->catalog_brightness_number_->state)) {
      const int32_t level = std::max<int32_t>(5, std::min<int32_t>(100, static_cast<int32_t>(this->catalog_brightness_number_->state)));
      lv_slider_set_value(this->catalog_brightness_slider_, level, LV_ANIM_OFF);
    }
    lv_obj_remove_flag(this->catalog_brightness_slider_, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(this->catalog_brightness_slider_);
    lv_obj_move_foreground(this->catalog_brightness_slider_);
  } else {
    lv_obj_add_flag(this->catalog_brightness_slider_, LV_OBJ_FLAG_HIDDEN);
    if (this->catalog_slider_return_focus_ != nullptr)
      lv_group_focus_obj(this->catalog_slider_return_focus_);
    this->catalog_slider_return_focus_ = nullptr;
  }
  this->raise_catalog_volume_control_();
}

void PaxLoader::hide_catalog_sliders() {
  if (this->catalog_volume_slider_ != nullptr)
    lv_obj_add_flag(this->catalog_volume_slider_, LV_OBJ_FLAG_HIDDEN);
  if (this->catalog_brightness_slider_ != nullptr)
    lv_obj_add_flag(this->catalog_brightness_slider_, LV_OBJ_FLAG_HIDDEN);
}

void PaxLoader::set_catalog_search(const std::string &query) {
  this->catalog_search_query_ = query.substr(0, 64);
  this->catalog_ui_pending_ = true;
}

void PaxLoader::set_rom_search(const std::string &query) {
  this->rom_search_query_ = query.substr(0, 64);
  this->update_rom_selector_ui_();
}

void PaxLoader::set_archive_search(const std::string &query) {
  this->archive_search_query_ = query.substr(0, 64);
  this->archive_search_pending_ = this->archive_mode_ == ARCHIVE_VIEW_SEARCH && this->archive_search_query_.size() >= 2;
  this->archive_search_due_us_ = this->archive_search_pending_ ? esp_timer_get_time() + 450000 : 0;
  if (this->archive_search_query_.size() < 2)
    this->archive_search_results_.clear();
  this->archive_ui_pending_ = true;
}

void PaxLoader::set_archive_settings_label(lv_obj_t *label) {
  this->archive_settings_label_ = label;
  this->update_archive_settings_ui_();
}

void PaxLoader::update_archive_settings_ui_() {
  if (this->archive_settings_label_ == nullptr)
    return;
  std::string text = "Default collection: ";
  text += this->archive_collection_name_.empty() ? "Not selected" : this->archive_collection_name_;
  text += "\nSaved in: ";
  text += this->archive_settings_location_.empty() ? "first mounted storage root" : this->archive_settings_location_;
  lv_label_set_text(this->archive_settings_label_, text.c_str());
}

void PaxLoader::update_archive_ui_() {
  if (this->archive_container_ == nullptr)
    return;
  lv_obj_clean(this->archive_container_);
  // lv_list_add_btn() creates child rows with the default light theme. Keep
  // the archive picker consistent with the dark launcher palette instead of
  // leaving black text on a white row.
  lv_obj_set_style_bg_color(this->archive_container_, lv_color_hex(0x101F31), 0);
  lv_obj_set_style_text_color(this->archive_container_, lv_color_hex(0xE2E8F0), 0);

  auto style_archive_text = [](lv_obj_t *label) {
    if (label == nullptr)
      return;
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xCBD5E1), 0);
  };
  auto style_archive_button = [](lv_obj_t *button) {
    if (button == nullptr)
      return;
    lv_obj_set_style_bg_color(button, lv_color_hex(0x101F31), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x172F4A), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x214B69), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x2D5875), 0);
  };

  if (this->archive_mode_ == ARCHIVE_VIEW_SEARCH) {
    std::string search_title = "SEARCH INTERNET ARCHIVE / " + this->archive_search_collection_;
    style_archive_text(lv_list_add_text(this->archive_container_, search_title.c_str()));
    if (!this->archive_collection_base_.empty()) {
      auto *context = new PaxArchiveSearchButtonContext{this, SIZE_MAX};  // NOLINT
      std::string label = std::string(LV_SYMBOL_DIRECTORY "  Browse default: ") +
                          (this->archive_collection_name_.empty() ? this->archive_collection_base_
                                                                   : this->archive_collection_name_);
      lv_obj_t *button = lv_list_add_btn(this->archive_container_, nullptr, label.c_str());
      if (button != nullptr) {
        style_archive_button(button);
        lv_obj_add_event_cb(button, pax_archive_search_button_event_cb, LV_EVENT_ALL, context);
        add_archive_focus_target_(button);
      } else {
        delete context;
      }
    }
    if (this->archive_loading_) {
      style_archive_text(lv_list_add_text(this->archive_container_, "Searching Archive.org..."));
      return;
    }
    if (this->archive_search_query_.size() < 2) {
      style_archive_text(lv_list_add_text(this->archive_container_,
                                          "Type at least 2 characters to search collections and public items."));
      return;
    }
    if (this->archive_result_ != ESP_OK) {
      const esp_err_t error = static_cast<esp_err_t>(this->archive_result_);
      std::string msg = "Archive.org search: ";
      msg += http_error_hint(error);
      msg += " (";
      msg += esp_err_to_name(error);
      msg += ")";
      style_archive_text(lv_list_add_text(this->archive_container_, msg.c_str()));
      return;
    }
    if (this->archive_search_results_.empty()) {
      style_archive_text(lv_list_add_text(this->archive_container_, "No Archive.org collections found."));
      return;
    }
    std::string header = "Choose a collection (" + std::to_string(this->archive_search_results_.size()) +
                         ") to make it the default:";
    style_archive_text(lv_list_add_text(this->archive_container_, header.c_str()));
    for (size_t i = 0; i < this->archive_search_results_.size(); i++) {
      const auto &result = this->archive_search_results_[i];
      auto *context = new PaxArchiveSearchButtonContext{this, i};  // NOLINT
      std::string label = std::string(LV_SYMBOL_DIRECTORY "  ") + result.title + " [" + result.identifier + "]";
      lv_obj_t *button = lv_list_add_btn(this->archive_container_, nullptr, label.c_str());
      if (button == nullptr) {
        delete context;
        continue;
      }
      style_archive_button(button);
      lv_obj_add_event_cb(button, pax_archive_search_button_event_cb, LV_EVENT_ALL, context);
      add_archive_focus_target_(button);
    }
    return;
  }

  if (this->archive_mode_ == ARCHIVE_VIEW_SYSTEMS) {
    style_archive_text(lv_list_add_text(this->archive_container_, "SELECT GAMING SYSTEM (ARCHIVE.ORG)"));
    for (size_t i = 0; i < sizeof(ARCHIVE_SYSTEMS) / sizeof(ARCHIVE_SYSTEMS[0]); i++) {
      if (!launcher_search_matches(launcher_search_lower(this->archive_search_query_), ARCHIVE_SYSTEMS[i].title))
        continue;
      auto *context = new PaxArchiveSystemButtonContext{this, i};  // NOLINT
      std::string label = std::string(LV_SYMBOL_DIRECTORY "  ") + ARCHIVE_SYSTEMS[i].title;
      lv_obj_t *button = lv_list_add_btn(this->archive_container_, nullptr, label.c_str());
      if (button == nullptr) {
        delete context;
        continue;
      }
      style_archive_button(button);
      lv_obj_add_event_cb(button, pax_archive_system_button_event_cb, LV_EVENT_ALL, context);
      add_archive_focus_target_(button);
    }
    return;
  }

  // ARCHIVE_VIEW_GAMES:
  if (this->archive_loading_) {
    std::string msg = "Loading " + this->archive_system_name_ + " games from Archive.org...";
    style_archive_text(lv_list_add_text(this->archive_container_, msg.c_str()));
    return;
  }
  if (this->archive_entries_.empty()) {
    std::string msg;
    if (this->archive_result_ == ESP_OK) {
      msg = "No games found for " + this->archive_system_name_;
    } else {
      const esp_err_t error = static_cast<esp_err_t>(this->archive_result_);
      msg = "Archive.org: ";
      msg += http_error_hint(error);
      msg += " (";
      msg += esp_err_to_name(error);
      msg += ")";
    }
    style_archive_text(lv_list_add_text(this->archive_container_, msg.c_str()));
    return;
  }
  const std::string query = launcher_search_lower(this->archive_search_query_);
  size_t shown = 0;
  for (const auto &entry : this->archive_entries_)
    if (launcher_search_matches(query, entry.label) || launcher_search_matches(query, entry.path))
      shown++;
  if (shown == 0) {
    style_archive_text(lv_list_add_text(this->archive_container_, "No matching games."));
    return;
  }
  std::string header = this->archive_system_name_ + " (" + std::to_string(shown) + " games available to download)";
  style_archive_text(lv_list_add_text(this->archive_container_, header.c_str()));
  for (const auto &entry : this->archive_entries_) {
    if (!launcher_search_matches(query, entry.label) && !launcher_search_matches(query, entry.path))
      continue;
    auto *context = new PaxArchiveEntryContext{this, entry.url, entry.path, entry.size};  // NOLINT
    std::string label = std::string(LV_SYMBOL_DOWNLOAD "  ") + entry.label;
    lv_obj_t *button = lv_list_add_btn(this->archive_container_, nullptr, label.c_str());
    if (button == nullptr) {
      delete context;
      continue;
    }
    style_archive_button(button);
    lv_obj_add_event_cb(button, pax_archive_entry_event_cb, LV_EVENT_ALL, context);
    add_archive_focus_target_(button);
  }
}

void PaxLoader::update_catalog_ui_() {
  if (this->catalog_container_ == nullptr)
    return;

  // The store view keeps the selection and an open detail page across rebuilds
  // (listings arriving, an install finishing).
  const uint16_t keep_selection = this->catalog_selection_;
  const std::string reopen_url = this->detail_url_;
  if (this->store_ui_)
    this->close_detail_();
  if (this->catalog_header_ != nullptr) {
    // Catalog changes can rebuild the list more than once: first for the
    // empty/loading state and again when the HTTP result arrives.  Preserve
    // focus here as well as in next_catalog(), otherwise Network loses focus
    // during that second asynchronous rebuild even though Storage and Store
    // already have entries available when they switch.
    if (lv_group_t *group = lv_group_get_default()) {
      if (lv_group_get_focused(group) == this->catalog_header_)
        this->catalog_header_focus_pending_ = true;
    }
    lv_obj_add_flag(this->catalog_header_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->catalog_header_);
    this->catalog_header_ = nullptr;
  }
  lv_obj_clean(this->catalog_container_);
  this->catalog_tiles_.clear();
  this->visible_catalog_indices_.clear();
  this->free_icons_();
  this->catalog_selection_ = 0;
  this->catalog_header_rows_ = 0;
  const std::string query = launcher_search_lower(this->catalog_search_query_);
  for (size_t i = 0; i < this->catalog_entries_.size(); i++) {
    const AppInfo *info = i < this->app_info_.size() ? &this->app_info_[i] : nullptr;
    const bool matches = query.empty() || launcher_search_matches(query, this->catalog_entries_[i].first) ||
                         launcher_search_matches(query, this->catalog_entries_[i].second) ||
                         (info != nullptr && (launcher_search_matches(query, info->title) ||
                                              launcher_search_matches(query, info->name)));
    if (matches && (!this->favorites_page_active_ ||
                    this->is_app_favorite_(this->detail_app_key_(static_cast<int>(i)))))
      this->visible_catalog_indices_.push_back(i);
  }
  if (this->store_ui_) {
    // The list's light theme does not suit the store. The source bar is a
    // page-level floating object below, so it stays pinned while this list
    // scrolls.
    lv_obj_set_style_bg_color(this->catalog_container_, lv_color_hex(0x08111F), 0);
    lv_obj_set_style_text_color(this->catalog_container_, lv_color_hex(0xE2E8F0), 0);
    this->build_drawer_();  // this source's side menu, even when it lists nothing
    lv_obj_t *screen = lv_obj_get_screen(this->catalog_container_);
    // Keep the dynamic source/search bar in the active page's object tree.
    // The search textarea is YAML-owned by that page; creating the bar under
    // the display screen made it a later sibling that painted over the
    // textarea during the first boot build. A page-local parent keeps both
    // controls in one z-order and still allows the drawer to remain global.
    lv_obj_t *header_parent = lv_obj_get_parent(this->catalog_container_);
    if (header_parent == nullptr)
      header_parent = screen;
    lv_obj_update_layout(header_parent);
    // Give the pinned source/search bar enough vertical room for the
    // textarea's font and cursor without LVGL adding an inner scrollbar.
    const int32_t header_h = 54;
    const int32_t gap = 4;
    const int32_t list_x = lv_obj_get_x(this->catalog_container_);
    const int32_t list_y = lv_obj_get_y(this->catalog_container_);
    const int32_t width = lv_obj_get_width(this->catalog_container_);
    lv_obj_t *header = lv_obj_create(header_parent);
    lv_obj_set_pos(header, list_x, std::max<int32_t>(0, list_y - header_h - gap));
    lv_obj_set_size(header, width, header_h);
    lv_obj_set_style_pad_all(header, 10, 0);
    lv_obj_set_style_radius(header, 10, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111C33), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0x1E2A44), 0);
    // The source bar is a controller-selectable target, not just a label.
    // Match the blue focus treatment used by the other launcher buttons so
    // it is obvious when A will switch Storage/Network/Store.
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0EA5E9), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(header, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(header, 3, LV_STATE_FOCUSED);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(header, LV_OBJ_FLAG_FLOATING);
    if (this->catalogs_.size() > 1 && !this->favorites_page_active_) {
      lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(header, LV_OBJ_FLAG_CLICK_FOCUSABLE);
      lv_obj_add_event_cb(header, pax_catalog_switch_event_cb, LV_EVENT_CLICKED, this);
    }
    const std::string source_name = this->favorites_page_active_
                                        ? std::string("*  Favorites")
                                        : (this->catalogs_.empty() ? std::string("Library")
                                                                    : this->catalogs_[this->catalog_index_].name);
    const size_t shown = this->visible_catalog_indices_.size();
    const std::string title = this->favorites_page_active_
                                  ? source_name
                                  : std::string(LV_SYMBOL_LEFT "  ") + source_name + "  " LV_SYMBOL_RIGHT;
    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title.c_str());
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xE2E8F0), 0);
    // Leave room for the app count and drawer tab. On the Library page the
    // search controls share this source bar, so keep the source title in its
    // own left-hand region instead of letting a long name run underneath it.
    const int32_t title_width = this->catalog_search_widget_ != nullptr ? 260 : width - 170;
    lv_obj_set_width(title_label, std::max<int32_t>(100, title_width));
    lv_obj_set_height(title_label, 24);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 0, 0);
    const std::string count = std::to_string(shown) + (shown == 1 ? " app" : " apps");
    lv_obj_t *count_label = lv_label_create(header);
    lv_label_set_text(count_label, count.c_str());
    lv_obj_set_style_text_color(count_label, lv_color_hex(0x94A3B8), 0);
    // Keep the count right-justified beside Refresh, but let a larger count
    // grow toward the left. The 116-pixel slot preserves the current layout
    // for ordinary libraries; only counts that need more room push the
    // search controls left instead of running underneath them.
    lv_obj_set_width(count_label, LV_SIZE_CONTENT);
    lv_obj_update_layout(header);
    const int32_t count_text_width = lv_obj_get_width(count_label);
    const int32_t count_slot_width = std::max<int32_t>(116, count_text_width);
    lv_obj_set_width(count_label, count_slot_width);
    lv_obj_set_height(count_label, 24);
    lv_label_set_long_mode(count_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(count_label, LV_TEXT_ALIGN_RIGHT, 0);
    // The YAML refresh button sits beside this source bar at the far right.
    lv_obj_align(count_label, LV_ALIGN_RIGHT_MID, -54, 0);

    const int32_t header_x = lv_obj_get_x(header);
    const int32_t header_y = lv_obj_get_y(header);
    const int32_t count_growth = count_slot_width - 116;
    if (this->catalog_search_widget_ != nullptr) {
      // These are YAML-owned controls so the existing on-screen keyboard and
      // textarea callbacks remain intact. Reposition them into the dynamic
      // source bar and keep the drawer above them when it is opened.
      const int32_t search_x = std::max<int32_t>(title_width + 20, 280 - count_growth);
      lv_obj_set_pos(this->catalog_search_widget_, header_x + search_x, header_y + 5);
      lv_obj_set_size(this->catalog_search_widget_, 430, 44);
      if (this->catalog_clear_search_widget_ != nullptr) {
        lv_obj_set_pos(this->catalog_clear_search_widget_, header_x + search_x + 440, header_y + 5);
        lv_obj_set_size(this->catalog_clear_search_widget_, 44, 44);
      }
      lv_obj_move_foreground(this->catalog_search_widget_);
      if (this->catalog_clear_search_widget_ != nullptr)
        lv_obj_move_foreground(this->catalog_clear_search_widget_);

    }

    // The library refresh action belongs to the same pinned source bar as
    // the Storage/Network/Store selector. Keep it after the count label,
    // before the drawer tab, instead of leaving it in the page toolbar.
    if (this->catalog_refresh_button_ != nullptr) {
      lv_obj_clear_flag(this->catalog_refresh_button_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(this->catalog_refresh_button_, header_x + width - 50, header_y + 5);
      lv_obj_set_size(this->catalog_refresh_button_, 44, 44);
      lv_obj_move_foreground(this->catalog_refresh_button_);
    }

    // Favorites also keeps its back arrow in the pinned source bar. Reserve
    // the slot immediately before the right-justified app count so the arrow
    // cannot overlap the search field, Clear, count, or Refresh controls.
    if (this->catalog_back_button_ != nullptr && this->favorites_page_active_) {
      const int32_t back_x = width - count_slot_width - 104;
      lv_obj_clear_flag(this->catalog_back_button_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(this->catalog_back_button_, header_x + back_x, header_y + 5);
      lv_obj_set_size(this->catalog_back_button_, 44, 44);
      lv_obj_move_foreground(this->catalog_back_button_);
    }

    this->catalog_header_ = header;
    // The source/count bar is created dynamically after the YAML widgets.
    // Put it in the controller group so Up from the first app row reaches
    // "Storage" instead of jumping to an unrelated page's slider.
    if (this->catalogs_.size() > 1 && !this->favorites_page_active_) {
      if (lv_group_t *group = lv_group_get_default())
        lv_group_add_obj(group, header);
    }
    // The header is created after the drawer, so explicitly restore the
    // drawer's z-order. Otherwise the source bar can paint over its panel.
    if (this->drawer_ != nullptr)
      lv_obj_move_foreground(this->drawer_);
    this->ensure_catalog_volume_control_();
    // Volume and brightness now live in the right drawer. Keep the legacy
    // top-layer controls hidden on library/store pages so they cannot overlap
    // the status cluster or the category bar.
    this->set_catalog_volume_control_visible_(false);
    this->raise_catalog_volume_control_();
    // Restore as soon as the replacement header exists. This also covers the
    // Network loading/empty state, which returns below before the normal list
    // selection code runs.
    this->restore_catalog_header_focus_();
  }
  if (this->catalog_entries_.empty()) {
    lv_list_add_text(this->catalog_container_, this->catalog_loading_ ? "Loading..." : "No .pax files found");
    return;
  }
  if (this->store_ui_) {
    if (this->visible_catalog_indices_.empty()) {
      lv_list_add_text(this->catalog_container_, this->favorites_page_active_
                                                   ? "No matching favorites."
                                                   : "No matching apps.");
      return;
    }
    this->build_store_grid_();
    this->set_catalog_selection_(keep_selection);
    this->restore_catalog_header_focus_();
    for (size_t i = 0; i < this->catalog_entries_.size() && !reopen_url.empty(); i++) {
      if (this->catalog_entries_[i].second == reopen_url) {
        for (size_t visible = 0; visible < this->visible_catalog_indices_.size(); visible++) {
          if (this->visible_catalog_indices_[visible] == i) {
            this->set_catalog_selection_(static_cast<uint16_t>(visible));
            this->open_detail_(static_cast<int>(visible));
            break;
          }
        }
        break;
      }
    }
    return;
  }

  for (const size_t source_index : this->visible_catalog_indices_) {
    const auto &entry = this->catalog_entries_[source_index];
    auto *context = new PaxCatalogButtonContext{this, entry.second};  // NOLINT
    lv_obj_t *button = lv_list_add_btn(this->catalog_container_, nullptr, entry.first.c_str());
    if (button == nullptr) {
      delete context;
      continue;
    }
    lv_obj_set_style_bg_color(button, lv_color_hex(0x101F31), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x172F4A), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x214B69), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x2D5875), 0);
    lv_obj_add_event_cb(button, pax_catalog_button_event_cb, LV_EVENT_ALL, context);
  }
  this->set_catalog_selection_(0);
  this->restore_catalog_header_focus_();
}

bool PaxLoader::rom_selector_page_active_() const {
  return this->rom_selector_container_ != nullptr &&
         lv_scr_act() == lv_obj_get_screen(this->rom_selector_container_);
}

void PaxLoader::set_rom_selector_selection_(uint16_t index) {
  if (this->rom_selector_buttons_.empty()) {
    this->rom_selector_selection_ = 0;
    return;
  }
  if (index >= this->rom_selector_buttons_.size())
    index = static_cast<uint16_t>(this->rom_selector_buttons_.size() - 1);
  if (this->rom_selector_selection_ != index) {
    this->play_ui_sound(UI_SOUND_CLICK);
  }
  this->rom_selector_selection_ = index;
  for (lv_obj_t *button : this->rom_selector_buttons_)
    lv_obj_remove_state(button, LV_STATE_FOCUSED);
  lv_obj_t *selected = this->rom_selector_buttons_[index];
  lv_obj_add_state(selected, LV_STATE_FOCUSED);
  lv_group_focus_obj(selected);
  lv_obj_scroll_to_view(selected, LV_ANIM_OFF);

  if (this->rom_selector_position_label_ != nullptr) {
    const std::string &path = this->rom_selector_paths_[index];
    const size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    char letter[2] = {'#', '\0'};
    for (char c : name) {
      if (std::isalnum(static_cast<unsigned char>(c))) {
        letter[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        break;
      }
    }
    lv_label_set_text_fmt(this->rom_selector_position_label_, "INDEX %s", letter);
  }

  // The toolbar star favorites the currently selected ROM. Keep its visual
  // state synchronized when keyboard/gamepad navigation changes selection.
  if (this->rom_selector_favorites_button_ != nullptr) {
    const bool favorite = index < this->rom_selector_paths_.size() &&
                          this->is_rom_favorite_(this->rom_selector_paths_[index]);
    lv_obj_t *label = lv_obj_get_child(this->rom_selector_favorites_button_, 0);
    if (label != nullptr)
      lv_obj_set_style_text_color(label, lv_color_hex(favorite ? 0x38BDF8 : 0xCBD5E1), 0);
    lv_obj_set_style_border_color(this->rom_selector_favorites_button_,
                                  lv_color_hex(favorite ? 0x38BDF8 : 0x475569), 0);
  }
}

static bool rom_selector_extension_matches(const std::string &name, const std::string &extensions) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos)
    return false;
  std::string extension = name.substr(dot);
  for (char &c : extension)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::string allowed = extensions;
  for (char &c : allowed)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return allowed.find(extension + "|") != std::string::npos;
}

void PaxLoader::refresh_rom_selector() {
  this->update_rom_selector_ui_();
}

void PaxLoader::select_rom(const std::string &path) {
  if (path.empty())
    return;
  if (this->launched_ || this->launch_pending_ || this->web_storage_busy_) {
    ESP_LOGW(TAG, "Ignoring ROM selection while a PAX is starting or running: %s", path.c_str());
    return;
  }

  // Normalize ZIP selections before handing them to the PAX. This makes a
  // picker-selected archive work even with an older emulator binary that only
  // accepts the extracted ROM extension, and avoids falling back to its built-in
  // game when the ZIP extractor is not part of the app itself.
  std::string launch_path = path;
  std::string extension = path.substr(path.rfind('.') == std::string::npos ? path.size() : path.rfind('.'));
  for (char &character : extension)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  if (extension == ".zip" && this->rom_selector_app_ != "neogeo") {
    std::string extract_extensions = this->rom_selector_extensions_;
    const size_t zip = extract_extensions.find(".zip|");
    if (zip != std::string::npos)
      extract_extensions.erase(zip, 5);
    char extracted[256] = {};
    const std::string cache_stem = launcher_zip_cache_stem(this->rom_selector_app_, path);
    const int result = PaxLoader::svc_file_zip_extract_first(
        path.c_str(), extract_extensions.c_str(), "/sd/roms/pax/.zipcache",
        cache_stem.c_str(), extracted, sizeof(extracted));
    if (result != 0) {
      ESP_LOGW(TAG, "ROM ZIP selection failed: %s (result %d)", path.c_str(), result);
      this->set_progress_(false, 0, 0, "%s", "Could not extract ZIP ROM");
      return;
    }
    launch_path = extracted;
    ESP_LOGI(TAG, "ROM ZIP selection extracted: %s -> %s", path.c_str(), launch_path.c_str());
  }

  // The emulator PAX reads this shared setting at startup. The sidecar keeps
  // the choice across a reboot when the SD card is available; USB-only systems
  // still work for the current launch through the in-memory setting.
  svc_settings_rom_path_set(launch_path.c_str());
  this->write_file(this->rom_selector_sidecar_, launch_path + "\n");

  std::string app = this->rom_selector_source_;
  if (app.empty())
    app = this->resolve_app_(this->rom_selector_app_);
  if (app.empty()) {
    ESP_LOGW(TAG, "ROM selected but no %s PAX is installed or reachable", this->rom_selector_app_.c_str());
    return;
  }
  ESP_LOGI(TAG, "%s ROM selected: %s; launching %s", this->rom_selector_app_.c_str(), launch_path.c_str(), app.c_str());
  this->request_launch(app);
}

void PaxLoader::update_rom_selector_ui_() {
  if (this->rom_selector_container_ == nullptr)
    return;

  if (this->rom_selector_favorites_button_ != nullptr) {
    const bool selected_favorite =
        this->rom_selector_selection_ < this->rom_selector_paths_.size() &&
        this->is_rom_favorite_(this->rom_selector_paths_[this->rom_selector_selection_]);
    lv_obj_t *label = lv_obj_get_child(this->rom_selector_favorites_button_, 0);
    if (label != nullptr) {
      lv_label_set_text(label, "\xEF\x80\x85");  // U+F005 Font Awesome solid star
      lv_obj_set_style_text_color(label,
          lv_color_hex(selected_favorite ? 0x38BDF8 : 0xCBD5E1), 0);
    }
    lv_obj_set_style_border_color(this->rom_selector_favorites_button_,
        lv_color_hex(selected_favorite ? 0x38BDF8 : 0x475569), 0);
  }
  if (this->rom_selector_favorites_filter_button_ != nullptr) {
    lv_obj_t *label = lv_obj_get_child(this->rom_selector_favorites_filter_button_, 0);
    if (label != nullptr)
      lv_obj_set_style_text_color(label,
          lv_color_hex(this->rom_selector_favorites_only_ ? 0x38BDF8 : 0xA8D8F0), 0);
    lv_obj_set_style_bg_color(this->rom_selector_favorites_filter_button_,
        lv_color_hex(this->rom_selector_favorites_only_ ? 0x14527A : 0x0D2137), 0);
    lv_obj_set_style_border_color(this->rom_selector_favorites_filter_button_,
        lv_color_hex(this->rom_selector_favorites_only_ ? 0x38BDF8 : 0x38BDF8), 0);
  }

  this->rom_selector_buttons_.clear();
  this->rom_selector_paths_.clear();
  lv_obj_clean(this->rom_selector_container_);
  lv_obj_set_style_bg_color(this->rom_selector_container_, lv_color_hex(0x08111F), 0);
  lv_obj_set_style_text_color(this->rom_selector_container_, lv_color_hex(0xE2E8F0), 0);
  this->rom_selector_selection_ = 0;

  std::string relative = this->rom_selector_folder_;
  while (!relative.empty() && relative.front() == '/')
    relative.erase(relative.begin());
  while (!relative.empty() && relative.back() == '/')
    relative.pop_back();

  std::vector<std::pair<std::string, std::string>> entries;
  if (!relative.empty()) {
    for (const auto &root : this->data_search_) {
      const std::string folder = root + "/" + relative;
      const std::string fs_folder = runtime_path(folder.c_str());
      DIR *dir = opendir(fs_folder.c_str());
      if (dir == nullptr)
        continue;
      while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || !rom_selector_extension_matches(name, this->rom_selector_extensions_))
          continue;
        struct stat info{};
        if (stat((fs_folder + "/" + name).c_str(), &info) != 0 || !S_ISREG(info.st_mode))
          continue;
        std::string label = name;
        if (root != "/sd")
          label += " (" + root.substr(1) + ")";
        entries.emplace_back(label, folder + "/" + name);
      }
      closedir(dir);
    }
  }
  std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
    std::string a = left.first;
    std::string b = right.first;
    for (char &c : a)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char &c : b)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a < b;
  });

  if (this->rom_selector_favorites_only_) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [this](const auto &entry) {
                    return !this->is_rom_favorite_(entry.second);
                  }),
                  entries.end());
  }

  const std::string query = launcher_search_lower(this->rom_search_query_);
  if (!query.empty()) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&query](const auto &entry) {
                    return !launcher_search_matches(query, entry.first) && !launcher_search_matches(query, entry.second);
                  }),
                  entries.end());
  }

  for (const auto &entry : entries) {
    auto *context = new PaxRomSelectorButtonContext{this, entry.second};  // NOLINT
    const std::string label = (this->is_rom_favorite_(entry.second) ? "* " : "  ") + entry.first;
    lv_obj_t *button = lv_list_add_btn(this->rom_selector_container_, nullptr, label.c_str());
    if (button == nullptr) {
      delete context;
      continue;
    }
    lv_obj_set_style_bg_color(button, lv_color_hex(0x101F31), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x172F4A), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x214B69), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x2D5875), 0);
    lv_obj_add_event_cb(button, pax_rom_selector_button_event_cb, LV_EVENT_ALL, context);
    if (lv_group_t *group = lv_group_get_default())
      lv_group_add_obj(group, button);
    this->rom_selector_buttons_.push_back(button);
    this->rom_selector_paths_.push_back(entry.second);
  }

  if (this->rom_selector_buttons_.empty()) {
    std::string message = (this->rom_selector_favorites_only_ ? "No favorite games yet in /" : "No compatible games found in /") + relative +
                          "\nSupported files: " + this->rom_selector_extensions_ +
                          "\n\nChecked /sd and /usb0.";
    lv_list_add_text(this->rom_selector_container_, message.c_str());
    if (!this->rom_selector_favorites_only_) {
      auto *context = new PaxRomArchiveButtonContext{this};  // NOLINT
      lv_obj_t *button = lv_list_add_btn(this->rom_selector_container_, nullptr,
                                         LV_SYMBOL_DOWNLOAD "  Open Internet Archive");
      if (button == nullptr) {
        delete context;
      } else {
        lv_obj_set_style_bg_color(button, lv_color_hex(0x101F31), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x172F4A), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x214B69), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(button, lv_color_hex(0x55D6FF), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x2D5875), 0);
        lv_obj_add_event_cb(button, pax_rom_archive_button_event_cb, LV_EVENT_ALL, context);
        if (lv_group_t *group = lv_group_get_default())
          lv_group_add_obj(group, button);
      }
    }
  } else {
    this->set_rom_selector_selection_(0);
  }
  ESP_LOGI(TAG, "%s ROM selector: found %u game(s) under %s across %u storage root(s)",
           this->rom_selector_app_.c_str(),
           static_cast<unsigned>(this->rom_selector_buttons_.size()), relative.c_str(),
           static_cast<unsigned>(this->data_search_.size()));
}
#endif

#ifdef PAX_LOADER_USE_LVGL
struct RomSelectorDefinition {
  const char *app;
  const char *folder;
  const char *extensions;
};

// Keep this table aligned with launcher/main/main.c and the extensions passed
// to each emulator's emu_rom_path()/emu_load_rom() helper. Data-driven ports
// such as Doom, Quake, Red Alert, and ScummVM intentionally launch directly.
static const RomSelectorDefinition ROM_SELECTOR_DEFINITIONS[] = {
    {"nes", "roms/nes", ".nes|.zip|"},
    {"nesplus", "roms/nes", ".nes|.fds|.nsf|.zip|"},
    {"gb", "roms/gb", ".gb|.gbc|.zip|"},
    {"sms", "roms/sms", ".sms|.gg|.col|.zip|"},
    {"spectrum", "roms/spectrum", ".z80|.sna|.tap|.tzx|.zip|"},
    {"stella", "roms/a26", ".a26|.bin|.zip|"},
    {"handy", "roms/lynx", ".lnx|.lyx|.zip|"},
    {"pce", "roms/pce", ".pce|.zip|"},
    {"atari800", "roms/a800", ".atr|.xex|.com|.cas|.a52|.car|.rom|.bin|.zip|"},
    {"snes", "roms/snes", ".smc|.sfc|.zip|"},
    {"genesis", "roms/gen", ".md|.gen|.bin|.smd|.zip|"},
    {"prosystem", "roms/a78", ".a78|.bin|.zip|"},
    {"neogeo", "roms/neogeo", ".zip|"},
    {"ngp", "roms/ngp", ".ngp|.ngc|.zip|"},
    {"ws", "roms/ws", ".ws|.wsc|.zip|"},
    {"msx", "roms/msx", ".rom|.mx1|.mx2|.zip|"},
    {"gx4000", "roms/gx4000", ".cpr|.bin|.zip|"},
    {"gw", "roms/gw", ".gw|.lcd|.zip|"},
};

static std::string rom_selector_app_id(const std::string &source) {
  // Store URLs are versioned (for example nes-0.1.0.pax), while the
  // selector definitions are keyed by the app's unversioned name (nes).
  // Keep this in one place with the rest of the loader's app-key handling so
  // detail-page actions and the library's Launch action behave identically.
  std::string value = canvas::app_key(source);
  for (char &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

static const RomSelectorDefinition *rom_selector_definition_for(const std::string &source) {
  const std::string id = rom_selector_app_id(source);
  for (const auto &definition : ROM_SELECTOR_DEFINITIONS) {
    if (id == definition.app)
      return &definition;
  }
  return nullptr;
}

bool PaxLoader::open_rom_selector_for_app(const std::string &source) {
  const RomSelectorDefinition *definition = rom_selector_definition_for(source);
  if (definition == nullptr)
    return false;

  this->rom_selector_app_ = definition->app;
  this->rom_selector_folder_ = definition->folder;
  this->rom_selector_extensions_ = definition->extensions;
  this->rom_selector_source_ = source;
  this->rom_selector_sidecar_ = std::string("/sd/roms/pax/") + definition->app + ".rom";
  this->rom_selector_favorites_only_ = false;

  if (this->rom_selector_container_ == nullptr) {
    ESP_LOGW(TAG, "ROM selector for %s is not configured in the YAML", definition->app);
    return false;
  }

  // Remember the emulator list that opened the picker. This keeps B and the
  // on-screen Back button on the same page, whether the picker came from the
  // library or the store catalog.
  this->rom_selector_return_container_ = this->catalog_container_;

  // A store detail panel lives over the library page. Remove it before
  // switching screens so its deferred deletion cannot cover the picker.
  this->close_detail_();
  lv_scr_load(lv_obj_get_screen(this->rom_selector_container_));
  this->update_rom_selector_ui_();
  ESP_LOGI(TAG, "Opening %s ROM selector: %s (%s)", definition->app, definition->folder,
           definition->extensions);
  return true;
}

void PaxLoader::back_from_rom_selector() {
  this->play_ui_sound(UI_SOUND_BACK);
#ifdef PAX_LOADER_USE_LVGL
  lv_obj_t *target = this->rom_selector_return_container_ != nullptr
                         ? this->rom_selector_return_container_
                         : this->catalog_container_;
  if (target != nullptr) {
    this->hide_catalog_sliders();
    lv_scr_load(lv_obj_get_screen(target));
  }
#endif
}

bool PaxLoader::supports_rom_selector(const std::string &source) const {
  return rom_selector_definition_for(source) != nullptr;
}
#endif

void PaxLoader::finish_app_() {
  if (!begin_pax_quiesce()) return;
  this->close_game_menu_();
  svc_game_menu_register(nullptr, nullptr, nullptr);
  const int result = this->pax_task_result_;
  ESP_LOGI(TAG, "PAX worker returned %d", result);
  const bool reload_after_close = this->reload_after_close_ && !this->reload_source_.empty();
  const std::string reload_source = reload_after_close ? this->reload_source_ : std::string();
  bool chained_app_pending = reload_after_close;
  if (this->handoff_mutex_ != nullptr &&
      xSemaphoreTake(this->handoff_mutex_, portMAX_DELAY) == pdTRUE) {
    chained_app_pending = chained_app_pending || this->chain_.pending() || this->chain_.depth() != 0;
    xSemaphoreGive(this->handoff_mutex_);
  }
  // Read before the close flags are reset below: was the app closed from the
  // loader, or did it return on its own?
  const bool closed_by_user = this->global_close_requested_ || this->toggle_close_requested_;
  // Stop the speaker before deleting the worker or unloading the PAX. The
  // speaker owns asynchronous mixer/resampler tasks and must be quiescent
  // before any app-side teardown can release memory associated with a frame.
  if (this->speaker_ != nullptr) {
    ESP_LOGI(TAG, "PAX close: stopping audio");
    this->speaker_->stop();
    ESP_LOGI(TAG, "PAX close: audio stopped");
  }
  // FILE storage may belong to a task's Newlib context. Close SDK handles
  // while those contexts still exist, before deleting their owning tasks.
  close_pax_files();
  if (this->pax_task_handle_ != nullptr) {
    ESP_LOGI(TAG, "PAX close: deleting worker task");
    if (this->pax_task_uses_caps_)
      vTaskDeleteWithCaps(this->pax_task_handle_);
    else
      vTaskDelete(this->pax_task_handle_);
    this->pax_task_handle_ = nullptr;
    this->pax_task_uses_caps_ = false;
    ESP_LOGI(TAG, "PAX close: worker task deleted");
  }
  // A PAX-created PSRAM task must not keep executing code from the mapped
  // image after that image is unmapped below.
  portENTER_CRITICAL(&pax_tasks_mux);
  pax_entry_task = nullptr;
  portEXIT_CRITICAL(&pax_tasks_mux);
  delete_registered_pax_tasks();
  release_pax_resources();
  if (this->app_handle_ != nullptr) {
    ESP_LOGI(TAG, "PAX close: unloading app");
    psram_app_unload(this->app_handle_);
    this->app_handle_ = nullptr;
    ESP_LOGI(TAG, "PAX close: app unloaded");
  }
  close_app_sockets_();
  close_app_tls_();
  log_heap("after the app ended", true);
  this->toggle_wait_release_ = this->toggle_button_ != nullptr;
  this->toggle_close_requested_ = false;
  this->global_close_requested_ = false;
  this->launched_ = false;
  this->running_app_source_.clear();
  this->reload_after_close_ = false;
  this->reload_source_.clear();
  this->control_overlay_offset_ = 0;
  this->control_overlay_rendered_offset_ = 0;
  this->control_overlay_last_touch_us_ = 0;
  this->control_overlay_last_redraw_us_ = 0;
  this->control_overlay_rendered_ = false;
  this->control_overlay_wake_gesture_active_ = false;
  this->control_overlay_swipe_tracking_ = false;
  this->control_overlay_swipe_start_x_ = 0;
  this->control_overlay_swipe_start_y_ = 0;
  this->control_overlay_swipe_start_us_ = 0;
  this->volume_touch_active_ = false;
  this->brightness_touch_active_ = false;
  this->volume_slider_visible_ = false;
  this->brightness_slider_visible_ = false;
  this->control_overlay_repaint_pending_ = false;
#ifdef PAX_LOADER_USE_LVGL
  // Require a release before the next launcher input is accepted after an
  // app closes. This prevents the button used to exit an app from launching
  // the currently highlighted PAX immediately afterward.
  this->launcher_input_armed_ = false;
  this->launcher_direction_state_ = 0;
  this->launcher_a_state_ = false;
  this->launcher_touch_state_ = false;
#endif
  // Remove every pixel from the outgoing app before handing off. launcher_ui
  // can use the native panel while the game PAX that follows it uses the
  // smaller legacy canvas, so stale pixels must not remain around that area.
  if (this->geometry_.canvas_w != canvas::LEGACY_WIDTH ||
      this->geometry_.canvas_h != canvas::LEGACY_HEIGHT) {
    this->restore_legacy_canvas_();  // changing canvas size also clears the panel
  } else {
    this->blank_panel_();
  }
  // Keep the full-panel black shield over the host UI during app-to-app
  // handoffs (including launcher reload). Repainting the dashboard between
  // chained apps exposes the old UI around the next app's smaller canvas.
  if (chained_app_pending) {
    ESP_LOGI(TAG, "Keeping display black during chained PAX hand-off");
  } else {
    this->restore_lvgl_();
  }
  this->send_report_(closed_by_user ? "closed" : "exited", result, this->report_source_);
  if (reload_after_close) {
    this->path_ = reload_source;
    this->next_arg_.clear();
    this->chain_launch_ = false;
    this->pax_frame_count_ = 0;
#ifdef PAX_LOADER_USE_LVGL
    if (reload_source.find("launcher_ui") == std::string::npos) {
      this->show_loading_message_();
      if (this->lvgl_ != nullptr && this->lvgl_->get_disp() != nullptr)
        lv_refr_now(this->lvgl_->get_disp());
    }
#endif
    this->launch_pending_ = true;
    ESP_LOGI(TAG, "Reloading PAX after clean shutdown: %s", this->path_.c_str());
  } else {
    // An app it handed over to (app_open), or the app to go back to.
    this->continue_chain_();
  }
}

void PaxLoader::continue_chain_() {
  handoff::Frame next;
  bool have = false;
  if (this->handoff_mutex_ != nullptr && xSemaphoreTake(this->handoff_mutex_, portMAX_DELAY) == pdTRUE) {
    have = this->chain_.next(&next);
    xSemaphoreGive(this->handoff_mutex_);
  }
  if (!have || next.source.empty())
    return;
  ESP_LOGI(TAG, "App hand-off: starting %s%s", next.source.c_str(), next.arg.empty() ? "" : " with an argument");
  this->path_ = next.source;
  this->next_arg_ = next.arg;
  this->chain_launch_ = true;
  this->pax_frame_count_ = 0;
#ifdef PAX_LOADER_USE_LVGL
  if (next.source.find("launcher_ui") == std::string::npos) {
    this->show_loading_message_();
    if (this->lvgl_ != nullptr && this->lvgl_->get_disp() != nullptr)
      lv_refr_now(this->lvgl_->get_disp());
  }
#endif
  this->launch_pending_ = true;
}

void PaxLoader::clear_(uint16_t color) {
  if (this->framebuffer_ != nullptr)
    std::fill_n(this->framebuffer_, static_cast<size_t>(this->geometry_.canvas_w) * this->geometry_.canvas_h, color);
}

void PaxLoader::wake_control_overlay_() {
  this->control_overlay_last_touch_us_ = esp_timer_get_time();
  if (this->control_overlay_offset_ != 0) {
    this->control_overlay_offset_ = 0;
    this->control_overlay_repaint_pending_ = true;
  }
}

uint16_t PaxLoader::game_menu_buttons_() const {
  uint16_t buttons = 0;
  for (uint8_t i = 0; i < BUTTON_COUNT && i < 16; ++i) {
    if (this->buttons_[i] != nullptr && this->buttons_[i]->get_state_default(false))
      buttons |= static_cast<uint16_t>(1U << i);
  }
  pax_gamepad_state_t remote{};
  const_cast<PaxLoader *>(this)->read_remote_ui_input_(&remote);
  for (uint8_t i = 0; i < BUTTON_COUNT && i < 16; ++i) {
    if (remote.values[i])
      buttons |= static_cast<uint16_t>(1U << i);
  }
  return buttons;
}

bool PaxLoader::game_menu_capture_held_() const {
  return this->game_menu_button_ != nullptr && this->game_menu_button_->get_state_default(false);
}

void PaxLoader::build_game_menu_frame_() {
  if (this->game_menu_frame_ == nullptr || this->game_menu_width_ <= 0 || this->game_menu_height_ <= 0)
    return;
  std::fill_n(this->game_menu_frame_, GAME_MENU_FRAME_PIXELS, overlay_theme::BACKGROUND);
  const int width = this->game_menu_width_;
  const int height = this->game_menu_height_;
  const int stride = GAME_MENU_FRAME_WIDTH;
  const uint16_t border = overlay_theme::MUTED;
  const uint16_t header = overlay_theme::PANEL;
  const uint16_t selected = overlay_theme::SELECTED;
  const uint16_t accent = overlay_theme::ACCENT;
  const uint16_t text = overlay_theme::TEXT;
  game_menu_fill_rect(this->game_menu_frame_, stride, width, height, 0, 0, width, height, overlay_theme::BACKGROUND);
  game_menu_fill_rect(this->game_menu_frame_, stride, width, height, 0, 0, width, 2, border);
  game_menu_fill_rect(this->game_menu_frame_, stride, width, height, 0, height - 2, width, 2, border);
  game_menu_fill_rect(this->game_menu_frame_, stride, width, height, 0, 0, 2, height, border);
  game_menu_fill_rect(this->game_menu_frame_, stride, width, height, width - 2, 0, 2, height, border);
  game_menu_fill_rect(this->game_menu_frame_, stride, width, height, 2, 2, width - 4, 37, header);
  const int title_scale = width >= 360 ? 2 : 1;
  game_menu_draw_text(this->game_menu_frame_, stride, width, height, 18, 12,
                      this->game_menu_app_page_ ? this->game_menu_page_.title : "GAME MENU", accent, title_scale);

  static constexpr const char *const labels[GAME_MENU_NES_ITEM_COUNT] = {
      "RESUME GAME", "RESTART APP", "EXIT TO LAUNCHER", "VOLUME", "BRIGHTNESS", "NES OPTIONS"};
  const int row_top = this->game_menu_row_top_;
  const int row_height = this->game_menu_row_height_;
  const int scale = this->game_menu_font_scale_;
  const int bar_x = std::max(145, width / 2);
  const int bar_width = std::max(36, width - bar_x - 22);
  for (uint8_t row = 0; row < this->game_menu_item_count_; ++row) {
    const int y = row_top + row * row_height;
    if (row == this->game_menu_selection_)
      game_menu_fill_rect(this->game_menu_frame_, stride, width, height, 8, y, width - 16, row_height - 2, selected);
    const int text_y = y + std::max(1, (row_height - 7 * scale) / 2);
    const uint16_t color = text;
    if (row == this->game_menu_selection_)
      game_menu_draw_text(this->game_menu_frame_, stride, width, height, 17, text_y, ">", accent, scale);
    const char *label = this->game_menu_app_page_ ? this->game_menu_page_.items[row].label :
                        row == GAME_MENU_BASE_ITEM_COUNT && this->game_menu_callback_ != nullptr ?
                        this->game_menu_provider_label_ : labels[row];
    game_menu_draw_text(this->game_menu_frame_, stride, width, height, 34, text_y, label, color, scale);
    if (!this->game_menu_app_page_ && (row == 3 || row == 4)) {
      const int bar_y = y + row_height / 2 - 4;
      const int level = row == 3 ? this->master_volume_ : s_brightness;
      game_menu_fill_rect(this->game_menu_frame_, stride, width, height, bar_x, bar_y, bar_width, 8, overlay_theme::MUTED);
      game_menu_fill_rect(this->game_menu_frame_, stride, width, height, bar_x, bar_y,
                          bar_width * std::max(0, std::min(100, level)) / 100, 8, accent);
    }
  }
  const int footer_y = height - 20;
  game_menu_fill_rect(this->game_menu_frame_, stride, width, height, 8, height - 29, width - 16, 1, overlay_theme::MUTED);
  if (this->game_menu_app_page_ && this->game_menu_page_.preview != nullptr && width >= 360) {
    const int preview_x = width - PAX_GAME_MENU_PREVIEW_WIDTH - 12;
    for (int y = 0; y < PAX_GAME_MENU_PREVIEW_HEIGHT; ++y)
      std::memcpy(this->game_menu_frame_ + (46 + y) * stride + preview_x,
                  this->game_menu_page_.preview + y * PAX_GAME_MENU_PREVIEW_WIDTH,
                  PAX_GAME_MENU_PREVIEW_WIDTH * sizeof(uint16_t));
  }
  const char *status = this->game_menu_busy_ ? "WORKING..." :
      this->game_menu_app_page_ && this->game_menu_page_.status[0] != '\0' ? this->game_menu_page_.status :
      "UP DOWN MOVE LEFT RIGHT ADJUST A SELECT B BACK";
  game_menu_draw_text(this->game_menu_frame_, stride, width, height, 12, footer_y, status, overlay_theme::MUTED, 1);
  // draw_pixels_at() writes raw panel pixels and bypasses the display
  // component's rotation transform. The PAX panel is mounted at 180 degrees,
  // so rotate this software-rendered overlay to match the app framebuffer.
  const size_t visible_pixels = static_cast<size_t>(width) * height;
  for (size_t index = 0; index < visible_pixels / 2; ++index) {
    const int x = static_cast<int>(index % width);
    const int y = static_cast<int>(index / width);
    const size_t first = static_cast<size_t>(y) * stride + x;
    const size_t opposite = static_cast<size_t>(height - 1 - y) * stride + (width - 1 - x);
    std::swap(this->game_menu_frame_[first], this->game_menu_frame_[opposite]);
  }
  esp_cache_msync(this->game_menu_frame_, GAME_MENU_FRAME_PIXELS * sizeof(uint16_t),
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  this->game_menu_frame_dirty_ = false;
}

void PaxLoader::draw_game_menu_() {
  if (!this->game_menu_open_ || this->display_ == nullptr || this->game_menu_frame_ == nullptr)
    return;
  if (this->game_menu_frame_dirty_)
    this->build_game_menu_frame_();
  const int raw_x = this->geometry_.panel_w - this->game_menu_screen_x_ - this->game_menu_width_;
  const int raw_y = this->geometry_.panel_h - this->game_menu_screen_y_ - this->game_menu_height_;
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, this->game_menu_width_, this->game_menu_height_))
    return;
  this->display_->draw_pixels_at(raw_x, raw_y, this->game_menu_width_, this->game_menu_height_,
                                 reinterpret_cast<const uint8_t *>(this->game_menu_frame_),
                                 display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false, 0, 0,
                                 GAME_MENU_FRAME_WIDTH - this->game_menu_width_);
}

void PaxLoader::draw_game_menu_button_() {
  if (!this->launched_ || this->display_ == nullptr)
    return;
  const int raw_x = this->geometry_.game_menu_raw_x(this->control_overlay_offset_);
  const int raw_y = this->geometry_.game_menu_raw_y(this->control_overlay_offset_);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, GAME_MENU_BUTTON_SIZE, GAME_MENU_BUTTON_SIZE))
    return;
  std::fill_n(game_menu_button_overlay_buffer, GAME_MENU_BUTTON_SIZE * GAME_MENU_BUTTON_SIZE, overlay_theme::PANEL);
  for (int y : {17, 27, 37}) {
    for (int x = 14; x < 44; ++x) {
      game_menu_button_overlay_buffer[y * GAME_MENU_BUTTON_SIZE + x] = overlay_theme::TEXT;
      game_menu_button_overlay_buffer[(y + 1) * GAME_MENU_BUTTON_SIZE + x] = overlay_theme::TEXT;
    }
  }
  this->display_->draw_pixels_at(raw_x, raw_y, GAME_MENU_BUTTON_SIZE, GAME_MENU_BUTTON_SIZE,
                                 reinterpret_cast<const uint8_t *>(game_menu_button_overlay_buffer),
                                 display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false);
}

void PaxLoader::clear_game_menu_button_(int slide) {
  if (this->display_ == nullptr)
    return;
  const int raw_x = this->geometry_.game_menu_raw_x(slide);
  const int raw_y = this->geometry_.game_menu_raw_y(slide);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, GAME_MENU_BUTTON_SIZE, GAME_MENU_BUTTON_SIZE))
    return;
  std::fill_n(game_menu_button_overlay_buffer, GAME_MENU_BUTTON_SIZE * GAME_MENU_BUTTON_SIZE, 0x0000);
  this->display_->draw_pixels_at(raw_x, raw_y, GAME_MENU_BUTTON_SIZE, GAME_MENU_BUTTON_SIZE,
                                 reinterpret_cast<const uint8_t *>(game_menu_button_overlay_buffer),
                                 display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false);
}

void PaxLoader::open_game_menu_() {
  if (!this->launched_ || this->global_close_requested_ || this->game_menu_open_ || this->display_ == nullptr)
    return;
  const bool locked = this->display_mutex_ != nullptr &&
                      xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY) == pdTRUE;
  if (this->game_menu_frame_ == nullptr) {
    this->game_menu_frame_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
        64, 1, GAME_MENU_ALLOC_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
    if (this->game_menu_frame_ == nullptr)
      this->game_menu_frame_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
          64, 1, GAME_MENU_ALLOC_PIXELS * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  }
  if (this->game_menu_frame_ == nullptr) {
    if (locked)
      xSemaphoreGiveRecursive(this->display_mutex_);
    ESP_LOGE(TAG, "Could not allocate the in-game menu frame buffer");
    return;
  }
  this->game_menu_width_ = std::min(GAME_MENU_FRAME_WIDTH, this->geometry_.canvas_w - 24);
  this->game_menu_height_ = std::min(GAME_MENU_FRAME_HEIGHT, this->geometry_.canvas_h - 24);
  this->game_menu_width_ = std::max(240, this->game_menu_width_);
  this->game_menu_height_ = std::max(200, this->game_menu_height_);
  this->game_menu_item_count_ =
      this->game_menu_callback_ != nullptr || canvas::app_key(this->running_app_source_) == "nes" ?
      GAME_MENU_NES_ITEM_COUNT : GAME_MENU_BASE_ITEM_COUNT;
  this->game_menu_app_page_ = false;
  this->game_menu_page_ = {};
  this->game_menu_screen_x_ = this->geometry_.x() + (this->geometry_.canvas_w - this->game_menu_width_) / 2;
  this->game_menu_screen_y_ = this->geometry_.y() + (this->geometry_.canvas_h - this->game_menu_height_) / 2;
  this->game_menu_font_scale_ = this->game_menu_width_ >= 360 ? 2 : 1;
  this->game_menu_row_top_ = 46;
  this->game_menu_row_height_ = (this->game_menu_height_ - 84) / this->game_menu_item_count_;
  this->game_menu_selection_ = 0;
  this->game_menu_previous_buttons_ = this->game_menu_buttons_();
  this->game_menu_frame_dirty_ = true;
  this->game_menu_open_ = true;
  this->volume_slider_visible_ = false;
  this->brightness_slider_visible_ = false;
  this->control_overlay_last_touch_us_ = esp_timer_get_time();
  this->control_overlay_offset_ = 0;
  this->control_overlay_repaint_pending_ = true;
  if (this->speaker_ != nullptr)
    this->speaker_->set_mute_state(true);
  if (locked)
    xSemaphoreGiveRecursive(this->display_mutex_);
  this->redraw_control_overlay_now_(true);
  ESP_LOGI(TAG, "PAX in-game menu opened; game input paused at the next input poll");
}

void PaxLoader::close_game_menu_() {
  if (!this->game_menu_open_)
    return;
  const bool locked = this->display_mutex_ != nullptr &&
                      xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY) == pdTRUE;
  this->game_menu_open_ = false;
  portENTER_CRITICAL(&this->game_menu_button_lock_);
  this->game_menu_busy_ = false;
  this->game_menu_action_pending_ = false;
  portEXIT_CRITICAL(&this->game_menu_button_lock_);
  this->game_menu_app_page_ = false;
  this->game_menu_page_.preview = nullptr;
  this->game_menu_quarantine_ = true;
  this->game_menu_quarantine_until_us_ = esp_timer_get_time() + 2000000;
  this->game_menu_previous_buttons_ = this->game_menu_buttons_();
  this->game_menu_touch_active_ = false;
  if (this->speaker_ != nullptr)
    this->speaker_->set_mute_state(this->master_volume_ == 0);
  if (this->game_menu_frame_ != nullptr) {
    heap_caps_free(this->game_menu_frame_);
    this->game_menu_frame_ = nullptr;
  }
  this->game_menu_width_ = 0;
  this->game_menu_height_ = 0;
  this->game_menu_frame_dirty_ = true;
  if (locked)
    xSemaphoreGiveRecursive(this->display_mutex_);
  this->control_overlay_last_touch_us_ = esp_timer_get_time();
  this->redraw_control_overlay_now_(true);
  ESP_LOGI(TAG, "PAX in-game menu closed; waiting for menu buttons to be released");
}

void PaxLoader::activate_game_menu_item_() {
  if (this->game_menu_app_page_) {
    const auto &item = this->game_menu_page_.items[this->game_menu_selection_];
    if ((item.flags & PAX_GAME_MENU_ITEM_DISABLED) == 0)
      this->queue_game_menu_action_(item.action);
    return;
  }
  switch (this->game_menu_selection_) {
    case 0:
      this->close_game_menu_();
      return;
    case 1:
      if (!this->running_app_source_.empty()) {
        this->reload_source_ = this->running_app_source_;
        this->reload_after_close_ = true;
        this->begin_close_();
        ESP_LOGI(TAG, "PAX in-game menu requested app restart");
      }
      return;
    case 2:
      this->begin_close_();
      ESP_LOGI(TAG, "PAX in-game menu requested return to launcher");
      return;
    case 3:
      this->set_master_volume(this->master_volume_ + 5);
      break;
    case 4:
      this->set_brightness_level(s_brightness + 5);
      break;
    case 5:
      if (this->game_menu_item_count_ > GAME_MENU_BASE_ITEM_COUNT) {
        if (this->game_menu_callback_ != nullptr) {
          this->queue_game_menu_action_(PAX_GAME_MENU_ACTION_OPEN);
          return;
        }
        portENTER_CRITICAL(&this->game_menu_button_lock_);
        this->game_menu_open_app_options_pending_ = true;
        portEXIT_CRITICAL(&this->game_menu_button_lock_);
        this->close_game_menu_();
        ESP_LOGI(TAG, "PAX in-game menu requested NES options");
      }
      return;
    default:
      break;
  }
  const bool locked = this->display_mutex_ != nullptr &&
                      xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY) == pdTRUE;
  if (locked)
    xSemaphoreGiveRecursive(this->display_mutex_);
  this->redraw_control_overlay_now_(true);
}

void PaxLoader::poll_game_menu_() {
  bool toggle = false;
  portENTER_CRITICAL(&this->game_menu_button_lock_);
  toggle = this->game_menu_toggle_pending_;
  this->game_menu_toggle_pending_ = false;
  portEXIT_CRITICAL(&this->game_menu_button_lock_);
  if (toggle) {
    if (this->game_menu_open_)
      this->close_game_menu_();
    else
      this->open_game_menu_();
  }

  const uint16_t current = this->game_menu_buttons_();
  const uint16_t pressed = static_cast<uint16_t>(current & ~this->game_menu_previous_buttons_);
  this->game_menu_previous_buttons_ = current;
  if (this->game_menu_quarantine_) {
    bool touch_down = false;
    if (this->touchscreen_ != nullptr) {
      const auto touch = this->touchscreen_->get_touch();
      touch_down = touch.has_value() && (touch->state & touchscreen::STATE_RELEASING) == 0;
    }
    if ((current == 0 && !this->game_menu_capture_held_() && !touch_down) ||
        esp_timer_get_time() >= this->game_menu_quarantine_until_us_)
      this->game_menu_quarantine_ = false;
  }
  if (!this->game_menu_open_ || this->game_menu_busy_)
    return;
  if ((pressed & ((1U << PAX_INPUT_B) | (1U << PAX_INPUT_SELECT))) != 0) {
    this->back_game_menu_();
    return;
  }

  bool redraw = false;
  if ((pressed & (1U << PAX_INPUT_DOWN)) != 0) {
    const bool locked = this->display_mutex_ != nullptr &&
                        xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY) == pdTRUE;
    this->game_menu_selection_ =
        static_cast<uint8_t>((this->game_menu_selection_ + 1) % this->game_menu_item_count_);
    this->game_menu_frame_dirty_ = true;
    if (locked)
      xSemaphoreGiveRecursive(this->display_mutex_);
    redraw = true;
  } else if ((pressed & (1U << PAX_INPUT_UP)) != 0) {
    const bool locked = this->display_mutex_ != nullptr &&
                        xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY) == pdTRUE;
    this->game_menu_selection_ = static_cast<uint8_t>(
        (this->game_menu_selection_ + this->game_menu_item_count_ - 1) % this->game_menu_item_count_);
    this->game_menu_frame_dirty_ = true;
    if (locked)
      xSemaphoreGiveRecursive(this->display_mutex_);
    redraw = true;
  } else if (this->game_menu_app_page_ && (pressed & ((1U << PAX_INPUT_LEFT) | (1U << PAX_INPUT_RIGHT))) != 0) {
    const auto &item = this->game_menu_page_.items[this->game_menu_selection_];
    if ((item.flags & PAX_GAME_MENU_ITEM_ADJUSTABLE) != 0 &&
        (item.flags & PAX_GAME_MENU_ITEM_DISABLED) == 0)
      this->queue_game_menu_action_(item.action, (pressed & (1U << PAX_INPUT_LEFT)) != 0 ? -1 : 1);
    return;
  } else if (!this->game_menu_app_page_ && (pressed & (1U << PAX_INPUT_LEFT)) != 0 && this->game_menu_selection_ == 3) {
    this->set_master_volume(this->master_volume_ - 5);
    redraw = true;
  } else if (!this->game_menu_app_page_ && (pressed & (1U << PAX_INPUT_RIGHT)) != 0 && this->game_menu_selection_ == 3) {
    this->set_master_volume(this->master_volume_ + 5);
    redraw = true;
  } else if (!this->game_menu_app_page_ && (pressed & (1U << PAX_INPUT_LEFT)) != 0 && this->game_menu_selection_ == 4) {
    this->set_brightness_level(s_brightness - 5);
    redraw = true;
  } else if (!this->game_menu_app_page_ && (pressed & (1U << PAX_INPUT_RIGHT)) != 0 && this->game_menu_selection_ == 4) {
    this->set_brightness_level(s_brightness + 5);
    redraw = true;
  } else if ((pressed & ((1U << PAX_INPUT_A) | (1U << PAX_INPUT_START))) != 0) {
    this->activate_game_menu_item_();
    return;
  }
  if (redraw)
    this->redraw_control_overlay_now_(true);
}

void PaxLoader::wait_for_game_menu_() {
  while (this->game_menu_open_ && this->launched_ && !this->global_close_requested_) {
    pax_game_menu_action_fn_t callback = nullptr;
    void *context = nullptr;
    uint32_t action = 0;
    int32_t direction = 0;
    portENTER_CRITICAL(&this->game_menu_button_lock_);
    if (this->game_menu_action_pending_ && this->game_menu_task_ == xTaskGetCurrentTaskHandle()) {
      callback = this->game_menu_callback_;
      context = this->game_menu_context_;
      action = this->game_menu_action_;
      direction = this->game_menu_direction_;
      this->game_menu_action_pending_ = false;
    }
    portEXIT_CRITICAL(&this->game_menu_button_lock_);
    if (callback != nullptr) {
      callback(context, action, direction);
      portENTER_CRITICAL(&this->game_menu_button_lock_);
      if (!this->game_menu_action_pending_)
        this->game_menu_busy_ = false;
      portEXIT_CRITICAL(&this->game_menu_button_lock_);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void PaxLoader::queue_game_menu_action_(uint32_t action, int32_t direction) {
  portENTER_CRITICAL(&this->game_menu_button_lock_);
  if (this->game_menu_open_ && this->game_menu_callback_ != nullptr && !this->game_menu_busy_) {
    this->game_menu_action_ = action;
    this->game_menu_direction_ = direction;
    this->game_menu_action_pending_ = true;
    this->game_menu_busy_ = true;
  }
  portEXIT_CRITICAL(&this->game_menu_button_lock_);
  ESP_LOGI(TAG, "Shared menu action %lu direction %ld queued on emulator thread",
           static_cast<unsigned long>(action), static_cast<long>(direction));
}

void PaxLoader::back_game_menu_() {
  if (this->game_menu_app_page_)
    this->queue_game_menu_action_(this->game_menu_page_.back_action);
  else
    this->close_game_menu_();
}

bool PaxLoader::game_menu_touch_(int x, int y) {
  if (!this->launched_)
    return false;
  if (!this->game_menu_open_) {
    if (this->control_overlay_offset_ == 0 && this->geometry_.in_game_menu(x, y, this->control_overlay_offset_)) {
      this->open_game_menu_();
      this->game_menu_touch_active_ = true;
      return true;
    }
    return false;
  }
  const int left = this->game_menu_screen_x_;
  if (this->game_menu_busy_)
    return true;
  const int top = this->game_menu_screen_y_ + this->game_menu_row_top_;
  const int right = left + this->game_menu_width_;
  const int bottom = top + this->game_menu_row_height_ * this->game_menu_item_count_;
  if (x < left || x >= right || y < top || y >= bottom) {
    if (!this->game_menu_touch_active_)
      this->back_game_menu_();
    return true;
  }
  if (!this->game_menu_touch_active_) {
    const uint8_t row = static_cast<uint8_t>((y - top) / this->game_menu_row_height_);
    if (row < this->game_menu_item_count_) {
      const bool locked = this->display_mutex_ != nullptr &&
                          xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY) == pdTRUE;
      this->game_menu_selection_ = row;
      if (locked)
        xSemaphoreGiveRecursive(this->display_mutex_);
      if (!this->game_menu_app_page_ && (row == 3 || row == 4)) {
        const int bar_x = this->game_menu_screen_x_ + std::max(145, this->game_menu_width_ / 2);
        const int bar_width = std::max(36, this->game_menu_width_ - std::max(145, this->game_menu_width_ / 2) - 22);
        const int level = std::max(0, std::min(100, (x - bar_x) * 100 / bar_width));
        if (row == 3)
          this->set_master_volume(level);
        else
          this->set_brightness_level(level);
        this->redraw_control_overlay_now_(true);
      } else {
        this->activate_game_menu_item_();
      }
    }
    this->game_menu_touch_active_ = true;
  }
  return true;
}

void PaxLoader::redraw_control_overlay_now_(bool full) {
  if (!this->launched_ || this->display_ == nullptr)
    return;
  const int64_t now = esp_timer_get_time();
  // A GT911 sample arrives every 50 ms, but touch handling can be called more
  // often while a finger is held.  Each direct MIPI transfer is expensive;
  // cap drag feedback at 20 Hz and avoid redrawing unrelated controls.
  if (!full && now - this->control_overlay_last_redraw_us_ < 50000)
    return;
  // Touch handling can run either on ESPHome's loop task or inside the PAX
  // worker. Serialize these small direct transfers with the same display
  // mutex used by frame submission so the state change is visible immediately
  // even when the app is rendering slowly or is sitting on a static frame.
  if (this->display_mutex_ != nullptr)
    xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY);
  if (full) {
    this->draw_volume_slider_overlay_();
    this->draw_brightness_slider_overlay_();
    this->draw_close_overlay_();
    this->draw_reload_overlay_();
    this->draw_volume_overlay_();
    this->draw_brightness_overlay_();
    this->draw_game_menu_button_();
  } else {
    if (this->volume_slider_visible_)
      this->draw_volume_slider_overlay_();
    if (this->brightness_slider_visible_)
      this->draw_brightness_slider_overlay_();
  }
  if (full && this->game_menu_open_) {
    this->game_menu_frame_dirty_ = true;
    this->draw_game_menu_();
  }
  this->control_overlay_last_redraw_us_ = now;
  if (this->display_mutex_ != nullptr)
    xSemaphoreGiveRecursive(this->display_mutex_);
}

void PaxLoader::update_control_overlay_() {
  if (!this->launched_ || this->display_ == nullptr) {
    this->control_overlay_offset_ = 0;
    this->control_overlay_last_touch_us_ = 0;
    this->control_overlay_last_redraw_us_ = 0;
    this->control_overlay_wake_gesture_active_ = false;
    this->control_overlay_swipe_tracking_ = false;
    this->control_overlay_swipe_start_us_ = 0;
    this->volume_touch_active_ = false;
    this->brightness_touch_active_ = false;
    this->volume_slider_visible_ = false;
    this->brightness_slider_visible_ = false;
    return;
  }

  const int64_t now = esp_timer_get_time();
  // A newly started PAX begins with the toolbar hidden. This avoids leaving
  // controls over the first game frame and makes the swipe the only way to
  // reveal them.
  const bool idle = !this->game_menu_open_ &&
                    (this->control_overlay_last_touch_us_ == 0 ||
                     now - this->control_overlay_last_touch_us_ >= CONTROL_OVERLAY_IDLE_US);
  if (idle) {
    if (this->volume_slider_visible_ || this->brightness_slider_visible_) {
      this->volume_slider_visible_ = false;
      this->brightness_slider_visible_ = false;
      this->control_overlay_repaint_pending_ = true;
    }
  }
  // The toolbar is centered horizontally but should hide by sliding upward
  // through the top edge, not sideways into the right edge of the panel.
  const int32_t max_offset = std::max<int32_t>(0, this->geometry_.close_y() + canvas::CLOSE_SIZE);
  const int32_t target = idle ? max_offset : 0;
  // Move the toolbar in one frame.  Animating it with partial display writes
  // repeatedly exposed the black erase buffer, which looked like flashing on
  // the game image.  The overlay still auto-hides, but the transition is now
  // an atomic repaint.
  if (this->control_overlay_offset_ != target) {
    this->control_overlay_offset_ = target;
    this->control_overlay_repaint_pending_ = true;
  }
}

static bool overlay_rect_on_panel(const canvas::Geometry &geometry, int raw_x, int raw_y, int width, int height) {
  return raw_x >= 0 && raw_y >= 0 && raw_x + width <= geometry.panel_w && raw_y + height <= geometry.panel_h;
}

void PaxLoader::draw_close_overlay_() {
  if (!this->launched_ || this->display_ == nullptr)
    return;

  const int slide = this->control_overlay_offset_;
  const int raw_x = this->geometry_.close_raw_x();
  const int raw_y = this->geometry_.close_raw_y(slide);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE))
    return;

  const int left = 0;
  const int top = 0;
  const int right = left + CLOSE_BUTTON_SIZE;
  const int bottom = top + CLOSE_BUTTON_SIZE;

  // Dark panel background with an error-colored border keeps the exit action distinct.
  for (int y = top; y < bottom; y++) {
    for (int x = left; x < right; x++)
      close_overlay_buffer[y * CLOSE_BUTTON_SIZE + x] = overlay_theme::PANEL;
  }

  // Use the launcher error color for the exit border.
  for (int i = 0; i < CLOSE_BUTTON_SIZE; i++) {
    close_overlay_buffer[top * CLOSE_BUTTON_SIZE + left + i] = overlay_theme::ERROR;
    close_overlay_buffer[(bottom - 1) * CLOSE_BUTTON_SIZE + left + i] = overlay_theme::ERROR;
    close_overlay_buffer[(top + i) * CLOSE_BUTTON_SIZE + left] = overlay_theme::ERROR;
    close_overlay_buffer[(top + i) * CLOSE_BUTTON_SIZE + right - 1] = overlay_theme::ERROR;
  }

  // High-contrast X.
  for (int i = 0; i < 26; i++) {
    for (int thickness = -1; thickness <= 1; thickness++) {
      const int x1 = left + 16 + i;
      const int y1 = top + 16 + i + thickness;
      const int x2 = left + 41 - i;
      const int y2 = top + 16 + i + thickness;
      if (x1 >= left && x1 < right && y1 >= top && y1 < bottom)
        close_overlay_buffer[y1 * CLOSE_BUTTON_SIZE + x1] = overlay_theme::TEXT;
      if (x2 >= left && x2 < right && y2 >= top && y2 < bottom)
        close_overlay_buffer[y2 * CLOSE_BUTTON_SIZE + x2] = overlay_theme::TEXT;
    }
  }

  this->display_->draw_pixels_at(
      raw_x, raw_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(close_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PaxLoader::clear_close_overlay_(int slide) {
  if (this->display_ == nullptr)
    return;

  const int raw_x = this->geometry_.close_raw_x();
  const int raw_y = this->geometry_.close_raw_y(slide);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE))
    return;

  std::fill_n(close_overlay_buffer, CLOSE_BUTTON_SIZE * CLOSE_BUTTON_SIZE, 0x0000);
  this->display_->draw_pixels_at(
      raw_x, raw_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(close_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PaxLoader::draw_reload_overlay_() {
  if (!this->launched_ || this->display_ == nullptr)
    return;
  const int slide = this->control_overlay_offset_;
  const int raw_x = this->geometry_.reload_raw_x();
  const int raw_y = this->geometry_.reload_raw_y(slide);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, RELOAD_BUTTON_SIZE, RELOAD_BUTTON_SIZE))
    return;

  std::fill_n(reload_overlay_buffer, RELOAD_BUTTON_SIZE * RELOAD_BUTTON_SIZE, overlay_theme::PANEL);
  const int center_x = RELOAD_BUTTON_SIZE / 2;
  const int center_y = RELOAD_BUTTON_SIZE / 2;
  constexpr int radius = 16;
  /* A clear, single-direction refresh arrow. Draw an open ring and a compact
   * triangular head at its upper-right end; this runs only when toolbar state
   * changes, not on every emulator frame. */
  for (int degree = 42; degree <= 318; degree += 2) {
    const float radians = degree * 0.017453292519943295f;
    const int x = center_x + static_cast<int>(std::lround(radius * std::cos(radians)));
    const int y = center_y + static_cast<int>(std::lround(radius * std::sin(radians)));
    for (int thick = -1; thick <= 1; thick++) {
      for (int thick_y = -1; thick_y <= 1; thick_y++) {
        const int px = x + thick;
        const int py = y + thick_y;
        if (px >= 0 && px < RELOAD_BUTTON_SIZE && py >= 0 && py < RELOAD_BUTTON_SIZE)
          reload_overlay_buffer[py * RELOAD_BUTTON_SIZE + px] = overlay_theme::TEXT;
      }
    }
  }
  const auto draw_arrow_stroke = [this](int x0, int y0, int x1, int y1) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
      for (int thick = -1; thick <= 1; thick++) {
        for (int thick_y = -1; thick_y <= 1; thick_y++) {
          const int px = x0 + thick;
          const int py = y0 + thick_y;
          if (px >= 0 && px < RELOAD_BUTTON_SIZE && py >= 0 && py < RELOAD_BUTTON_SIZE)
            reload_overlay_buffer[py * RELOAD_BUTTON_SIZE + px] = overlay_theme::TEXT;
        }
      }
      if (x0 == x1 && y0 == y1) break;
      const int twice_error = 2 * error;
      if (twice_error >= dy) { error += dy; x0 += sx; }
      if (twice_error <= dx) { error += dx; y0 += sy; }
    }
  };
  const int tip_x = center_x + static_cast<int>(std::lround(radius * std::cos(318 * 0.017453292519943295f)));
  const int tip_y = center_y + static_cast<int>(std::lround(radius * std::sin(318 * 0.017453292519943295f)));
  draw_arrow_stroke(tip_x, tip_y, tip_x - 9, tip_y - 2);
  draw_arrow_stroke(tip_x, tip_y, tip_x - 3, tip_y - 9);
  this->display_->draw_pixels_at(raw_x, raw_y, RELOAD_BUTTON_SIZE, RELOAD_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(reload_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PaxLoader::clear_reload_overlay_(int slide) {
  if (this->display_ == nullptr)
    return;
  const int raw_x = this->geometry_.reload_raw_x();
  const int raw_y = this->geometry_.reload_raw_y(slide);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, RELOAD_BUTTON_SIZE, RELOAD_BUTTON_SIZE))
    return;
  std::fill_n(reload_overlay_buffer, RELOAD_BUTTON_SIZE * RELOAD_BUTTON_SIZE, 0x0000);
  this->display_->draw_pixels_at(raw_x, raw_y, RELOAD_BUTTON_SIZE, RELOAD_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(reload_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PaxLoader::draw_volume_overlay_() {
  if (!this->launched_ || this->display_ == nullptr)
    return;

  const int slide = this->control_overlay_offset_;
  const int raw_x = this->geometry_.volume_raw_x();
  const int raw_y = this->geometry_.volume_raw_y(slide);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, VOLUME_BUTTON_SIZE, VOLUME_BUTTON_SIZE))
    return;

  static_assert(VOLUME_BUTTON_SIZE == volume_icon::SIZE, "Volume icon must match its hit target");
  volume_icon::render(volume_overlay_buffer, this->master_volume_);

  this->display_->draw_pixels_at(
      raw_x, raw_y, VOLUME_BUTTON_SIZE, VOLUME_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(volume_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PaxLoader::draw_volume_slider_overlay_() {
  if (!this->launched_ || !this->volume_slider_visible_ || this->display_ == nullptr)
    return;

  std::fill_n(volume_slider_overlay_buffer, VOLUME_SLIDER_WIDTH * VOLUME_SLIDER_HEIGHT, overlay_theme::PANEL);
  const int track_left = 16;
  const int track_right = VOLUME_SLIDER_WIDTH - 16;
  const int track_top = 24;
  const int track_bottom = 34;
  const int level = std::max<int32_t>(0, std::min<int32_t>(100, this->master_volume_));
  // The panel's raw transfer is rotated 180 degrees by the display path. Use
  // the mirrored raw coordinate here so the visible knob follows touch: left
  // means quiet and right means loud.
  const int knob_x = track_right - (track_right - track_left) * level / 100;
  for (int y = track_top; y <= track_bottom; y++) {
    for (int x = track_left; x <= track_right; x++)
      volume_slider_overlay_buffer[y * VOLUME_SLIDER_WIDTH + x] = x >= knob_x ? overlay_theme::ACCENT : overlay_theme::MUTED;
  }
  for (int y = 17; y <= 41; y++) {
    for (int x = std::max(track_left, knob_x - 5); x <= std::min(track_right, knob_x + 5); x++)
      volume_slider_overlay_buffer[y * VOLUME_SLIDER_WIDTH + x] = overlay_theme::TEXT;
  }
  this->display_->draw_pixels_at(
      this->geometry_.volume_slider_raw_x(), this->geometry_.volume_slider_raw_y(),
      VOLUME_SLIDER_WIDTH, VOLUME_SLIDER_HEIGHT,
      reinterpret_cast<const uint8_t *>(volume_slider_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PaxLoader::clear_volume_slider_overlay_() {
  if (this->display_ == nullptr)
    return;
  std::fill_n(volume_slider_overlay_buffer, VOLUME_SLIDER_WIDTH * VOLUME_SLIDER_HEIGHT, 0x0000);
  this->display_->draw_pixels_at(
      this->geometry_.volume_slider_raw_x(), this->geometry_.volume_slider_raw_y(), VOLUME_SLIDER_WIDTH,
      VOLUME_SLIDER_HEIGHT, reinterpret_cast<const uint8_t *>(volume_slider_overlay_buffer),
      display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false);
}

void PaxLoader::draw_brightness_overlay_() {
  if (!this->launched_ || this->display_ == nullptr)
    return;

  const int slide = this->control_overlay_offset_;
  const int raw_x = this->geometry_.brightness_raw_x();
  const int raw_y = this->geometry_.brightness_raw_y(slide);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, BRIGHTNESS_BUTTON_SIZE, BRIGHTNESS_BUTTON_SIZE))
    return;

  std::fill_n(brightness_overlay_buffer, BRIGHTNESS_BUTTON_SIZE * BRIGHTNESS_BUTTON_SIZE, overlay_theme::PANEL);
  // Sun icon: a filled centre and eight short rays. Keep it deliberately
  // pixel-based so it remains available even when an app uses no LVGL font.
  for (int y = 23; y <= 35; y++) {
    for (int x = 23; x <= 35; x++) {
      const int dx = x - 29;
      const int dy = y - 29;
      if (dx * dx + dy * dy <= 30)
        brightness_overlay_buffer[y * BRIGHTNESS_BUTTON_SIZE + x] = overlay_theme::TEXT;
    }
  }
  for (int i = 0; i < 8; i++) {
    // Explicit ray coordinates make the icon symmetrical and avoid trig or
    // floating point in the display flush path.
    static constexpr int rays[8][4] = {
        {29, 11, 29, 18}, {42, 16, 37, 21}, {47, 29, 40, 29}, {42, 42, 37, 37},
        {29, 47, 29, 40}, {16, 42, 21, 37}, {11, 29, 18, 29}, {16, 16, 21, 21},
    };
    const int x0 = rays[i][0], y0 = rays[i][1], x1 = rays[i][2], y1 = rays[i][3];
    const int sx = x0 <= x1 ? 1 : -1;
    const int sy = y0 <= y1 ? 1 : -1;
    int x = x0, y = y0;
    while (true) {
      if (x >= 0 && x < BRIGHTNESS_BUTTON_SIZE && y >= 0 && y < BRIGHTNESS_BUTTON_SIZE)
        brightness_overlay_buffer[y * BRIGHTNESS_BUTTON_SIZE + x] = overlay_theme::TEXT;
      if (x == x1 && y == y1)
        break;
      if (x != x1)
        x += sx;
      if (y != y1)
        y += sy;
    }
  }

  this->display_->draw_pixels_at(
      raw_x, raw_y, BRIGHTNESS_BUTTON_SIZE,
      BRIGHTNESS_BUTTON_SIZE, reinterpret_cast<const uint8_t *>(brightness_overlay_buffer),
      display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false);
}

void PaxLoader::draw_brightness_slider_overlay_() {
  if (!this->launched_ || !this->brightness_slider_visible_ || this->display_ == nullptr)
    return;

#ifdef PAX_LOADER_USE_LVGL
  // The settings page is the source of truth when brightness was changed
  // before launching the PAX. Keep the in-app slider's knob in sync with it.
  if (this->catalog_brightness_number_ != nullptr && std::isfinite(this->catalog_brightness_number_->state))
    s_brightness = std::max<int32_t>(5, std::min<int32_t>(100, static_cast<int32_t>(this->catalog_brightness_number_->state)));
#endif
  std::fill_n(brightness_slider_overlay_buffer, BRIGHTNESS_SLIDER_WIDTH * BRIGHTNESS_SLIDER_HEIGHT, overlay_theme::PANEL);
  const int track_left = 16;
  const int track_right = BRIGHTNESS_SLIDER_WIDTH - 16;
  const int track_top = 24;
  const int track_bottom = 34;
  const int level = std::max<int32_t>(5, std::min<int32_t>(100, s_brightness));
  const int knob_x = track_right - (track_right - track_left) * level / 100;
  for (int y = track_top; y <= track_bottom; y++) {
    for (int x = track_left; x <= track_right; x++)
      brightness_slider_overlay_buffer[y * BRIGHTNESS_SLIDER_WIDTH + x] = x >= knob_x ? overlay_theme::ACCENT : overlay_theme::MUTED;
  }
  for (int y = 17; y <= 41; y++) {
    for (int x = std::max(track_left, knob_x - 5); x <= std::min(track_right, knob_x + 5); x++)
      brightness_slider_overlay_buffer[y * BRIGHTNESS_SLIDER_WIDTH + x] = overlay_theme::TEXT;
  }
  this->display_->draw_pixels_at(
      this->geometry_.brightness_slider_raw_x(), this->geometry_.brightness_slider_raw_y(),
      BRIGHTNESS_SLIDER_WIDTH, BRIGHTNESS_SLIDER_HEIGHT,
      reinterpret_cast<const uint8_t *>(brightness_slider_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PaxLoader::clear_brightness_slider_overlay_() {
  if (this->display_ == nullptr)
    return;
  std::fill_n(brightness_slider_overlay_buffer, BRIGHTNESS_SLIDER_WIDTH * BRIGHTNESS_SLIDER_HEIGHT, 0x0000);
  this->display_->draw_pixels_at(
      this->geometry_.brightness_slider_raw_x(), this->geometry_.brightness_slider_raw_y(),
      BRIGHTNESS_SLIDER_WIDTH, BRIGHTNESS_SLIDER_HEIGHT,
      reinterpret_cast<const uint8_t *>(brightness_slider_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PaxLoader::clear_volume_overlay_(int slide) {
  if (this->display_ == nullptr)
    return;

  const int raw_x = this->geometry_.volume_raw_x();
  const int raw_y = this->geometry_.volume_raw_y(slide);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, VOLUME_BUTTON_SIZE, VOLUME_BUTTON_SIZE))
    return;

  std::fill_n(volume_overlay_buffer, VOLUME_BUTTON_SIZE * VOLUME_BUTTON_SIZE, 0x0000);
  this->display_->draw_pixels_at(
      raw_x, raw_y, VOLUME_BUTTON_SIZE, VOLUME_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(volume_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PaxLoader::clear_brightness_overlay_(int slide) {
  if (this->display_ == nullptr)
    return;

  const int raw_x = this->geometry_.brightness_raw_x();
  const int raw_y = this->geometry_.brightness_raw_y(slide);
  if (!overlay_rect_on_panel(this->geometry_, raw_x, raw_y, BRIGHTNESS_BUTTON_SIZE, BRIGHTNESS_BUTTON_SIZE))
    return;

  std::fill_n(brightness_overlay_buffer, BRIGHTNESS_BUTTON_SIZE * BRIGHTNESS_BUTTON_SIZE, 0x0000);
  this->display_->draw_pixels_at(
      raw_x, raw_y, BRIGHTNESS_BUTTON_SIZE,
      BRIGHTNESS_BUTTON_SIZE, reinterpret_cast<const uint8_t *>(brightness_overlay_buffer),
      display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false);
}

void PaxLoader::poll_close_button_() {
  if (!this->launched_ || this->touchscreen_ == nullptr || this->global_close_requested_)
    return;

  auto touch = this->touchscreen_->get_touch();
  if (!touch.has_value() || (touch->state & touchscreen::STATE_RELEASING) != 0) {
    this->close_hold_since_us_ = 0;
    this->volume_touch_active_ = false;
    this->brightness_touch_active_ = false;
    this->game_menu_touch_active_ = false;
    this->control_overlay_wake_gesture_active_ = false;
    // A short tap in the wake strip is not a swipe. Always discard its
    // candidate on release so the next swipe starts with a fresh origin.
    this->control_overlay_swipe_tracking_ = false;
    this->control_overlay_swipe_start_x_ = 0;
    this->control_overlay_swipe_start_y_ = 0;
    this->control_overlay_swipe_start_us_ = 0;
    return;
  }

  // Ordinary one-finger touches belong to the game and must not wake the
  // hidden loader toolbar. Require a downward swipe in the small top-center
  // strip where the toolbar appears. A tap there is harmless and cannot hit
  // the hidden close button.
  const canvas::Geometry geometry = this->geometry_;
  const int wake_left = std::max(0, geometry.control_group_x() - 18);
  const int wake_right = std::min(geometry.panel_w, geometry.control_group_x() + geometry.control_group_width() + 18);
  const int wake_top = std::max(0, geometry.control_top_y() - 12);
  const int wake_bottom = std::min(geometry.panel_h, geometry.control_top_y() + canvas::CLOSE_SIZE + 18);
  if (this->control_overlay_offset_ > 0) {
    const int64_t now = esp_timer_get_time();
    const bool in_wake_strip = touch->x >= wake_left && touch->x < wake_right &&
                               touch->y >= wake_top && touch->y < wake_bottom;
    if (!this->control_overlay_swipe_tracking_ && in_wake_strip) {
      this->control_overlay_swipe_tracking_ = true;
      this->control_overlay_swipe_start_x_ = touch->x;
      this->control_overlay_swipe_start_y_ = touch->y;
      this->control_overlay_swipe_start_us_ = now;
    }
    if (this->control_overlay_swipe_tracking_) {
      const int dx = touch->x - this->control_overlay_swipe_start_x_;
      const int dy = touch->y - this->control_overlay_swipe_start_y_;
      const int64_t elapsed = now - this->control_overlay_swipe_start_us_;
      if (elapsed > CONTROL_OVERLAY_SWIPE_MAX_US) {
        // Do not leave a stale candidate armed forever if a finger rests in
        // the strip without completing a swipe.
        this->control_overlay_swipe_tracking_ = false;
      } else if (dy >= CONTROL_OVERLAY_SWIPE_THRESHOLD && dy >= std::abs(dx)) {
        this->control_overlay_swipe_tracking_ = false;
        this->control_overlay_wake_gesture_active_ = true;
        this->wake_control_overlay_();
        this->redraw_control_overlay_now_();
        ESP_LOGI(TAG, "PAX control overlay woken by downward toolbar swipe");
      }
      // Never let the activation swipe fall through to the PAX or to the
      // hidden close hit box.
      return;
    }
  }
  if (this->control_overlay_wake_gesture_active_)
    return;

  if (this->game_menu_touch_(touch->x, touch->y))
    return;
  if (this->volume_touch_(touch->x, touch->y))
    return;
  if (this->close_touch_(touch->x, touch->y))
    return;
  this->reload_touch_(touch->x, touch->y);
}

// A touch on the dedicated close control; true when it asked the app to close.
// The control is kept in the shared toolbar, so a tap is unambiguous and
// closes immediately rather than requiring the old over-canvas hold gesture.
bool PaxLoader::close_touch_(int x, int y) {
  if (!this->launched_ || this->control_overlay_offset_ != 0)
    return false;
  const canvas::Geometry geometry = this->geometry_;
  if (!geometry.in_close(x, y, this->control_overlay_offset_)) {
    this->close_hold_since_us_ = 0;
    return false;
  }
  this->wake_control_overlay_();
  bool overlay_state_changed = false;
  if (!this->global_close_requested_) {
    this->begin_close_();
    ESP_LOGI(TAG, "PAX on-screen close requested");
  }
  return true;
}

bool PaxLoader::reload_touch_(int x, int y) {
  if (!this->launched_ || this->control_overlay_offset_ != 0)
    return false;
  if (!this->geometry_.in_reload(x, y, this->control_overlay_offset_))
    return false;
  this->wake_control_overlay_();
  if (!this->reload_after_close_ && !this->global_close_requested_) {
    this->reload_source_ = this->running_app_source_;
    this->reload_after_close_ = !this->reload_source_.empty();
    if (this->reload_after_close_) {
      this->begin_close_();
      ESP_LOGI(TAG, "PAX on-screen reload requested: %s", this->reload_source_.c_str());
    }
  }
  return true;
}

bool PaxLoader::volume_touch_(int x, int y) {
  if (!this->launched_ || this->control_overlay_wake_gesture_active_ || this->control_overlay_swipe_tracking_)
    return false;

  const int slide = this->control_overlay_offset_;
  // Compact controls are touchable only while they are actually visible.
  // Slider hit boxes are enabled only while their slider is drawn. This keeps
  // the direct-rendered overlay from intercepting the launcher's widgets.
  const bool compact_visible = slide == 0;
  const bool in_volume_button = compact_visible && this->geometry_.in_volume(x, y, slide);
  const bool in_volume_slider = this->volume_slider_visible_ && this->geometry_.in_volume_slider(x, y);
  const bool in_brightness_button = compact_visible && this->geometry_.in_brightness(x, y, slide);
  const bool in_brightness_slider = this->brightness_slider_visible_ && this->geometry_.in_brightness_slider(x, y);
  if (!in_volume_button && !in_volume_slider && !in_brightness_button && !in_brightness_slider) {
    this->volume_touch_active_ = false;
    this->brightness_touch_active_ = false;
    if (this->volume_slider_visible_ || this->brightness_slider_visible_) {
      this->volume_slider_visible_ = false;
      this->brightness_slider_visible_ = false;
      this->control_overlay_repaint_pending_ = true;
    }
    return false;
  }
  this->wake_control_overlay_();
  bool overlay_state_changed = false;

  // The slider row is below the compact toolbar, so the close and toggle
  // buttons always remain independently touchable while a slider is shown.
  if (this->volume_slider_visible_ && in_volume_slider) {
    this->volume_touch_active_ = true;
    this->brightness_touch_active_ = false;
    const int left = this->geometry_.volume_slider_x() + 16;
    const int right = this->geometry_.volume_slider_x() + canvas::VOLUME_SLIDER_WIDTH - 16;
    const int level = right > left ? (x - left) * 100 / (right - left) : 0;
    this->set_master_volume(std::max<int32_t>(0, std::min<int32_t>(100, level)));
    this->redraw_control_overlay_now_(false);
    return true;
  }
  if (this->brightness_slider_visible_ && in_brightness_slider) {
    this->brightness_touch_active_ = true;
    this->volume_touch_active_ = false;
    const int left = this->geometry_.brightness_slider_x() + 16;
    const int right = this->geometry_.brightness_slider_x() + canvas::BRIGHTNESS_SLIDER_WIDTH - 16;
    const int level = right > left ? (x - left) * 95 / (right - left) + 5 : 5;
    this->set_brightness_level(level);
    this->redraw_control_overlay_now_(false);
    return true;
  }

  if (!this->volume_touch_active_ && !this->brightness_touch_active_) {
    if (in_brightness_button) {
      this->brightness_touch_active_ = true;
      this->brightness_slider_visible_ = !this->brightness_slider_visible_;
      this->volume_slider_visible_ = false;
      this->control_overlay_repaint_pending_ = true;
      overlay_state_changed = true;
      this->play_ui_sound(UI_SOUND_CONFIRM);
      ESP_LOGI(TAG, "PAX on-screen brightness slider %s", this->brightness_slider_visible_ ? "shown" : "hidden");
    } else if (in_volume_button) {
      this->volume_touch_active_ = true;
      this->volume_slider_visible_ = !this->volume_slider_visible_;
      this->brightness_slider_visible_ = false;
      this->control_overlay_repaint_pending_ = true;
      overlay_state_changed = true;
      this->play_ui_sound(UI_SOUND_CONFIRM);
      ESP_LOGI(TAG, "PAX on-screen volume slider %s", this->volume_slider_visible_ ? "shown" : "hidden");
    } else if (in_brightness_slider) {
      this->brightness_touch_active_ = true;
      this->brightness_slider_visible_ = true;
      this->control_overlay_repaint_pending_ = true;
    } else if (in_volume_slider) {
      this->volume_touch_active_ = true;
      this->volume_slider_visible_ = true;
      this->control_overlay_repaint_pending_ = true;
    }
  }
  if (in_volume_slider && this->volume_touch_active_) {
    const int left = this->geometry_.volume_slider_x() + 16;
    const int right = this->geometry_.volume_slider_x() + canvas::VOLUME_SLIDER_WIDTH - 16;
    const int level = right > left ? (x - left) * 100 / (right - left) : 0;
    this->set_master_volume(std::max<int32_t>(0, std::min<int32_t>(100, level)));
  } else if (in_brightness_slider && this->brightness_touch_active_) {
    const int left = this->geometry_.brightness_slider_x() + 16;
    const int right = this->geometry_.brightness_slider_x() + canvas::BRIGHTNESS_SLIDER_WIDTH - 16;
    const int level = right > left ? (x - left) * 95 / (right - left) + 5 : 5;
    this->set_brightness_level(level);
  }
  // The button/visibility path needs a complete repaint once.  A continuing
  // drag only needs the active slider, and that path is throttled above.
  if (overlay_state_changed)
    this->redraw_control_overlay_now_(true);
  else if (this->volume_slider_visible_ || this->brightness_slider_visible_)
    this->redraw_control_overlay_now_(false);
  return true;
}

// Copies the fingers on the panel for touch_read_points: the touchscreen
// component updates its list on this (the main loop's) task.
void PaxLoader::snapshot_touches_() {
  if (this->touchscreen_ == nullptr)
    return;
  const touchscreen::TouchPoints_t touches = this->touchscreen_->get_touches();
  portENTER_CRITICAL(&this->touch_points_lock_);
  int count = 0;
  for (const auto &touch : touches) {
    if (count == MAX_TOUCH_POINTS)
      break;
    if (this->control_overlay_wake_gesture_active_ || this->control_overlay_swipe_tracking_)
      break;
    if ((touch.state & touchscreen::STATE_RELEASING) == 0)
      this->touch_points_[count++] = touch;
  }
  this->touch_point_count_ = count;
  portEXIT_CRITICAL(&this->touch_points_lock_);
}

void PaxLoader::flush_framebuffer_() {
  if (this->display_ == nullptr || this->framebuffer_ == nullptr)
    return;

  this->update_control_overlay_();
  const auto dirty = this->launcher_dirty_region_;
  this->launcher_dirty_region_.clear();
  // Native-resolution launcher updates avoid full-screen presentation.
  // Retain the full path for large redraws, sliders, menus and remote viewing.
  if (pax_launcher::owns_pointer(this->running_source_) && dirty.w > 0 &&
      this->rotated_framebuffer_ != nullptr && !this->stream_client_connected_ &&
      !this->control_overlay_repaint_pending_ &&
      !this->volume_slider_visible_ && !this->brightness_slider_visible_ && !this->game_menu_open_ &&
      dirty.w * dirty.h < this->geometry_.canvas_w * this->geometry_.canvas_h / 4) {
    if (this->display_mutex_ != nullptr)
      xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY);
    const int64_t start = esp_timer_get_time();
    dirty.rotated_pixels(this->framebuffer_, this->geometry_.canvas_w, this->rotated_framebuffer_);
    const size_t bytes = canvas::sync_bytes(size_t(dirty.w) * dirty.h * sizeof(uint16_t), this->frame_alloc_bytes_);
    esp_cache_msync(this->rotated_framebuffer_, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    this->last_frame_direct_ = false;
  this->native_rgb888_frame_ = nullptr;
    ++this->pax_frame_count_;
    this->send_display_buffer_(this->rotated_framebuffer_, start,
        this->geometry_.raw_x() + this->geometry_.canvas_w - dirty.x - dirty.w,
        this->geometry_.raw_y() + this->geometry_.canvas_h - dirty.y - dirty.h,
        dirty.w, dirty.h, dirty.w);
    static uint32_t frames = 0;
    static int64_t total_us = 0;
    total_us += esp_timer_get_time() - start;
    if (++frames == 120) {
      ESP_LOGI(TAG, "Launcher dirty-region presentation: avg=%lld us last=%dx%d",
               static_cast<long long>(total_us / frames), dirty.w, dirty.h);
      frames = 0;
      total_us = 0;
    }
    if (this->display_mutex_ != nullptr)
      xSemaphoreGiveRecursive(this->display_mutex_);
    return;
  }

  this->pax_frame_count_++;
  const int64_t flush_start_us = esp_timer_get_time();

  if (this->display_mutex_ != nullptr)
    xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY);

  // Partial overlay writes modify the currently scanned buffer. Retire both
  // cached letterboxes on show/hide so the other back buffer cannot bring
  // stale toolbar pixels back on a subsequent swap.
  if (this->control_overlay_repaint_pending_) {
    this->initialized_panel_framebuffers_[0] = nullptr;
    this->initialized_panel_framebuffers_[1] = nullptr;
  }

  const int64_t transform_start_us = esp_timer_get_time();
  const uint16_t *display_buffer = this->rotated_framebuffer_;
  this->last_frame_direct_ = false;
  this->native_rgb888_frame_ = nullptr;
  bool ppa_ok = false;
  bool panel_frame_buffer = false;
  uint16_t *panel_back_buffer = nullptr;
  const int canvas_w = this->geometry_.canvas_w;
  const int canvas_h = this->geometry_.canvas_h;
  const int panel_w = this->geometry_.panel_w;
  const int panel_h = this->geometry_.panel_h;
  esp_err_t ppa_result = ESP_ERR_INVALID_STATE;
  int64_t ppa_us = 0;
  int64_t input_cache_us = 0;
  int64_t output_cache_us = 0;
  int64_t panel_init_us = 0;
  int64_t fallback_cache_us = 0;
  int64_t cpu_rotation_us = 0;
  bool ppa_attempted = false;
  if (this->ppa_srm_client_ != nullptr && this->ppa_framebuffer_ != nullptr && this->mipi_dsi_ != nullptr &&
      ((canvas_w == panel_w && canvas_h == panel_h) ||
       (this->control_overlay_offset_ != 0 && !this->volume_slider_visible_ &&
        !this->brightness_slider_visible_ && !this->game_menu_open_)) &&
      this->mipi_dsi_->get_native_width() == panel_w && this->mipi_dsi_->get_native_height() == panel_h &&
      this->geometry_.raw_x() >= 0 && this->geometry_.raw_y() >= 0 &&
      this->geometry_.raw_x() + canvas_w <= panel_w && this->geometry_.raw_y() + canvas_h <= panel_h) {
    panel_frame_buffer = this->mipi_dsi_->get_back_frame_buffer(&panel_back_buffer);
  }
  if (panel_frame_buffer) {
    static bool logged_panel_frame_buffer = false;
    if (!logged_panel_frame_buffer) {
      ESP_LOGI(TAG, "PAX direct DSI framebuffer path active (%dx%d panel, %dx%d canvas)", panel_w, panel_h,
               canvas_w, canvas_h);
      logged_panel_frame_buffer = true;
    }
  }
  if (this->ppa_srm_client_ != nullptr && this->ppa_framebuffer_ != nullptr) {
    const size_t input_bytes =
        canvas::sync_bytes(canvas::frame_bytes(canvas_w, canvas_h), this->frame_alloc_bytes_);
    const int64_t input_cache_start_us = esp_timer_get_time();
    esp_cache_msync(this->framebuffer_, input_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    input_cache_us = esp_timer_get_time() - input_cache_start_us;
    uint16_t *ppa_output = this->ppa_framebuffer_;
    size_t output_bytes = input_bytes;
    int output_w = canvas_w;
    int output_h = canvas_h;
    int output_x = 0;
    int output_y = 0;
    if (panel_frame_buffer) {
      // Keep the letterbox black, but only initialize each DSI back buffer
      // once. Both scanout buffers are reused and PPA overwrites the entire
      // game rectangle every frame; clearing 1.2 MB of PSRAM on every frame
      // was adding roughly 50-60 ms to the display path.
      output_bytes = canvas::frame_bytes(panel_w, panel_h);
      ppa_output = panel_back_buffer;
      output_w = panel_w;
      output_h = panel_h;
      output_x = this->geometry_.raw_x();
      output_y = this->geometry_.raw_y();
      bool initialized = false;
      int free_slot = -1;
      for (int i = 0; i < 2; ++i) {
        if (this->initialized_panel_framebuffers_[i] == ppa_output) {
          initialized = true;
          break;
        }
        if (this->initialized_panel_framebuffers_[i] == nullptr && free_slot < 0)
          free_slot = i;
      }
      if (!initialized) {
        const int64_t panel_init_start_us = esp_timer_get_time();
        std::memset(ppa_output, 0, output_bytes);
        esp_cache_msync(ppa_output, output_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        panel_init_us = esp_timer_get_time() - panel_init_start_us;
        if (free_slot >= 0)
          this->initialized_panel_framebuffers_[free_slot] = ppa_output;
        ESP_LOGI(TAG, "PAX initialized DSI back buffer %d (%zu bytes) in %lld us", free_slot,
                 output_bytes, static_cast<long long>(panel_init_us));
      }
    }
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = this->framebuffer_,
            .pic_w = static_cast<uint32_t>(canvas_w), .pic_h = static_cast<uint32_t>(canvas_h),
            .block_w = static_cast<uint32_t>(canvas_w), .block_h = static_cast<uint32_t>(canvas_h),
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = ppa_output, .buffer_size = output_bytes,
            .pic_w = static_cast<uint32_t>(output_w), .pic_h = static_cast<uint32_t>(output_h),
            .block_offset_x = static_cast<uint32_t>(output_x),
            .block_offset_y = static_cast<uint32_t>(output_y), .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_180,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mirror_x = false, .mirror_y = false,
        .rgb_swap = false, .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_attempted = true;
    const int64_t ppa_start_us = esp_timer_get_time();
    ppa_result = ppa_do_scale_rotate_mirror(reinterpret_cast<ppa_client_handle_t>(this->ppa_srm_client_), &cfg);
    ppa_us = esp_timer_get_time() - ppa_start_us;
    if (ppa_result == ESP_OK) {
      // PPA wrote the rotated pixels behind the CPU cache. Invalidate/update
      // the CPU view before the display driver reads the buffer. DIR_C2M here
      // pushed stale cache lines back over the fresh PPA output, which showed
      // up as intermittent full-screen green flashes in every PAX.
      const int64_t output_cache_start_us = esp_timer_get_time();
      esp_cache_msync(ppa_output, output_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
      output_cache_us = esp_timer_get_time() - output_cache_start_us;
      display_buffer = ppa_output;
      ppa_ok = true;
    }
  }
  if (!ppa_ok) {
    // Fallback for boards/IDF builds without a working PPA SRM client.
    const int64_t cpu_start_us = esp_timer_get_time();
    for (int y = 0; y < canvas_h; y++) {
      const uint16_t *src = this->framebuffer_ + static_cast<size_t>(y) * canvas_w;
      uint16_t *dst = this->rotated_framebuffer_ + static_cast<size_t>(canvas_h - 1 - y) * canvas_w;
      for (int x = 0; x < canvas_w; x++)
        dst[canvas_w - 1 - x] = src[x];
    }
    cpu_rotation_us = esp_timer_get_time() - cpu_start_us;
    // The CPU wrote this PSRAM buffer. Make those writes visible to the DSI
    // DMA before submitting it; without the writeback, fallback frames could
    // contain stale/cache-line fragments (seen as brief cyan/white flashes).
    const size_t rotated_bytes =
        canvas::sync_bytes(canvas::frame_bytes(canvas_w, canvas_h), this->frame_alloc_bytes_);
    const int64_t fallback_cache_start_us = esp_timer_get_time();
    esp_cache_msync(this->rotated_framebuffer_, rotated_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    fallback_cache_us = esp_timer_get_time() - fallback_cache_start_us;
    this->direct_frame_[2] = 0;  // the direct path must clear its border again
    static bool logged_unavailable = false;
    static int64_t last_ppa_error_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (!ppa_attempted && !logged_unavailable) {
      ESP_LOGW(TAG, "PAX PPA rotation unavailable; using cache-synced CPU fallback");
      logged_unavailable = true;
    } else if (ppa_attempted && now_us - last_ppa_error_us >= 5000000) {
      ESP_LOGW(TAG, "PAX PPA rotation failed (%s); using cache-synced CPU fallback",
               esp_err_to_name(ppa_result));
      last_ppa_error_us = now_us;
    }
  }
  const int64_t transform_us = esp_timer_get_time() - transform_start_us;
  if (ppa_ok && panel_frame_buffer) {
    this->send_display_buffer_(display_buffer, flush_start_us, 0, 0, panel_w, panel_h, panel_w, true);
  } else {
    this->send_display_buffer_(display_buffer, flush_start_us);
  }
  const int64_t total_flush_us = esp_timer_get_time() - flush_start_us;

  // Keep frame-transfer profiling separate from the app's own render timing.
  // DSI draw_bitmap waits for the panel transfer callback, so this tells us
  // whether time is going into PPA/rotation or the physical display transfer.
  static uint32_t profile_frames = 0;
  static int64_t transform_total_us = 0;
  static int64_t ppa_total_us = 0;
  static int64_t input_cache_total_us = 0;
  static int64_t output_cache_total_us = 0;
  static int64_t panel_init_total_us = 0;
  static int64_t fallback_cache_total_us = 0;
  static int64_t cpu_rotation_total_us = 0;
  static int64_t dsi_total_us = 0;
  static int64_t flush_total_us = 0;
  ++profile_frames;
  transform_total_us += transform_us;
  ppa_total_us += ppa_us;
  input_cache_total_us += input_cache_us;
  output_cache_total_us += output_cache_us;
  panel_init_total_us += panel_init_us;
  fallback_cache_total_us += fallback_cache_us;
  cpu_rotation_total_us += cpu_rotation_us;
  dsi_total_us += s_pax_last_dsi_transfer_us;
  flush_total_us += total_flush_us;
  if (profile_frames >= 300) {
    ESP_LOGI(TAG, "PAX frame timing (%dx%d): transform=%lld us (input cache=%lld, PPA=%lld, "
                  "output cache=%lld, panel init=%lld, fallback cache=%lld, CPU rotate=%lld) "
                  "DSI=%lld us total=%lld us avg over %u frames",
             canvas_w, canvas_h, static_cast<long long>(transform_total_us / profile_frames),
             static_cast<long long>(input_cache_total_us / profile_frames),
             static_cast<long long>(ppa_total_us / profile_frames),
             static_cast<long long>(output_cache_total_us / profile_frames),
             static_cast<long long>(panel_init_total_us / profile_frames),
             static_cast<long long>(fallback_cache_total_us / profile_frames),
             static_cast<long long>(cpu_rotation_total_us / profile_frames),
             static_cast<long long>(dsi_total_us / profile_frames),
             static_cast<long long>(flush_total_us / profile_frames), static_cast<unsigned>(profile_frames));
    profile_frames = 0;
    transform_total_us = ppa_total_us = input_cache_total_us = output_cache_total_us = panel_init_total_us =
        fallback_cache_total_us = cpu_rotation_total_us = dsi_total_us = flush_total_us = 0;
  }

  if (this->display_mutex_ != nullptr)
    xSemaphoreGiveRecursive(this->display_mutex_);
}

void PaxLoader::send_display_buffer_(const uint16_t *display_buffer, int64_t flush_start_us, int raw_x, int raw_y,
                                      int width, int height, int source_stride, bool panel_frame_buffer) {
  // One motion sample per second is enough for remote diagnosis and keeps the
  // raw video traffic small enough that it cannot starve the audio stream.
  static int64_t last_stream_frame_us = 0;
  const canvas::Geometry geometry = this->geometry_;
  if (raw_x < 0)
    raw_x = geometry.raw_x();
  if (raw_y < 0)
    raw_y = geometry.raw_y();
  if (width < 0)
    width = geometry.canvas_w;
  if (height < 0)
    height = geometry.canvas_h;
  if (source_stride < 0)
    source_stride = width;
  if (this->stream_client_connected_ && this->stream_mutex_ != nullptr && display_buffer != nullptr &&
      flush_start_us - last_stream_frame_us >= 1000000) {
    if (xSemaphoreTake(this->stream_mutex_, 0) == pdTRUE) {
      // A 100x60 sample of the panel-oriented canvas. Compact frames occupy
      // only the visible rectangle; full-panel framebuffer frames are already
      // laid out in panel coordinates.
      const int region_x = raw_x - geometry.raw_x();
      const int region_y = raw_y - geometry.raw_y();
      for (int y = 0; y < SCREEN_STREAM_HEIGHT; y++) {
        uint16_t *target = this->stream_framebuffer_ + y * SCREEN_STREAM_WIDTH;
        const int sample_y = y * (panel_frame_buffer ? geometry.panel_h : geometry.canvas_h) / SCREEN_STREAM_HEIGHT;
        for (int x = 0; x < SCREEN_STREAM_WIDTH; x++) {
          const int sample_x = x * (panel_frame_buffer ? geometry.panel_w : geometry.canvas_w) / SCREEN_STREAM_WIDTH;
          if (panel_frame_buffer) {
            target[x] = display_buffer[static_cast<size_t>(sample_y) * geometry.panel_w + sample_x];
          } else if (sample_x >= region_x && sample_x < region_x + width && sample_y >= region_y &&
              sample_y < region_y + height) {
            target[x] = display_buffer[static_cast<size_t>(sample_y - region_y) * source_stride +
                                       static_cast<size_t>(sample_x - region_x)];
          } else {
            target[x] = 0;
          }
        }
      }
      this->stream_frame_sequence_++;
      this->stream_frame_ready_ = true;
      last_stream_frame_us = flush_start_us;
      xSemaphoreGive(this->stream_mutex_);
    }
  }
  // Never erase the old overlay with a black buffer before the game frame is
  // transferred. On the P4 the game may use a partial MIPI transfer, making
  // that erase visible as a black flash or rectangle. A pending state change
  // forces the next render through the full-canvas path below, which replaces
  // every slider pixel from the game frame before the new controls are drawn.
  this->update_control_overlay_();
  const bool overlay_repaint_pending = this->control_overlay_repaint_pending_;

  // The frame is turned 180 degrees, so it goes to the canvas's mirrored
  // corner: (112, 60) for 800x480 on 1024x600, (0, 0) for a full-panel canvas.
  const int64_t dsi_start_us = esp_timer_get_time();
  if (panel_frame_buffer) {
    this->display_->draw_pixels_at(0, 0, geometry.panel_w, geometry.panel_h,
                                   reinterpret_cast<const uint8_t *>(display_buffer),
                                   display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false);
  } else {
    this->display_->draw_pixels_at(raw_x, raw_y, width, height,
                                   reinterpret_cast<const uint8_t *>(display_buffer),
                                   display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false);
  }
  s_pax_last_dsi_transfer_us = esp_timer_get_time() - dsi_start_us;
  if (this->launched_ && overlay_repaint_pending && this->control_overlay_rendered_) {
    // The compact toolbar can sit in the letterbox above a legacy 800x480
    // canvas. A full canvas repaint cannot cover that margin, so clear only
    // those old compact controls after the game frame—not before it. Slider
    // rectangles are deliberately not cleared: they are inside the canvas
    // and have already been replaced by the full frame.
    const int old_slide = this->control_overlay_rendered_offset_;
    const auto inside_canvas = [this](int x, int y, int w, int h) {
      return x >= this->geometry_.x() && y >= this->geometry_.y() &&
             x + w <= this->geometry_.x() + this->geometry_.canvas_w &&
             y + h <= this->geometry_.y() + this->geometry_.canvas_h;
    };
    if (!inside_canvas(this->geometry_.close_x(), this->geometry_.close_y(old_slide), CLOSE_BUTTON_SIZE,
                       CLOSE_BUTTON_SIZE))
      this->clear_close_overlay_(old_slide);
    if (!inside_canvas(this->geometry_.reload_x(), this->geometry_.reload_y(old_slide), RELOAD_BUTTON_SIZE,
                       RELOAD_BUTTON_SIZE))
      this->clear_reload_overlay_(old_slide);
    if (!inside_canvas(this->geometry_.volume_x(), this->geometry_.volume_y(old_slide), VOLUME_BUTTON_SIZE,
                       VOLUME_BUTTON_SIZE))
      this->clear_volume_overlay_(old_slide);
    if (!inside_canvas(this->geometry_.brightness_x(), this->geometry_.brightness_y(old_slide),
                       BRIGHTNESS_BUTTON_SIZE, BRIGHTNESS_BUTTON_SIZE))
      this->clear_brightness_overlay_(old_slide);
    if (!inside_canvas(this->geometry_.game_menu_x(), this->geometry_.game_menu_y(old_slide),
                       GAME_MENU_BUTTON_SIZE, GAME_MENU_BUTTON_SIZE))
      this->clear_game_menu_button_(old_slide);
  }
  // The compact toolbar is normally in the letterbox above a legacy 800x480
  // app canvas. In that layout the game frame cannot overwrite it, so sending
  // the three small MIPI transfers after every emulator frame only steals time
  // from the app and its audio. Redraw it on state changes only. A full-panel
  // app is different: its frame covers the toolbar, so retain the per-frame
  // redraw there. Sliders are below the toolbar and overlap the canvas when
  // visible, so they still need to be refreshed every frame.
  if (this->launched_) {
    const int compact_y = this->geometry_.close_y(this->control_overlay_offset_);
    const int presented_y = this->geometry_.panel_h - raw_y - height;
    const bool compact_overlaps_canvas =
        compact_y < presented_y + height && compact_y + CLOSE_BUTTON_SIZE > presented_y;
    const bool compact_needs_draw = !this->control_overlay_rendered_ || overlay_repaint_pending ||
                                    compact_overlaps_canvas || panel_frame_buffer;
    if (this->volume_slider_visible_)
      this->draw_volume_slider_overlay_();
    if (this->brightness_slider_visible_)
      this->draw_brightness_slider_overlay_();
    if (compact_needs_draw) {
      // Draw the compact controls last so a slider can never cover the close
      // button or either toggle, even if a future layout brings their regions
      // close together.
      this->draw_close_overlay_();
      this->draw_reload_overlay_();
      this->draw_volume_overlay_();
      this->draw_brightness_overlay_();
      this->draw_game_menu_button_();
    }
    if (this->game_menu_open_)
      this->draw_game_menu_();
    this->control_overlay_rendered_offset_ = this->control_overlay_offset_;
    this->control_overlay_rendered_ = true;
  } else {
    this->control_overlay_rendered_ = false;
  }
  this->control_overlay_repaint_pending_ = false;

  const int64_t flush_us = esp_timer_get_time() - flush_start_us;
  static uint32_t flush_frames = 0;
  static int64_t flush_total_us = 0;
  static int64_t flush_worst_us = 0;
  ++flush_frames;
  flush_total_us += flush_us;
  flush_worst_us = std::max(flush_worst_us, flush_us);
  if (flush_frames == 1800) {
    ESP_LOGI(TAG, "PAX video flush: avg=%lld us worst=%lld us last=%lld us",
             static_cast<long long>(flush_total_us / flush_frames),
             static_cast<long long>(flush_worst_us), static_cast<long long>(flush_us));
    flush_frames = 0;
    flush_total_us = 0;
    flush_worst_us = 0;
  }
}

void PaxLoader::screen_stream_task_entry_(void *arg) {
  auto *loader = static_cast<PaxLoader *>(arg);
  if (loader != nullptr)
    loader->screen_stream_task_();
  vTaskDelete(nullptr);
}

void PaxLoader::screen_stream_task_() {
  const int server_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (server_fd < 0)
    return;
  int reuse = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(SCREEN_STREAM_PORT);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(server_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 || listen(server_fd, 1) < 0) {
    close(server_fd);
    return;
  }
  timeval accept_timeout{1, 0};
  setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout, sizeof(accept_timeout));
  while (true) {
    if (!this->stream_enabled_) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    sockaddr_in peer{};
    socklen_t peer_length = sizeof(peer);
    const int client_fd = accept(server_fd, reinterpret_cast<sockaddr *>(&peer), &peer_length);
    if (client_fd < 0)
      continue;
    // A full 800x480 RGB565 frame is 768 KB (a 1024x600 canvas 1.2 MB). Allow
    // slower hosts and TCP back-pressure to drain it without truncating the
    // diagnostic frame.
    timeval send_timeout{5, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    this->stream_client_connected_ = true;
    this->stream_frame_ready_ = false;
    this->stream_audio_read_ = 0;
    this->stream_audio_write_ = 0;
    this->stream_audio_available_ = 0;
    ESP_LOGI(TAG, "Diagnostic screen recorder connected");
    bool connected = true;
    while (connected) {
      // A recorder that has gone away is only noticed when a send fails, and
      // with no app running nothing is sent: the next screenshot or file would
      // go to the dead socket while the new client waits. Check for its close.
      char probe;
      const int peeked = recv(client_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
      if (peeked == 0 || (peeked < 0 && errno != EWOULDBLOCK && errno != EAGAIN))
        break;
      if (this->screenshot_requested_) {
        this->screenshot_requested_ = false;
        if (!this->send_screenshot_(client_fd))
          break;
      }
      if (this->file_requested_) {
        this->file_requested_ = false;
        if (!this->send_file_(client_fd))
          break;
      }
      if (this->stream_mutex_ == nullptr || xSemaphoreTake(this->stream_mutex_, pdMS_TO_TICKS(250)) != pdTRUE)
        continue;
      const bool ready = this->stream_frame_ready_;
      const uint32_t sequence = this->stream_frame_sequence_;
      size_t audio_chunk = 0;
      if (ready) {
        std::memcpy(this->stream_frame_packet_, this->stream_framebuffer_, SCREEN_STREAM_FRAME_BYTES);
        this->stream_frame_ready_ = false;
      }
      if (this->stream_audio_available_ != 0) {
        audio_chunk = std::min<size_t>(this->stream_audio_available_, SCREEN_STREAM_AUDIO_PACKET_BYTES);
        const size_t first = std::min(audio_chunk, SCREEN_STREAM_AUDIO_BYTES - this->stream_audio_read_);
        std::memcpy(this->stream_audio_packet_, this->stream_audio_buffer_ + this->stream_audio_read_, first);
        if (first < audio_chunk)
          std::memcpy(this->stream_audio_packet_ + first, this->stream_audio_buffer_, audio_chunk - first);
        this->stream_audio_read_ = (this->stream_audio_read_ + audio_chunk) % SCREEN_STREAM_AUDIO_BYTES;
        this->stream_audio_available_ -= audio_chunk;
      }
      xSemaphoreGive(this->stream_mutex_);
      if (ready) {
        ScreenStreamHeader header{{'P','A','P','P','F','B','0','1'}, SCREEN_STREAM_WIDTH,
                                  SCREEN_STREAM_HEIGHT, static_cast<uint32_t>(SCREEN_STREAM_FRAME_BYTES), sequence};
        connected = send_screen_stream_bytes(client_fd, &header, sizeof(header)) &&
                    send_screen_stream_bytes(client_fd, this->stream_frame_packet_, SCREEN_STREAM_FRAME_BYTES);
      }
      if (connected && audio_chunk != 0) {
        ScreenStreamAudioHeader audio_header{{'P','A','P','P','A','U','0','1'},
                                             static_cast<uint32_t>(this->audio_sample_rate_ / 2), 1, 16,
                                             static_cast<uint32_t>(audio_chunk), sequence};
        connected = send_screen_stream_bytes(client_fd, &audio_header, sizeof(audio_header)) &&
                    send_screen_stream_bytes(client_fd, this->stream_audio_packet_, audio_chunk);
      }
      if (!connected)
        break;
      vTaskDelay(pdMS_TO_TICKS(SCREEN_STREAM_INTERVAL_MS));
    }
    this->stream_client_connected_ = false;
    // A request that arrived for a client that had already gone waits for the next one.
    this->stream_enabled_ = this->screenshot_requested_ || this->file_requested_;
    close(client_fd);
    ESP_LOGI(TAG, "Diagnostic screen recorder disconnected");
  }
}

// Sends one PAXSS01 packet: the app's own canvas (800x480 unless it chose
// another size; logical orientation, the same RGB565 layout as the PAXFB01
// thumbnails), the menu, or an empty 0x0 packet. The copy lives in PSRAM only
// while it is sent.
bool PaxLoader::capture_screenshot(uint16_t *&copy, uint16_t &width, uint16_t &height) {
  copy = nullptr; width = height = 0;
  if (this->screenshot_capture_lock_.test_and_set()) return false;
  struct CaptureGuard { std::atomic_flag &flag; ~CaptureGuard() { flag.clear(); } } guard{this->screenshot_capture_lock_};
  if (this->launched_ && !this->pax_loading_ && this->framebuffer_ != nullptr) {
    // The app draws without this lock; holding it keeps a flush (and its
    // rotation into the panel buffer) or a canvas switch from running during
    // the copy.
    const bool locked = this->display_mutex_ != nullptr &&
                        xSemaphoreTakeRecursive(this->display_mutex_, pdMS_TO_TICKS(200)) == pdTRUE;
    if (this->display_mutex_ != nullptr && !locked)
      return false;
    const int canvas_w = this->geometry_.canvas_w;
    const int canvas_h = this->geometry_.canvas_h;
    const size_t frame_bytes = canvas::frame_bytes(canvas_w, canvas_h);
    copy = static_cast<uint16_t *>(heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy != nullptr) {
      if (this->native_rgb888_frame_ != nullptr) {
        const int left = this->geometry_.x(), top = this->geometry_.y();
        for (int y = 0; y < canvas_h; ++y) {
          const uint8_t *row = this->native_rgb888_frame_ +
              (size_t(y + top) * this->geometry_.panel_w + left) * 3;
          for (int x = 0; x < canvas_w; ++x)
            copy[size_t(y) * canvas_w + x] = uint16_t((row[x * 3 + 2] >> 3) << 11) |
                uint16_t((row[x * 3 + 1] >> 2) << 5) | (row[x * 3] >> 3);
        }
      } else if (this->last_frame_direct_) {
        // The direct PPA path keeps only the turned/rotated visible rectangle
        // in ppa_framebuffer_. Rebuild the logical canvas for screenshots,
        // including its black letterbox, without reading outside the compact
        // buffer.
        std::memset(copy, 0, frame_bytes);
        const int x0 = this->direct_frame_[0];
        const int y0 = this->direct_frame_[1];
        const int out_w = this->direct_frame_[2];
        const int out_h = this->direct_frame_[3];
        const uint16_t *source = this->ppa_framebuffer_;
        for (int y = 0; y < out_h; y++) {
          uint16_t *target = copy + static_cast<size_t>(y0 + y) * canvas_w + x0;
          const uint16_t *source_row = source + static_cast<size_t>(out_h - 1 - y) * out_w;
          for (int x = 0; x < out_w; x++)
            target[x] = source_row[out_w - 1 - x];
        }
      } else {
        std::memcpy(copy, this->framebuffer_, frame_bytes);
      }
      if (this->game_menu_open_ && this->game_menu_frame_ != nullptr) {
        if (this->game_menu_frame_dirty_)
          this->build_game_menu_frame_();
        const int left = this->game_menu_screen_x_ - this->geometry_.x();
        const int top = this->game_menu_screen_y_ - this->geometry_.y();
        for (int y = 0; y < this->game_menu_height_; ++y) {
          for (int x = 0; x < this->game_menu_width_; ++x) {
            if (left + x >= 0 && left + x < canvas_w && top + y >= 0 && top + y < canvas_h)
              copy[(top + y) * canvas_w + left + x] =
                  this->game_menu_frame_[(this->game_menu_height_ - 1 - y) * GAME_MENU_FRAME_WIDTH +
                                         this->game_menu_width_ - 1 - x];
          }
        }
      }
      width = static_cast<uint16_t>(canvas_w);
      height = static_cast<uint16_t>(canvas_h);
    }
    if (locked)
      xSemaphoreGiveRecursive(this->display_mutex_);
  }
#if defined(PAX_LOADER_USE_LVGL) && defined(LV_USE_SNAPSHOT) && LV_USE_SNAPSHOT
  if (copy == nullptr && !this->launched_) {
    // No app: the main loop snapshots the menu; wait for it briefly. A shot
    // that arrived after an earlier request gave up is stale: drop it first.
    if (this->menu_shot_ready_) {
      heap_caps_free(this->menu_shot_);
      this->menu_shot_ = nullptr;
      this->menu_shot_ready_ = false;
    }
    this->menu_shot_wanted_ = true;
    for (int waited = 0; !this->menu_shot_ready_ && waited < 1500; waited += 20)
      vTaskDelay(pdMS_TO_TICKS(20));
    if (this->menu_shot_ready_) {
      copy = this->menu_shot_;
      width = this->menu_shot_w_;
      height = this->menu_shot_h_;
      this->menu_shot_ = nullptr;
      this->menu_shot_ready_ = false;
    }
  }
#endif
  return copy != nullptr && width != 0 && height != 0;
}

bool PaxLoader::send_screenshot_(int client_fd) {
  static uint32_t screenshot_sequence = 0;
  uint16_t *copy = nullptr, width = 0, height = 0;
  this->capture_screenshot(copy, width, height);
  const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint16_t);
  ScreenStreamHeader header{{'P','A','P','P','S','S','0','1'}, width, height, static_cast<uint32_t>(bytes),
                            ++screenshot_sequence};
  bool ok = send_screen_stream_bytes(client_fd, &header, sizeof(header));
  if (ok && copy != nullptr)
    ok = send_screen_stream_bytes(client_fd, copy, bytes);
  if (copy != nullptr) {
    ESP_LOGI(TAG, "Screenshot %s: %ux%u", ok ? "sent" : "transfer failed",
             static_cast<unsigned>(width), static_cast<unsigned>(height));
    heap_caps_free(copy);
  } else {
    ESP_LOGI(TAG, "Screenshot requested, but no PAX is running (menu screenshots need LV_USE_SNAPSHOT)");
  }
  return ok;
}

void PaxLoader::request_file(const std::string &path) {
  if (path.size() >= sizeof(this->file_request_path_)) {
    ESP_LOGW(TAG, "File request path is too long");
    return;
  }
  std::memcpy(this->file_request_path_, path.c_str(), path.size() + 1);
  this->file_requested_ = true;
  this->stream_enabled_ = true;
  ESP_LOGI(TAG, "File requested remotely: %s", this->file_request_path_);
}

static constexpr size_t WRITE_FILE_MAX_BYTES = 16 * 1024;

// Text files only: a written .pax or firmware image would be code. Deleting
// also takes leftovers (.bak, .log), but never game data or apps.
static bool writable_extension(const std::string &path, bool deleting = false) {
  static const char *const allowed[] = {".ini", ".cfg", ".conf", ".txt", ".json", ".yaml", ".yml", ".csv", ".rom"};
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos || path.find('/', dot) != std::string::npos)
    return false;
  std::string ext = path.substr(dot);
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char *candidate : allowed) {
    if (ext == candidate)
      return true;
  }
  return deleting && (ext == ".bak" || ext == ".log");
}

void PaxLoader::delete_file(const std::string &path) {
  if (path.rfind("/sd/", 0) != 0 || path.find("..") != std::string::npos || path.back() == '/' ||
      path.size() >= sizeof(this->file_request_path_) || !writable_extension(path, true)) {
    ESP_LOGW(TAG, "deletefile refused: %s is not a text or leftover file under /sd/", path.c_str());
    return;
  }
  if (this->launched_ || this->pax_loading_ || this->install_task_handle_ != nullptr || this->web_storage_busy_) {
    ESP_LOGW(TAG, "deletefile refused while an app is running (it may still use %s)", path.c_str());
    return;
  }
  const std::string local = runtime_path(path.c_str());
  struct stat info {};
  if (stat(local.c_str(), &info) != 0) {
    ESP_LOGW(TAG, "deletefile: %s does not exist", path.c_str());
    return;
  }
  if (!S_ISREG(info.st_mode)) {
    ESP_LOGW(TAG, "deletefile refused: %s is not a regular file", path.c_str());
    return;
  }
  if (unlink(local.c_str()) != 0) {
    ESP_LOGW(TAG, "deletefile: removing %s failed (errno %d)", path.c_str(), errno);
    return;
  }
  ESP_LOGI(TAG, "deletefile: removed %s (%u bytes)", path.c_str(), static_cast<unsigned>(info.st_size));
}

void PaxLoader::write_file(const std::string &path, const std::string &data) {
  if (path.rfind("/sd/", 0) != 0 || path.find("..") != std::string::npos || path.back() == '/' ||
      path.size() >= sizeof(this->file_request_path_) || !writable_extension(path)) {
    ESP_LOGW(TAG, "writefile refused: %s is not a text file under /sd/", path.c_str());
    return;
  }
  if (data.size() > WRITE_FILE_MAX_BYTES || data.find('\0') != std::string::npos) {
    ESP_LOGW(TAG, "writefile refused: %u bytes (up to %u bytes of text)", static_cast<unsigned>(data.size()),
             static_cast<unsigned>(WRITE_FILE_MAX_BYTES));
    return;
  }
  if (this->launched_ || this->pax_loading_ || this->install_task_handle_ != nullptr || this->web_storage_busy_) {
    ESP_LOGW(TAG, "writefile refused while an app is running (it may rewrite %s on exit)", path.c_str());
    return;
  }
  const std::string local = runtime_path(path.c_str());
  const std::string backup = local + ".bak";
  const std::string staging = local + ".tmp";
  recover_staged_file_(local, ".bak");
  struct stat info {};
  const bool existed = stat(local.c_str(), &info) == 0;
  if (existed) {
    if (!S_ISREG(info.st_mode)) {
      ESP_LOGW(TAG, "writefile refused: %s is not a regular file", path.c_str());
      return;
    }
  }
  std::remove(staging.c_str());
  FILE *file = std::fopen(staging.c_str(), "wb");
  bool ok = file != nullptr;
  if (ok) {
    ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    if (ok)
      ok = sync_file_(file, staging.c_str());
    ok = std::fclose(file) == 0 && ok;
  }
  if (!ok) {
    ESP_LOGW(TAG, "writefile: writing %s failed (errno %d)", path.c_str(), errno);
    std::remove(staging.c_str());
    return;
  }
  if (existed) {
    std::remove(backup.c_str());
    if (std::rename(local.c_str(), backup.c_str()) != 0) {
      ESP_LOGW(TAG, "writefile: could not stage the old %s (errno %d)", path.c_str(), errno);
      std::remove(staging.c_str());
      return;
    }
  }
  if (std::rename(staging.c_str(), local.c_str()) != 0) {
    ESP_LOGW(TAG, "writefile: could not commit %s (errno %d)", path.c_str(), errno);
    if (existed && std::rename(backup.c_str(), local.c_str()) != 0)
      ESP_LOGE(TAG, "writefile: could not restore %s (errno %d)", path.c_str(), errno);
    std::remove(staging.c_str());
    return;
  }
  warn_if_not_durable_(local);
  ESP_LOGI(TAG, "writefile: wrote %u bytes to %s%s", static_cast<unsigned>(data.size()), path.c_str(),
           existed ? " (old copy kept as .bak)" : "");
}

// Sends one PAXFL01 packet for the requested /sd/ path: the file's bytes, or
// a listing when the path ends in '/'. Anything else gets a status and no data.
bool PaxLoader::send_file_(int client_fd) {
  static uint32_t file_sequence = 0;
  char requested[sizeof(this->file_request_path_)];
  std::memcpy(requested, this->file_request_path_, sizeof(requested));
  requested[sizeof(requested) - 1] = '\0';
  const std::string path(requested);
  const std::string local = runtime_path(requested);

  uint32_t status = FILE_OK;
  uint8_t *content = nullptr;
  size_t size = 0;
  std::string listing;
  if (path.rfind("/sd/", 0) != 0 || path.find("..") != std::string::npos) {
    status = FILE_BAD_PATH;
  } else if (path.back() == '/') {
    DIR *dir = opendir(local.c_str());
    if (dir == nullptr) {
      status = FILE_NOT_FOUND;
    } else {
      while (dirent *entry = readdir(dir)) {
        struct stat info {};
        const bool known = stat((local + entry->d_name).c_str(), &info) == 0;
        listing += entry->d_name;
        if (known && S_ISDIR(info.st_mode))
          listing += '/';
        listing += '\t';
        listing += std::to_string(known ? static_cast<long long>(info.st_size) : 0LL);
        listing += '\n';
        if (listing.size() > STREAM_LISTING_MAX_BYTES) {
          status = FILE_TOO_LARGE;
          break;
        }
      }
      closedir(dir);
      if (status == FILE_OK) {
        content = reinterpret_cast<uint8_t *>(listing.data());
        size = listing.size();
      }
    }
  } else {
    FILE *file = std::fopen(local.c_str(), "rb");
    if (file == nullptr) {
      status = FILE_NOT_FOUND;
    } else {
      struct stat info {};
      if (fstat(fileno(file), &info) != 0 || info.st_size < 0) {
        status = FILE_READ_ERROR;
      } else if (static_cast<size_t>(info.st_size) > STREAM_FILE_MAX_BYTES) {
        status = FILE_TOO_LARGE;
      } else {
        size = static_cast<size_t>(info.st_size);
        content = static_cast<uint8_t *>(heap_caps_malloc(std::max<size_t>(size, 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (content == nullptr || std::fread(content, 1, size, file) != size) {
          status = FILE_READ_ERROR;
          heap_caps_free(content);
          content = nullptr;
        }
      }
      std::fclose(file);
    }
    if (status != FILE_OK)
      size = 0;
  }

  StreamFileHeader header{{'P','A','P','P','F','L','0','1'}, status, static_cast<uint32_t>(size), ++file_sequence};
  bool ok = send_screen_stream_bytes(client_fd, &header, sizeof(header));
  if (ok && size != 0)
    ok = send_screen_stream_bytes(client_fd, content, size);
  ESP_LOGI(TAG, "File %s: status %u, %u bytes", path.c_str(), static_cast<unsigned>(status),
           static_cast<unsigned>(size));
  if (content != nullptr && content != reinterpret_cast<uint8_t *>(listing.data()))
    heap_caps_free(content);
  return ok;
}

#ifdef PAX_LOADER_USE_LVGL
void PaxLoader::show_loading_message_() {
  if (this->loading_message_ != nullptr || this->touch_modal_shield_ == nullptr || this->lvgl_ == nullptr ||
      !this->lvgl_->is_loop_started())
    return;

  // This sits on the same full-screen black shield used to protect the host
  // UI. It remains visible throughout app loading and initialization, then
  // the first PAX frame replaces it on the directly-rendered display.
  lv_obj_t *panel = lv_obj_create(this->touch_modal_shield_);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B2A4A), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_style_radius(panel, 14, 0);
  lv_obj_set_style_pad_all(panel, 24, 0);
  lv_obj_center(panel);

  lv_obj_t *label = lv_label_create(panel);
  lv_label_set_text(label, "Loading game...");
  lv_obj_set_style_text_color(label, lv_color_hex(0xD7F3FF), 0);
  lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
  lv_obj_center(label);
  this->loading_message_ = panel;
}

void PaxLoader::hide_loading_message_() {
  if (this->loading_message_ != nullptr) {
    lv_obj_delete(this->loading_message_);
    this->loading_message_ = nullptr;
  }
}
#endif

void PaxLoader::restore_lvgl_() {
#ifdef PAX_LOADER_USE_LVGL
  if (this->lvgl_ == nullptr || !this->lvgl_->is_loop_started()) {
    ESP_LOGW(TAG, "PAX closed before LVGL was ready; skipping UI restore");
    return;
  }

  lv_obj_t *screen = this->lvgl_->get_screen_active();
  if (screen == nullptr || this->lvgl_->get_disp() == nullptr) {
    ESP_LOGW(TAG, "PAX closed but LVGL screen/display is unavailable");
    return;
  }

  // PAX draws directly to the panel and bypasses LVGL's dirty-area tracking.
  // Invalidate the active screen and force one immediate refresh so the
  // interface underneath the PAX replaces the black PAX canvas at once.
  // Drop the PAX's final touch/press state before exposing the launcher
  // again. This keeps a close tap from activating the button underneath it.
  lv_indev_reset(nullptr, nullptr);
  if (this->touch_modal_shield_ != nullptr) {
    lv_obj_delete(this->touch_modal_shield_);
    this->touch_modal_shield_ = nullptr;
    this->loading_message_ = nullptr;
  }
  this->lvgl_->set_paused(false, false);
  for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr;
       indev = lv_indev_get_next(indev)) {
    lv_indev_enable(indev, true);
  }
  lv_indev_reset(nullptr, nullptr);
  lv_obj_invalidate(screen);
  lv_refr_now(this->lvgl_->get_disp());
  ESP_LOGI(TAG, "LVGL interface restored after PAX exit");
#endif
}

void PaxLoader::render_custom_(const uint16_t *buffer, uint16_t in_w, uint16_t in_h, float scale, bool byte_swap) {
  if (this->framebuffer_ == nullptr || buffer == nullptr || in_w == 0 || in_h == 0 || scale <= 0.0f)
    return;

  const int64_t render_start_us = esp_timer_get_time();
  // Resolve an idle hide before choosing the compact direct-transfer path. If
  // this happened only in send_display_buffer_(), one stale slider frame
  // could still be sent before the required full repaint was noticed.
  this->update_control_overlay_();

  // The frame scaled, at most the canvas, centred in it. Recomputed under the
  // display lock by the direct path, so a canvas switch never mixes sizes.
  int canvas_w = 0, canvas_h = 0, out_w = 0, out_h = 0, x0 = 0, y0 = 0;
  bool ppa_size_exact = false;
  auto fit = [&]() {
    canvas_w = this->geometry_.canvas_w;
    canvas_h = this->geometry_.canvas_h;
    out_w = std::min(std::max(1, static_cast<int>(in_w * scale + 0.5f)), canvas_w);
    out_h = std::min(std::max(1, static_cast<int>(in_h * scale + 0.5f)), canvas_h);
    x0 = (canvas_w - out_w) / 2;
    y0 = (canvas_h - out_h) / 2;
    ppa_size_exact = canvas::ppa_scale_exact(in_w, out_w) && canvas::ppa_scale_exact(in_h, out_h);
  };
  fit();

  this->native_rgb888_frame_ = nullptr;
  // Native RGB888 panels can scale/convert into the scanout back buffer in
  // one PPA pass, avoiding both legacy rotations and intermediate copies.
  if (this->native_mipi_dsi_ != nullptr && ppa_size_exact && !byte_swap &&
      !this->control_overlay_rendered_ && !this->control_overlay_repaint_pending_ &&
      !this->game_menu_open_ && !this->stream_client_connected_) {
    if (this->display_mutex_ != nullptr)
      xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY);
    fit();
    const bool sent = this->native_mipi_dsi_->draw_scaled_rgb565(
        this->geometry_.x() + x0, this->geometry_.y() + y0, out_w, out_h, buffer, in_w, in_h,
        &this->native_rgb888_frame_);
    if (sent) {
      ++this->pax_frame_count_;
      this->last_frame_direct_ = false;
    }
    if (this->display_mutex_ != nullptr)
      xSemaphoreGiveRecursive(this->display_mutex_);
    if (sent) {
      this->log_render_time_(render_start_us, in_w, in_h, scale);
      return;
    }
  }

  // Fast path: one PPA pass scales the frame, turns it 180 degrees for the
  // panel and writes only the visible rectangle into ppa_framebuffer_. The
  // panel then receives that rectangle instead of the old canvas-sized
  // 800x480 transfer. This avoids both the full-frame MIPI transfer and the
  // CPU clear/copy that used to be needed for the letterboxed border.
  if (ppa_size_exact && !byte_swap && !this->control_overlay_repaint_pending_ && this->ppa_srm_client_ != nullptr &&
      this->ppa_framebuffer_ != nullptr && this->rotated_framebuffer_ != nullptr) {
    if (this->display_mutex_ != nullptr)
      xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY);
    fit();
    const size_t frame_bytes = canvas::sync_bytes(canvas::frame_bytes(out_w, out_h), this->frame_alloc_bytes_);
    const uint16_t place[4] = {static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), static_cast<uint16_t>(out_w),
                               static_cast<uint16_t>(out_h)};
    if (std::memcmp(place, this->direct_frame_, sizeof(place)) != 0) {
      // A changed letterbox leaves pixels from the previous frame outside the
      // new rectangle. Clear the old full canvas once, not on every frame,
      // before switching to the compact transfer.
      const size_t canvas_bytes =
          canvas::sync_bytes(canvas::frame_bytes(canvas_w, canvas_h), this->frame_alloc_bytes_);
      std::memset(this->rotated_framebuffer_, 0, canvas_bytes);
      esp_cache_msync(this->rotated_framebuffer_, canvas_bytes,
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
      this->display_->draw_pixels_at(
          this->geometry_.raw_x(), this->geometry_.raw_y(), canvas_w, canvas_h,
          reinterpret_cast<const uint8_t *>(this->rotated_framebuffer_), display::COLOR_ORDER_RGB,
          display::COLOR_BITNESS_565, false);
      std::memcpy(this->direct_frame_, place, sizeof(place));
    }
    sync_app_frame(buffer, static_cast<size_t>(in_w) * in_h * sizeof(uint16_t));
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = const_cast<uint16_t *>(buffer), .pic_w = in_w, .pic_h = in_h,
            .block_w = in_w, .block_h = in_h,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = this->ppa_framebuffer_, .buffer_size = frame_bytes,
            .pic_w = static_cast<uint32_t>(out_w), .pic_h = static_cast<uint32_t>(out_h),
            .block_offset_x = 0, .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_180,
        .scale_x = static_cast<float>(out_w) / in_w,
        .scale_y = static_cast<float>(out_h) / in_h,
        .mirror_x = false, .mirror_y = false,
        .rgb_swap = false, .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    const bool sent =
        ppa_size_exact &&
        ppa_do_scale_rotate_mirror(reinterpret_cast<ppa_client_handle_t>(this->ppa_srm_client_), &cfg) == ESP_OK;
    if (sent) {
      // The PPA wrote memory behind the CPU cache: invalidate so the remote
      // view sample and screenshot path see this frame, not a stale one.
      esp_cache_msync(this->ppa_framebuffer_, frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
      this->last_frame_direct_ = true;
      // The direct PPA route bypasses flush_framebuffer_(), so account for
      // the submitted frame here.  Without this, the no-frame watchdog sees
      // a healthy direct-rendering app as a black screen and closes it after
      // five seconds.
      this->pax_frame_count_++;
      const int raw_x = this->geometry_.raw_x() + canvas_w - x0 - out_w;
      const int raw_y = this->geometry_.raw_y() + canvas_h - y0 - out_h;
      this->send_display_buffer_(this->ppa_framebuffer_, render_start_us, raw_x, raw_y, out_w, out_h, out_w);
    } else {
      this->direct_frame_[2] = 0;
    }
    if (this->display_mutex_ != nullptr)
      xSemaphoreGiveRecursive(this->display_mutex_);
    if (sent) {
      this->log_render_time_(render_start_us, in_w, in_h, scale);
      return;
    }
  }

  fit();
  bool ppa_scaled = false;
  // The common PAX path is an exact 2x 400x240 -> 800x480 frame. Let the
  // P4 SRM unit do that copy/scale instead of touching 384,000 pixels on the
  // worker CPU. The old scaler remains below for arbitrary emulator sizes,
  // fractional scales, and byte-swapped input.
  if (ppa_size_exact && !byte_swap && this->ppa_srm_client_ != nullptr && this->ppa_framebuffer_ != nullptr &&
      out_w == canvas_w && out_h == canvas_h && x0 == 0 && y0 == 0) {
    const size_t input_bytes = static_cast<size_t>(in_w) * in_h * sizeof(uint16_t);
    const size_t output_bytes =
        canvas::sync_bytes(canvas::frame_bytes(canvas_w, canvas_h), this->frame_alloc_bytes_);
    sync_app_frame(buffer, input_bytes);
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = const_cast<uint16_t *>(buffer), .pic_w = in_w, .pic_h = in_h,
            .block_w = in_w, .block_h = in_h,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = this->framebuffer_, .buffer_size = output_bytes,
            .pic_w = static_cast<uint32_t>(canvas_w), .pic_h = static_cast<uint32_t>(canvas_h),
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = static_cast<float>(out_w) / in_w,
        .scale_y = static_cast<float>(out_h) / in_h,
        .mirror_x = false, .mirror_y = false,
        .rgb_swap = false, .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(reinterpret_cast<ppa_client_handle_t>(this->ppa_srm_client_), &cfg) == ESP_OK) {
      esp_cache_msync(this->framebuffer_, output_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      ppa_scaled = true;
    }
  }

  // Also accelerate centered emulator-sized frames (for example NES
  // 320x240 -> 640x480). PPA writes the scaled image to its compact output
  // buffer, then only the visible rows are copied into the virtual canvas.
  // This avoids the much more expensive nearest-neighbour loop over the
  // entire scaled image while preserving the existing canvas contract.
  if (!ppa_scaled && ppa_size_exact && !byte_swap && this->ppa_srm_client_ != nullptr && this->ppa_framebuffer_ != nullptr &&
      out_w <= canvas_w && out_h <= canvas_h) {
    const size_t input_bytes = static_cast<size_t>(in_w) * in_h * sizeof(uint16_t);
    const size_t output_bytes = static_cast<size_t>(out_w) * out_h * sizeof(uint16_t);
    sync_app_frame(buffer, input_bytes);
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = const_cast<uint16_t *>(buffer), .pic_w = in_w, .pic_h = in_h,
            .block_w = in_w, .block_h = in_h,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = this->ppa_framebuffer_, .buffer_size = output_bytes,
            .pic_w = static_cast<uint32_t>(out_w), .pic_h = static_cast<uint32_t>(out_h),
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = static_cast<float>(out_w) / in_w,
        .scale_y = static_cast<float>(out_h) / in_h,
        .mirror_x = false, .mirror_y = false,
        .rgb_swap = false, .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(reinterpret_cast<ppa_client_handle_t>(this->ppa_srm_client_), &cfg) == ESP_OK) {
      esp_cache_msync(this->ppa_framebuffer_, output_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      this->clear_(0x0000);
      for (int row = 0; row < out_h; row++) {
        std::memcpy(this->framebuffer_ + static_cast<size_t>(y0 + row) * canvas_w + x0,
                    this->ppa_framebuffer_ + static_cast<size_t>(row) * out_w,
                    static_cast<size_t>(out_w) * sizeof(uint16_t));
      }
      ppa_scaled = true;
    }
  }

  if (!ppa_scaled) {
    this->clear_(0x0000);
    for (int y = 0; y < out_h; y++) {
      const int src_y = (y * in_h) / out_h;
      for (int x = 0; x < out_w; x++) {
        const int src_x = (x * in_w) / out_w;
        uint16_t pixel = buffer[src_y * in_w + src_x];
        if (byte_swap)
          pixel = static_cast<uint16_t>((pixel >> 8) | (pixel << 8));
        this->framebuffer_[static_cast<size_t>(y0 + y) * canvas_w + x0 + x] = pixel;
      }
    }
  }
  this->flush_framebuffer_();
  this->log_render_time_(render_start_us, in_w, in_h, scale);
}

void PaxLoader::log_render_time_(int64_t render_start_us, uint16_t in_w, uint16_t in_h, float scale) {
  const int64_t render_us = esp_timer_get_time() - render_start_us;
  static uint32_t render_frames = 0;
  static int64_t render_total_us = 0;
  static int64_t render_worst_us = 0;
  ++render_frames;
  render_total_us += render_us;
  render_worst_us = std::max(render_worst_us, render_us);
  if (render_frames == 60) {
    ESP_LOGI(TAG, "PAX video frame: avg=%lld us worst=%lld us last=%lld us input=%ux%u scale=%.2f",
             static_cast<long long>(render_total_us / render_frames),
             static_cast<long long>(render_worst_us), static_cast<long long>(render_us),
             static_cast<unsigned>(in_w), static_cast<unsigned>(in_h), scale);
    ESP_LOGI(TAG, "PAX native path: configured=%d used=%d overlay=%d repaint=%d menu=%d recorder=%d",
             this->native_mipi_dsi_ != nullptr, this->native_rgb888_frame_ != nullptr,
             this->control_overlay_rendered_, this->control_overlay_repaint_pending_,
             this->game_menu_open_, this->stream_client_connected_);
    render_frames = 0;
    render_total_us = 0;
    render_worst_us = 0;
  }
}

void PaxLoader::render_emu_() { this->render_custom_(this->emu_buffer_, EMU_WIDTH, EMU_HEIGHT, 2.0f, false); }

bool PaxLoader::right_stick_mouse_enabled_() const {
  // Mouse-oriented PAXs opt into this without changing the stable ABI. The
  // normal gamepad mapping remains unchanged for emulators: their right stick
  // still provides the existing strafe/secondary direction inputs.
  if (!this->launched_)
    return false;
  std::string app = canvas::app_key(this->running_source_);
  for (char &character : app)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return app.find("netsurf") != std::string::npos || app.find("scummvm") != std::string::npos ||
         app.find("redalert") != std::string::npos || app.find("quake2") != std::string::npos;
}

void PaxLoader::read_input_(pax_gamepad_state_t *state, bool include_hid) {
  if (state == nullptr)
    return;
  this->wait_for_game_menu_();
  if (this->game_menu_quarantine_) {
    std::memset(state, 0, sizeof(*state));
    return;
  }
  bool open_app_options = false;
  portENTER_CRITICAL(&this->game_menu_button_lock_);
  const int64_t now_us = esp_timer_get_time();
  if (this->game_menu_open_app_options_pending_) {
    this->game_menu_open_app_options_pending_ = false;
    // Keep the synthetic menu press visible across several app input polls.
    // PAXs may have more than one task polling the shared input service, so
    // a one-read pulse can be consumed before the emulator's frame loop sees it.
    this->game_menu_open_app_options_until_us_ = now_us + 200000;
    ESP_LOGI(TAG, "NES options input pulse delivered to PAX");
  }
  open_app_options = now_us < this->game_menu_open_app_options_until_us_;
  if (!open_app_options)
    this->game_menu_open_app_options_until_us_ = 0;
  portEXIT_CRITICAL(&this->game_menu_button_lock_);
  if (open_app_options) {
    std::memset(state, 0, sizeof(*state));
    state->values[PAX_INPUT_MENU] = 1;
    return;
  }
  // Ignore small stale/noisy axis values around center. The Switch HID
  // reports are quantized and this also prevents a near-threshold value from
  // looking like a held direction after a map/portal transition.
  constexpr float stick_deadzone = 40.0f;
  std::memset(state, 0, sizeof(*state));
  if (include_hid) {
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
      if (this->buttons_[i] != nullptr)
        state->values[i] = this->buttons_[i]->get_state_default(false) ? 1 : 0;
    }
  }
  this->read_remote_ui_input_(state);

  // GPIO2 is the board's TTP223 touch button. The right trigger remains a
  // normal A/fire input, and the touch button is an additional A/fire input.
  if (this->fire_button_ != nullptr && this->fire_button_->get_state_default(false))
    state->values[PAX_INPUT_A] = 1;
  if (this->touch_button_ != nullptr) {
    if (!this->touch_button_->get_state_default(false))
      this->touch_button_released_();
    else if (!this->touch_button_stuck_())
      state->values[PAX_INPUT_A] = 1;
  }

  // Elecrow GPIO16 is a resistor ladder. Preserve the calibrated mapping used
  // by the ESPHome UI, while exposing its diagonal/axis directions to PAXs.
  if (this->adc_button_sensor_ != nullptr) {
    const float voltage = this->adc_button_sensor_->has_state() ? this->adc_button_sensor_->get_state() : 3.3f;
    if (voltage < 1.48f) {
      state->values[PAX_INPUT_DOWN] = 1;
      state->values[PAX_INPUT_LEFT] = 1;
    } else if (voltage < 1.60f) {
      state->values[PAX_INPUT_UP] = 1;
      state->values[PAX_INPUT_LEFT] = 1;
    } else if (voltage < 1.75f) {
      state->values[PAX_INPUT_LEFT] = 1;
    } else if (voltage < 1.95f) {
      state->values[PAX_INPUT_DOWN] = 1;
      state->values[PAX_INPUT_RIGHT] = 1;
    } else if (voltage < 2.10f) {
      state->values[PAX_INPUT_UP] = 1;
      state->values[PAX_INPUT_RIGHT] = 1;
    } else if (voltage < 2.40f) {
      state->values[PAX_INPUT_RIGHT] = 1;
    } else if (voltage < 2.75f) {
      state->values[PAX_INPUT_DOWN] = 1;
    } else if (voltage < 3.10f) {
      state->values[PAX_INPUT_UP] = 1;
    }
  }

  const bool redalert_pointer = this->running_source_.find("redalert") != std::string::npos;
  if (this->left_stick_x_sensor_ != nullptr) {
    const float x = this->left_stick_x_sensor_->has_state() ? this->left_stick_x_sensor_->get_state() : 0.0f;
    if (redalert_pointer) {
      if (std::fabs(x) > stick_deadzone)
        this->enqueue_mouse_delta(x * 0.10f, 0.0f);
    } else {
      if (x < -stick_deadzone) state->values[PAX_INPUT_LEFT] = 1;
      if (x > stick_deadzone) state->values[PAX_INPUT_RIGHT] = 1;
    }
  }
  if (this->left_stick_y_sensor_ != nullptr) {
    const float y = this->left_stick_y_sensor_->has_state() ? this->left_stick_y_sensor_->get_state() : 0.0f;
    if (redalert_pointer) {
      if (std::fabs(y) > stick_deadzone)
        this->enqueue_mouse_delta(0.0f, y * 0.10f);
    } else {
      // USB gamepad Y follows the usual HID convention: negative is physically
      // up and positive is physically down. Keep this separate from the D-pad.
      if (y < -stick_deadzone) state->values[PAX_INPUT_UP] = 1;
      if (y > stick_deadzone) state->values[PAX_INPUT_DOWN] = 1;
    }
  }
  // Mouse-oriented PAXs use the right stick as a relative pointer. Mouse and
  // stick deltas share the visible-landscape direction convention; keep the
  // old L/R and U/D mapping for emulators.
  const bool right_stick_mouse = this->right_stick_mouse_enabled_();
  const bool quake2_mouse = this->launched_ && pax_stick_mouse_is_quake2(this->running_source_.c_str());
  static pax_stick_mouse_t quake2_stick{};
  int look_x = 0, look_y = 0;
  const float qx = this->right_stick_x_sensor_ != nullptr && this->right_stick_x_sensor_->has_state()
                       ? this->right_stick_x_sensor_->get_state() : 0;
  const float qy = this->right_stick_y_sensor_ != nullptr && this->right_stick_y_sensor_->has_state()
                       ? this->right_stick_y_sensor_->get_state() : 0;
  // The game's watchdog also polls the pad: account for elapsed time once
  // across all callers, while enqueue_mouse_delta owns its own lock.
  portENTER_CRITICAL(&this->mouse_input_lock_);
  if (quake2_mouse)
    pax_stick_mouse_step(&quake2_stick, qx, qy, esp_timer_get_time(), &look_x, &look_y);
  else
    quake2_stick = {};
  portEXIT_CRITICAL(&this->mouse_input_lock_);
  if (quake2_mouse)
    this->enqueue_mouse_delta(look_x, look_y);
  if (!quake2_mouse && this->right_stick_x_sensor_ != nullptr) {
    const float x = this->right_stick_x_sensor_->has_state() ? this->right_stick_x_sensor_->get_state() : 0.0f;
    if (right_stick_mouse) {
      if (std::fabs(x) > stick_deadzone)
        this->enqueue_mouse_delta(x * 0.10f, 0.0f);
    } else {
      if (x < -stick_deadzone) state->values[PAX_INPUT_L] = 1;
      if (x > stick_deadzone) state->values[PAX_INPUT_R] = 1;
    }
  }
  if (!quake2_mouse && this->right_stick_y_sensor_ != nullptr) {
    const float y = this->right_stick_y_sensor_->has_state() ? this->right_stick_y_sensor_->get_state() : 0.0f;
    if (right_stick_mouse) {
      if (std::fabs(y) > stick_deadzone)
        this->enqueue_mouse_delta(0.0f, y * 0.10f);
    } else {
      if (y < -stick_deadzone) state->values[PAX_INPUT_UP] = 1;
      if (y > stick_deadzone) state->values[PAX_INPUT_DOWN] = 1;
    }
  }

  // Arrow presses that only came as text (enqueue_keyboard_text): a short
  // D-pad press, enough for a menu step or a small move.
  if (include_hid) {
    const int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < 4; i++) {
      if (now_us < this->arrow_tap_until_us_[i])
        state->values[PAX_INPUT_UP + i] = 1;
    }
  }

#ifdef PAX_LOADER_USE_USB_HIDX
  if (include_hid && this->usb_hidx_ != nullptr && !pax_launcher::owns_pointer(this->running_source_)) {
    /* USB keyboard keys are text/keyboard input, not a second gamepad. Apps
     * such as C64/Frodo consume them through input_keyboard_read(). Keep
     * mouse buttons here because they are a separate HID input device. */
    state->values[PAX_INPUT_A] |= usb_mouse_button_pressed(this->usb_hidx_->get_mouse_left_sensor());
    state->values[PAX_INPUT_B] |= usb_mouse_button_pressed(this->usb_hidx_->get_mouse_right_sensor());
    state->values[PAX_INPUT_X] |= usb_mouse_button_pressed(this->usb_hidx_->get_mouse_middle_sensor());
  }
#endif

  // The current psram_lvgl PAX exits on PAX_INPUT_X. Keep the toggle
  // control independent of the normal gamepad mapping and synthesize that
  // exit input while the configured button is held. This works with the
  // existing PAX binary on the SD card; no ABI or SD-card replacement is
  // required.
  if (this->toggle_button_ != nullptr) {
    const bool pressed = this->toggle_button_->get_state_default(false);
    this->toggle_button_state_ = pressed;
    if (pressed)
      this->toggle_close_requested_ = true;
    if (pressed || this->toggle_close_requested_)
      state->values[PAX_INPUT_X] = 1;
  }

  // A close (on-screen button or API) shows up as MENU, X and the dedicated L3
  // service, in the order close_buttons_() gives.
  const uint8_t close = this->close_buttons_();
  if (close & 1)
    state->values[PAX_INPUT_MENU] = 1;
  if (close & 2)
    state->values[PAX_INPUT_X] = 1;
}

void PaxLoader::read_remote_ui_input_(pax_gamepad_state_t *state) {
  if (state == nullptr)
    return;
  int button = -1;
  int64_t until_us = 0;
  portENTER_CRITICAL(&this->remote_ui_input_lock_);
  button = this->remote_ui_button_;
  until_us = this->remote_ui_button_until_us_;
  portEXIT_CRITICAL(&this->remote_ui_input_lock_);
  if (button >= 0 && button < PAX_INPUT_MAX && esp_timer_get_time() < until_us)
    state->values[button] = 1;
}

void PaxLoader::request_remote_ui_input(const std::string &command) {
  static const std::string launcher_name = "launcher_ui.pax";
  const std::string &source = this->running_app_source_;
  std::string action = command;
  if (!action.empty() && action[0] == '@') {
    const size_t separator = action.find(' ');
    if (separator == std::string::npos) {
      ESP_LOGW(TAG, "Remote UI input ignored: invalid command sequence prefix");
      return;
    }
    action.erase(0, separator + 1);
  }

  const bool launcher_running = this->launched_ && source.size() >= launcher_name.size() &&
      source.compare(source.size() - launcher_name.size(), launcher_name.size(), launcher_name) == 0;
  // Diagnostic button presses exercise the same path as the physical pad.
  // Permit them in the native quick launcher, Quake II and registered shared
  // menu apps for regression QA. Other apps retain the existing guard.
  const bool diagnostic_pad = action.rfind("press ", 0) == 0 &&
      (!this->launched_ || pax_stick_mouse_is_quake2(this->running_source_.c_str()) ||
       this->game_menu_callback_ != nullptr);
  const bool diagnostic_menu_touch = this->game_menu_open_ && action.rfind("tap ", 0) == 0;
  if (!launcher_running && !diagnostic_pad && !diagnostic_menu_touch) {
    ESP_LOGW(TAG, "Remote UI input ignored: launcher_ui.pax is not running");
    return;
  }

  int x = 0, y = 0;
  if (std::sscanf(action.c_str(), "tap %d %d", &x, &y) == 2) {
    if (x < 0 || y < 0 || x >= this->geometry_.canvas_w || y >= this->geometry_.canvas_h) {
      ESP_LOGW(TAG, "Remote UI tap outside canvas: (%d,%d) %dx%d", x, y,
               this->geometry_.canvas_w, this->geometry_.canvas_h);
      return;
    }
    if (this->game_menu_open_) {
      this->game_menu_touch_active_ = false;
      this->game_menu_touch_(x + this->geometry_.x(), y + this->geometry_.y());
      this->game_menu_touch_active_ = false;
      ESP_LOGI(TAG, "Remote shared game menu tap: (%d,%d)", x, y);
      return;
    }
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&this->remote_ui_input_lock_);
    this->remote_ui_touch_x_ = x;
    this->remote_ui_touch_y_ = y;
    this->remote_ui_touch_end_x_ = x;
    this->remote_ui_touch_end_y_ = y;
    this->remote_ui_touch_start_us_ = now_us;
    this->remote_ui_touch_until_us_ = now_us + 180000;
    this->remote_ui_touch_release_pending_ = true;
    portEXIT_CRITICAL(&this->remote_ui_input_lock_);
    ESP_LOGI(TAG, "Remote launcher tap: (%d,%d)", x, y);
    return;
  }

  int x1 = 0, y1 = 0, x2 = 0, y2 = 0, duration_ms = 0;
  if (std::sscanf(action.c_str(), "swipe %d %d %d %d %d",
                  &x1, &y1, &x2, &y2, &duration_ms) == 5) {
    if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0 ||
        x1 >= this->geometry_.canvas_w || x2 >= this->geometry_.canvas_w ||
        y1 >= this->geometry_.canvas_h || y2 >= this->geometry_.canvas_h ||
        duration_ms < 80 || duration_ms > 2000) {
      ESP_LOGW(TAG, "Remote launcher swipe invalid: (%d,%d)->(%d,%d) %d ms on %dx%d",
               x1, y1, x2, y2, duration_ms,
               this->geometry_.canvas_w, this->geometry_.canvas_h);
      return;
    }
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&this->remote_ui_input_lock_);
    this->remote_ui_touch_x_ = x1;
    this->remote_ui_touch_y_ = y1;
    this->remote_ui_touch_end_x_ = x2;
    this->remote_ui_touch_end_y_ = y2;
    this->remote_ui_touch_start_us_ = now_us;
    this->remote_ui_touch_until_us_ = now_us + static_cast<int64_t>(duration_ms) * 1000;
    this->remote_ui_touch_release_pending_ = true;
    portEXIT_CRITICAL(&this->remote_ui_input_lock_);
    ESP_LOGI(TAG, "Remote launcher swipe: (%d,%d)->(%d,%d) in %d ms",
             x1, y1, x2, y2, duration_ms);
    return;
  }

  if (std::sscanf(action.c_str(), "wheel %d", &x) == 1) {
    if (x == 0 || x < -32 || x > 32) {
      ESP_LOGW(TAG, "Remote launcher wheel ignored: expected -32..-1 or 1..32");
      return;
    }
    this->enqueue_mouse_wheel(static_cast<float>(x));
    ESP_LOGI(TAG, "Remote launcher wheel: %d", x);
    return;
  }

  if (action.rfind("press ", 0) == 0) {
    std::string name = action.substr(6);
    for (char &ch : name)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    int button = -1;
    if (name == "up") button = PAX_INPUT_UP;
    else if (name == "right") button = PAX_INPUT_RIGHT;
    else if (name == "down") button = PAX_INPUT_DOWN;
    else if (name == "left") button = PAX_INPUT_LEFT;
    else if (name == "a") button = PAX_INPUT_A;
    else if (name == "b") button = PAX_INPUT_B;
    else if (name == "x") button = PAX_INPUT_X;
    else if (name == "y") button = PAX_INPUT_Y;
    else if (name == "start") button = PAX_INPUT_START;
    else if (name == "select") button = PAX_INPUT_SELECT;
    else if (name == "l") button = PAX_INPUT_L;
    else if (name == "r") button = PAX_INPUT_R;
    else if (name == "menu") button = PAX_INPUT_MENU;
    if (button < 0) {
      ESP_LOGW(TAG, "Remote UI press ignored: unsupported button '%s'", name.c_str());
      return;
    }
    if (button == PAX_INPUT_MENU && this->launched_) {
      portENTER_CRITICAL(&this->game_menu_button_lock_);
      this->game_menu_toggle_pending_ = true;
      portEXIT_CRITICAL(&this->game_menu_button_lock_);
      ESP_LOGI(TAG, "Remote shared game menu toggle requested");
      return;
    }
    portENTER_CRITICAL(&this->remote_ui_input_lock_);
    this->remote_ui_button_ = button;
    this->remote_ui_button_until_us_ = esp_timer_get_time() + 180000;
    portEXIT_CRITICAL(&this->remote_ui_input_lock_);
    ESP_LOGI(TAG, "Remote launcher button: %s", name.c_str());
    return;
  }

  if (action.rfind("key ", 0) == 0) {
    const std::string key_name = action.substr(4);
    int key = -1;
    if (key_name.size() == 1 && static_cast<unsigned char>(key_name[0]) >= 32)
      key = static_cast<unsigned char>(key_name[0]);
    else if (key_name == "space") key = ' ';
    else if (key_name == "enter") key = '\r';
    else if (key_name == "backspace") key = 8;
    if (key < 0 || key > 255) {
      ESP_LOGW(TAG, "Remote UI key ignored: unsupported key");
      return;
    }
    this->enqueue_keyboard_tap_(key);
    ESP_LOGI(TAG, "Remote launcher keyboard key queued: %d", key);
    return;
  }

  if (action.rfind("type ", 0) == 0) {
    const std::string value = action.substr(5);
    if (value.empty() || value.size() > 40) {
      ESP_LOGW(TAG, "Remote UI type ignored: expected 1-40 printable characters");
      return;
    }
    for (unsigned char ch : value) {
      if (ch >= 32 && ch <= 126)
        this->enqueue_keyboard_tap_(ch);
    }
    ESP_LOGI(TAG, "Remote launcher text queued: %u characters", static_cast<unsigned>(value.size()));
    return;
  }

  ESP_LOGW(TAG, "Remote UI input ignored: use tap, swipe, wheel, press, key, or type");
}

void PaxLoader::begin_close_() {
  if (this->global_close_requested_)
    return;
  this->close_game_menu_();
  this->volume_slider_visible_ = false;
  this->brightness_slider_visible_ = false;
  // Leave the combo latched while the buttons remain down. Resetting it here
  // makes the same held Select+Start pair retrigger every 750 ms.
  this->exit_combo_triggered_ = true;
  this->close_requested_us_ = esp_timer_get_time();
  this->global_close_requested_ = true;
}

void PaxLoader::poll_exit_combo_() {
  if (!this->launched_)
    return;
  const bool select = this->buttons_[PAX_INPUT_SELECT] != nullptr &&
                      this->buttons_[PAX_INPUT_SELECT]->get_state_default(false);
  const bool start = this->buttons_[PAX_INPUT_START] != nullptr &&
                     this->buttons_[PAX_INPUT_START]->get_state_default(false);
  if (!select || !start) {
    this->exit_combo_since_us_ = 0;
    this->exit_combo_triggered_ = false;
    return;
  }

  const int64_t now = esp_timer_get_time();
  if (this->exit_combo_since_us_ == 0) {
    this->exit_combo_since_us_ = now;
    return;
  }
  if (!this->exit_combo_triggered_ && now - this->exit_combo_since_us_ >= 750000) {
    this->exit_combo_triggered_ = true;
    this->begin_close_();
    ESP_LOGI(TAG, "PAX exit combo: Select+Start held");
  }
}

// Apps quit on different things: Menu pressed (most emulators; SNES only while
// X and Y are up), Menu released (Stella and Atari800 open their exit menu),
// Menu or L3 held for up to 3 s (Doom, Duke3D, Quake, OpenTyrian), or X
// (utilities such as the touch test). Holding everything at once hid the first
// two, so a close taps Menu alone, lets go, then holds Menu, X and L3 until
// the app returns.
uint8_t PaxLoader::close_buttons_() const {
  if (!this->global_close_requested_)
    return 0;
  const int64_t since_ms = (esp_timer_get_time() - this->close_requested_us_) / 1000;
  if (since_ms < 250)
    return 1;  // Menu alone
  if (since_ms < 500)
    return 0;  // released
  // Keep Menu asserted as well as X/L3. This covers PAXs such as Stella that
  // use Menu's release edge to open their quit menu, while the other two
  // signals cover apps that use a direct quit action.
  return 1 | 2 | 4;
}

int PaxLoader::read_touch_(int *x, int *y) {
  this->wait_for_game_menu_();
  if (this->game_menu_quarantine_)
    return 0;
  int remote_x = 0, remote_y = 0;
  int remote_end_x = 0, remote_end_y = 0;
  int64_t remote_start_us = 0;
  int64_t remote_until_us = 0;
  bool remote_release_pending = false;
  portENTER_CRITICAL(&this->remote_ui_input_lock_);
  remote_x = this->remote_ui_touch_x_;
  remote_y = this->remote_ui_touch_y_;
  remote_end_x = this->remote_ui_touch_end_x_;
  remote_end_y = this->remote_ui_touch_end_y_;
  remote_start_us = this->remote_ui_touch_start_us_;
  remote_until_us = this->remote_ui_touch_until_us_;
  remote_release_pending = this->remote_ui_touch_release_pending_;
  const int64_t now_us = esp_timer_get_time();
  if (now_us >= remote_until_us)
    this->remote_ui_touch_release_pending_ = false;
  portEXIT_CRITICAL(&this->remote_ui_input_lock_);
  if (now_us < remote_until_us) {
    const int64_t duration_us = remote_until_us - remote_start_us;
    const int64_t elapsed_us = now_us > remote_start_us ? now_us - remote_start_us : 0;
    const int64_t x_delta = static_cast<int64_t>(remote_end_x) - remote_x;
    const int64_t y_delta = static_cast<int64_t>(remote_end_y) - remote_y;
    if (duration_us > 0 && (x_delta != 0 || y_delta != 0)) {
      remote_x += static_cast<int>((x_delta * elapsed_us) / duration_us);
      remote_y += static_cast<int>((y_delta * elapsed_us) / duration_us);
    }
    if (!this->touch_active_)
      ESP_LOGI(TAG, "Remote launcher touch PRESSED: (%d,%d)", remote_x, remote_y);
    this->touch_active_ = true;
    if (x != nullptr) *x = remote_x;
    if (y != nullptr) *y = remote_y;
    return 1;
  }
  if (remote_release_pending) {
    if (this->touch_active_)
      ESP_LOGI(TAG, "Remote launcher touch RELEASED");
    this->touch_active_ = false;
    this->close_hold_since_us_ = 0;
    this->volume_touch_active_ = false;
    this->brightness_touch_active_ = false;
    return 0;
  }
  if (this->touchscreen_ == nullptr)
    return 0;

  // The ESPHome touchscreen loop remains active while the PAX worker runs;
  // read the latest cached sample without starting a competing I2C poll.
  auto touch = this->touchscreen_->get_touch();
  if (!touch.has_value() || (touch->state & touchscreen::STATE_RELEASING) != 0) {
    if (this->touch_active_) {
      ESP_LOGI(TAG, "PAX touch RELEASED");
      this->touch_active_ = false;
    }
    this->close_hold_since_us_ = 0;
    this->volume_touch_active_ = false;
    this->brightness_touch_active_ = false;
    return 0;
  }

  const int physical_x = touch->x;
  const int physical_y = touch->y;
  // The hidden toolbar is intentionally woken only by the two-finger gesture
  // handled in poll_close_button_(). A regular game touch must stay a game
  // touch and must not reveal or move the loader controls.
  if (this->control_overlay_wake_gesture_active_ || this->control_overlay_swipe_tracking_) {
    this->touch_active_ = false;
    return 0;
  }
  const canvas::Geometry geometry = this->geometry_;
  if (this->volume_touch_(physical_x, physical_y)) {
    this->touch_active_ = true;
    return 0;
  }
  if (this->close_touch_(physical_x, physical_y)) {
    this->touch_active_ = true;
    return 0;
  }
  if (this->reload_touch_(physical_x, physical_y)) {
    this->touch_active_ = true;
    return 0;
  }

  int canvas_x = 0;
  int canvas_y = 0;
  if (!geometry.to_canvas(physical_x, physical_y, &canvas_x, &canvas_y)) {
    if (!this->touch_active_) {
      ESP_LOGD(TAG, "PAX touch outside canvas: physical=(%d,%d)", physical_x, physical_y);
    }
    return 0;
  }

  // The touchscreen component reports the physical landscape panel point,
  // which is already the coordinate space used by the visible UI. The
  // framebuffer's 180-degree presentation rotation must not be applied to
  // touch a second time or left/right and up/down are reversed.
  const int logical_x = canvas_x;
  const int logical_y = canvas_y;

  if (!this->touch_active_) {
    ESP_LOGI(TAG, "PAX touch PRESSED: physical=(%d,%d) panel_canvas=(%d,%d) logical=(%d,%d)",
             physical_x, physical_y, canvas_x, canvas_y, logical_x, logical_y);
    this->touch_active_ = true;
  }

  if (x != nullptr)
    *x = logical_x;
  if (y != nullptr)
    *y = logical_y;
  return 1;
}

void PaxLoader::audio_init_(int sample_rate) {
  if (this->speaker_ == nullptr || sample_rate <= 0)
    return;
  const bool rate_changed = this->audio_sample_rate_ != sample_rate;
  if (rate_changed) {
    this->audio_sample_rate_ = sample_rate;
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 2, sample_rate));
    ESP_LOGI(TAG, "PAX audio initialized: %d Hz stereo 16-bit", sample_rate);
  }
  this->speaker_->set_mute_state(this->game_menu_open_ || this->master_volume_ == 0);
  this->speaker_->set_volume(static_cast<float>(this->master_volume_) / 100.0f);
  // Repeated app initialization must not restart a healthy audio chain.
  // A stopped chain still needs start(), including after a long silence.
  if (!rate_changed && this->speaker_->is_running())
    return;
  // The speaker's I2S channel and DMA buffers are allocated from internal
  // DMA-capable RAM as it starts: what is left for them.
  log_heap("as the app starts audio");
  this->speaker_->start();
}

void PaxLoader::audio_submit_(short *stereo_buf, int frame_count) {
  if (this->speaker_ == nullptr || stereo_buf == nullptr || frame_count <= 0)
    return;
  const size_t bytes = static_cast<size_t>(frame_count) * 2 * sizeof(int16_t);
  const size_t stream_bytes = static_cast<size_t>((frame_count + 1) / 2) * sizeof(int16_t);
  if (this->stream_client_connected_ && this->stream_mutex_ != nullptr && this->stream_audio_buffer_ != nullptr &&
      xSemaphoreTake(this->stream_mutex_, 0) == pdTRUE) {
    while (this->stream_audio_available_ + stream_bytes > SCREEN_STREAM_AUDIO_BYTES) {
      this->stream_audio_read_ = (this->stream_audio_read_ + sizeof(int16_t)) % SCREEN_STREAM_AUDIO_BYTES;
      this->stream_audio_available_ -= sizeof(int16_t);
    }
    // The diagnostic track is mono (the left channel) at half the game's
    // native sample rate. This keeps the combined video/audio transport below
    // the board's sustainable TCP rate.
    for (int frame = 0; frame < frame_count; frame += 2) {
      std::memcpy(this->stream_audio_buffer_ + this->stream_audio_write_, &stereo_buf[frame * 2], sizeof(int16_t));
      this->stream_audio_write_ = (this->stream_audio_write_ + sizeof(int16_t)) % SCREEN_STREAM_AUDIO_BYTES;
    }
    this->stream_audio_available_ += stream_bytes;
    xSemaphoreGive(this->stream_mutex_);
  }
  // Let the speaker/resampler chain drain transient back-pressure. This runs
  // on the PAX's dedicated audio mixer task, not the game/render task. The
  // chain starts asynchronously while Quake is already submitting its first
  // block, so keep that complete block intact for several output intervals.
  const uint8_t *pcm = reinterpret_cast<const uint8_t *>(stereo_buf);
  const int64_t audio_submit_start_us = esp_timer_get_time();
  const int64_t write_deadline_us = audio_submit_start_us + 750000;
  size_t written = 0;
  while (written < bytes) {
    const size_t accepted = this->speaker_->play(pcm + written, bytes - written, pdMS_TO_TICKS(100));
    if (accepted > 0) {
      written += accepted;
      continue;
    }
    if (esp_timer_get_time() >= write_deadline_us)
      break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  const int64_t audio_wait_us = esp_timer_get_time() - audio_submit_start_us;
  static int64_t last_audio_debug_log_us = 0;
  const int64_t audio_now_us = esp_timer_get_time();
  if (audio_now_us - last_audio_debug_log_us >= 30000000) {
    int peak = 0;
    for (int i = 0; i < frame_count * 2; i++) {
      const int value = std::abs(static_cast<int>(stereo_buf[i]));
      if (value > peak)
        peak = value;
    }
    ESP_LOGI(TAG, "PAX audio: input=%d Hz frames=%d peak=%d wrote=%u/%u wait=%lld us running=%d muted=%d volume=%.2f",
             this->audio_sample_rate_, frame_count, peak, static_cast<unsigned>(written),
             static_cast<unsigned>(bytes), static_cast<long long>(audio_wait_us), this->speaker_->is_running(),
             this->speaker_->get_mute_state(), this->speaker_->get_volume());
    last_audio_debug_log_us = audio_now_us;
  }
  if (written != bytes) {
    // The first of an app run: a speaker that could not get its I2S DMA
    // buffers (ESP_ERR_NO_MEM) takes nothing, so show the heap it had.
    if (!this->audio_trouble_logged_) {
      this->audio_trouble_logged_ = true;
      log_heap("at the app's first short audio write");
    }
    static int64_t last_short_write_log_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_short_write_log_us >= 1000000) {
      ESP_LOGW(TAG, "PAX audio short write: %u/%u bytes", static_cast<unsigned>(written),
               static_cast<unsigned>(bytes));
      last_short_write_log_us = now_us;
    }
  }
  // ESPHome's mixer/resampler can remain marked running after its downstream
  // speaker stops draining. Re-start the chain after sustained empty writes;
  // otherwise both music and SFX remain silent indefinitely.
  static int empty_writes = 0;
  empty_writes = written == 0 ? empty_writes + 1 : 0;
  if (empty_writes >= 10) {
    ESP_LOGW(TAG, "PAX audio: speaker is not draining; restarting it");
    this->speaker_->stop();
    this->speaker_->start();
    empty_writes = 0;
  }
}

uint16_t *PaxLoader::svc_display_get_framebuffer() { return active_ != nullptr ? active_->framebuffer_ : nullptr; }
uint16_t *PaxLoader::svc_display_get_emu_buffer() { return active_ != nullptr ? active_->emu_buffer_ : nullptr; }
void PaxLoader::svc_display_flush() {
  if (active_ != nullptr)
    active_->flush_framebuffer_();
}
void PaxLoader::svc_display_emu_flush() {
  if (active_ != nullptr)
    active_->render_emu_();
}
void PaxLoader::svc_display_clear(uint16_t color) {
  if (active_ != nullptr)
    active_->clear_(color);
}
void PaxLoader::svc_display_set_scale(float sx, float sy) {
  if (active_ != nullptr) {
    active_->scale_x_ = sx;
    active_->scale_y_ = sy;
  }
}
void PaxLoader::svc_display_write_frame_rgb565(const uint16_t *buffer) {
  if (active_ != nullptr && active_->framebuffer_ != nullptr && buffer != nullptr)
    std::memcpy(active_->framebuffer_, buffer,
                canvas::frame_bytes(active_->geometry_.canvas_w, active_->geometry_.canvas_h));
}
void PaxLoader::add_hardware_sensor(const std::string &name,const std::string &unit,sensor::Sensor *sensor) {
  HardwareChannel c; snprintf(c.info.name,sizeof(c.info.name),"%s",name.c_str());
  snprintf(c.info.unit,sizeof(c.info.unit),"%s",unit.c_str()); c.info.flags=PAX_HW_READ;
  c.read=[sensor](){return sensor->state;}; this->hardware_.push_back(std::move(c));
}
void PaxLoader::add_hardware_binary(const std::string &name,const std::string &unit,binary_sensor::BinarySensor *sensor) {
  HardwareChannel c; snprintf(c.info.name,sizeof(c.info.name),"%s",name.c_str());
  snprintf(c.info.unit,sizeof(c.info.unit),"%s",unit.c_str()); c.info.flags=PAX_HW_READ;
  c.read=[sensor](){return sensor->has_state()?float(sensor->state):NAN;}; this->hardware_.push_back(std::move(c));
}
void PaxLoader::add_hardware_number(const std::string &name,const std::string &unit,number::Number *number) {
  HardwareChannel c; snprintf(c.info.name,sizeof(c.info.name),"%s",name.c_str());
  snprintf(c.info.unit,sizeof(c.info.unit),"%s",unit.c_str()); c.info.flags=PAX_HW_READ|PAX_HW_WRITE;
  c.read=[number](){return number->state;}; c.output=number; this->hardware_.push_back(std::move(c));
}
void PaxLoader::update_hardware_() {
  // The vector is fixed after codegen/setup. Only cached POD state crosses tasks.
  for(auto &c:this->hardware_) {
    float value=c.read();float low=0,high=0;
    if(c.output) {low=c.output->traits.get_min_value();high=c.output->traits.get_max_value();}
    portENTER_CRITICAL(&this->hardware_lock_);
    c.value=value;c.valid=std::isfinite(value);c.info.minimum=low;c.info.maximum=high;
    portEXIT_CRITICAL(&this->hardware_lock_);
  }
  for(unsigned processed=0;processed<16;processed++) {
    HardwareCommand command{};bool have=false;
    portENTER_CRITICAL(&this->hardware_lock_);
    if(this->hardware_head_!=this->hardware_tail_) {
      command=this->hardware_queue_[this->hardware_head_];this->hardware_head_=(this->hardware_head_+1)%16;have=true;
    }
    portEXIT_CRITICAL(&this->hardware_lock_);
    if(!have)break;
    auto *output=this->hardware_[command.index].output;
    if(output)output->make_call().set_value(command.value).perform();
  }
}
int PaxLoader::svc_jpeg_decode_rgb565(const uint8_t *data, uint32_t size, uint16_t *out,
                                      uint32_t capacity, int width, int height, int stride) {
  return jpeg_accelerator.decode(data, size, out, capacity, width, height, stride);
}
int PaxLoader::svc_hardware_count() {return active_?active_->hardware_.size():0;}
int PaxLoader::svc_hardware_info(int index,pax_hardware_info_t *out) {
  auto *self=active_;if(!self || !out || index<0 || index>=(int)self->hardware_.size())return -1;
  portENTER_CRITICAL(&self->hardware_lock_);*out=self->hardware_[index].info;
  if(self->hardware_[index].valid)out->flags|=PAX_HW_VALID;
  portEXIT_CRITICAL(&self->hardware_lock_);return 0;
}
int PaxLoader::svc_hardware_read(const char *name,float *value) {
  auto *self=active_;if(!self || !name || !value)return -1;
  for(auto &c:self->hardware_)if(strcmp(c.info.name,name)==0) {
    portENTER_CRITICAL(&self->hardware_lock_);bool valid=c.valid;*value=c.value;portEXIT_CRITICAL(&self->hardware_lock_);
    return valid?0:-2;
  }
  return -1;
}
int PaxLoader::svc_hardware_write(const char *name,float value) {
  auto *self=active_;if(!self || !name)return -1;
  if(!std::isfinite(value))return -2;
  for(size_t i=0;i<self->hardware_.size();i++) {
    auto &c=self->hardware_[i];if(strcmp(c.info.name,name)!=0)continue;
    if(!c.output)return -1;
    portENTER_CRITICAL(&self->hardware_lock_);
    if(value<c.info.minimum || value>c.info.maximum) {portEXIT_CRITICAL(&self->hardware_lock_);return -2;}
    unsigned next=(self->hardware_tail_+1)%16;
    if(next==self->hardware_head_) {portEXIT_CRITICAL(&self->hardware_lock_);return -3;}
    self->hardware_queue_[self->hardware_tail_]={(int)i,value};self->hardware_tail_=next;
    portEXIT_CRITICAL(&self->hardware_lock_);return 0;
  }
  return -1;
}
void PaxLoader::svc_display_get_size(int *width, int *height) {
  int w = canvas::LEGACY_WIDTH;
  int h = canvas::LEGACY_HEIGHT;
  if (active_ != nullptr) {
    // The app's own choice once it made one, else what it is offered.
    w = active_->canvas_chosen_ ? active_->geometry_.canvas_w : active_->offered_canvas_w_;
    h = active_->canvas_chosen_ ? active_->geometry_.canvas_h : active_->offered_canvas_h_;
  }
  if (width != nullptr)
    *width = w;
  if (height != nullptr)
    *height = h;
}
int PaxLoader::svc_display_set_canvas(int width, int height) {
  return active_ != nullptr ? active_->set_canvas_(width, height) : -1;
}
int PaxLoader::svc_touch_read_points(pax_touch_point_t *points, int max) {
  if (active_ == nullptr || points == nullptr || max <= 0)
    return 0;
  active_->wait_for_game_menu_();
  if (active_->game_menu_quarantine_)
    return 0;
  touchscreen::TouchPoint touches[MAX_TOUCH_POINTS];
  portENTER_CRITICAL(&active_->touch_points_lock_);
  const int count = active_->touch_point_count_;
  for (int i = 0; i < count; i++)
    touches[i] = active_->touch_points_[i];
  portEXIT_CRITICAL(&active_->touch_points_lock_);
  const canvas::Geometry geometry = active_->geometry_;
  int out = 0;
  for (int i = 0; i < count && out < max; i++) {
    int x = 0, y = 0;
    if (!geometry.to_canvas(touches[i].x, touches[i].y, &x, &y))
      continue;
    // Keep multi-touch coordinates on the same canonical PAX ABI as
    // touch_read(): the physical panel point is already in the visible
    // landscape coordinate space. Do not apply the framebuffer rotation a
    // second time here.
    points[out].x = static_cast<int16_t>(x);
    points[out].y = static_cast<int16_t>(y);
    points[out].id = touches[i].id;
    out++;
  }
  return out;
}
int PaxLoader::svc_midi_read(uint8_t *buf, int len) {
#ifdef PAX_LOADER_USE_USB_MIDI
  if (active_ != nullptr && active_->usb_midi_ != nullptr && buf != nullptr && len > 0)
    return static_cast<int>(active_->usb_midi_->read(buf, static_cast<size_t>(len)));
#endif
  return 0;
}
int PaxLoader::svc_midi_write(const uint8_t *data, int len) {
#ifdef PAX_LOADER_USE_USB_MIDI
  if (active_ != nullptr && active_->usb_midi_ != nullptr && data != nullptr && len > 0)
    return active_->usb_midi_->write(data, static_cast<size_t>(len)) ? 0 : -1;
#endif
  return -1;
}
void PaxLoader::svc_display_write_frame_custom(const uint16_t *buffer, uint16_t in_w, uint16_t in_h,
                                                float scale, bool byte_swap) {
  if (active_ != nullptr)
    active_->render_custom_(buffer, in_w, in_h, scale, byte_swap);
}
void PaxLoader::svc_display_write_rect(int x, int y, int w, int h, const uint16_t *data) {
  if (active_ == nullptr || active_->framebuffer_ == nullptr || data == nullptr)
    return;
  const int canvas_w = active_->geometry_.canvas_w;
  const int canvas_h = active_->geometry_.canvas_h;
  if (pax_launcher::owns_pointer(active_->running_source_))
    active_->launcher_dirty_region_.add(x, y, w, h, canvas_w, canvas_h);
  pax_frame::copy_rect(active_->framebuffer_, canvas_w, canvas_h, x, y, w, h, data);
}
int PaxLoader::svc_sprite_blit(uint16_t *framebuf, uint32_t fb_w, uint32_t fb_h,
                                uint32_t x, uint32_t y, const uint16_t *sprite,
                                uint32_t sp_w, uint32_t sp_h, uint16_t colorkey) {
  if (framebuf == nullptr || sprite == nullptr || fb_w == 0 || fb_h == 0 || sp_w == 0 || sp_h == 0)
    return -1;
  for (uint32_t row = 0; row < sp_h; row++) {
    const uint32_t dst_y = y + row;
    if (dst_y >= fb_h)
      continue;
    for (uint32_t col = 0; col < sp_w; col++) {
      const uint32_t dst_x = x + col;
      if (dst_x >= fb_w)
        continue;
      const uint16_t pixel = sprite[row * sp_w + col];
      if (pixel != colorkey)
        framebuf[dst_y * fb_w + dst_x] = pixel;
    }
  }
  return 0;
}
int PaxLoader::svc_fb_copy(const uint16_t *src, uint16_t *dst, uint32_t w, uint32_t h) {
  if (src == nullptr || dst == nullptr || w == 0 || h == 0)
    return -1;
  std::memcpy(dst, src, static_cast<size_t>(w) * h * sizeof(uint16_t));
  return 0;
}

uint16_t *PaxLoader::svc_png_load_rgb565(const char *path,
                                          uint16_t *out_w, uint16_t *out_h) {
  if (path == nullptr)
    return nullptr;

  // PAXs use the stable /sd namespace, while the ESPHome SD component mounts
  // the actual VFS at /sdcard. Keep PNG loading consistent with file_open().
  std::string fs_path(path);
  if (fs_path == "/sd")
    fs_path = "/sdcard";
  else if (fs_path.rfind("/sd/", 0) == 0)
    fs_path = "/sdcard/" + fs_path.substr(4);

  pngObject png{};
  if (!loadPngFromFileRaw(fs_path.c_str(), &png, true, true) || png.data == nullptr ||
      png.w == 0 || png.h == 0) {
    ESP_LOGW(TAG, "PNG load failed: %s", path);
    return nullptr;
  }

  // PNGdec's little-endian RGB565 output must be converted to the panel's
  // byte order and BGR wiring. This matches the native launcher path.
  auto *pixels = reinterpret_cast<uint16_t *>(png.data);
  const size_t pixel_count = static_cast<size_t>(png.w) * png.h;
  for (size_t i = 0; i < pixel_count; i++) {
    const uint16_t pixel = pixels[i];
    const uint16_t swapped = static_cast<uint16_t>((pixel >> 8) | (pixel << 8));
    pixels[i] = static_cast<uint16_t>((swapped & 0x07e0) |
                                      ((swapped & 0x001f) << 11) |
                                      ((swapped >> 11) & 0x001f));
  }

  if (out_w != nullptr)
    *out_w = png.w;
  if (out_h != nullptr)
    *out_h = png.h;
  ESP_LOGI(TAG, "PNG loaded: %s (%ux%u)", path, png.w, png.h);
  return reinterpret_cast<uint16_t *>(png.data);
}

uint16_t *PaxLoader::svc_png_load_rgb565_blended(const char *path,
                                                  uint16_t *out_w, uint16_t *out_h,
                                                  uint32_t background_bgr888) {
  if (path == nullptr)
    return nullptr;

  std::string fs_path(path);
  if (fs_path == "/sd")
    fs_path = "/sdcard";
  else if (fs_path.rfind("/sd/", 0) == 0)
    fs_path = "/sdcard/" + fs_path.substr(4);

  pngObject png{};
  if (!loadPngFromFileRawBackground(fs_path.c_str(), &png, true, true,
                                    background_bgr888) ||
      png.data == nullptr || png.w == 0 || png.h == 0) {
    ESP_LOGW(TAG, "Alpha PNG load failed: %s", path);
    return nullptr;
  }

  // Keep the same panel byte/channel ordering as the legacy PNG service.
  auto *pixels = reinterpret_cast<uint16_t *>(png.data);
  const size_t pixel_count = static_cast<size_t>(png.w) * png.h;
  for (size_t i = 0; i < pixel_count; i++) {
    const uint16_t pixel = pixels[i];
    const uint16_t swapped = static_cast<uint16_t>((pixel >> 8) | (pixel << 8));
    pixels[i] = static_cast<uint16_t>((swapped & 0x07e0) |
                                      ((swapped & 0x001f) << 11) |
                                      ((swapped >> 11) & 0x001f));
  }

  if (out_w != nullptr)
    *out_w = png.w;
  if (out_h != nullptr)
    *out_h = png.h;
  ESP_LOGI(TAG, "Alpha PNG loaded: %s (%ux%u)", path, png.w, png.h);
  return reinterpret_cast<uint16_t *>(png.data);
}
void PaxLoader::svc_display_lock() {
  if (active_ != nullptr && active_->display_mutex_ != nullptr)
    if (xSemaphoreTakeRecursive(active_->display_mutex_, portMAX_DELAY) == pdTRUE)
      pax_display_lease(1);
}
void PaxLoader::svc_display_unlock() {
  if (active_ != nullptr && active_->display_mutex_ != nullptr)
    if (xSemaphoreGiveRecursive(active_->display_mutex_) == pdTRUE)
      pax_display_lease(-1);
}
void PaxLoader::svc_audio_init(int sample_rate) {
  if (active_ != nullptr)
    active_->audio_init_(sample_rate);
}
void PaxLoader::svc_audio_submit(short *stereo_buf, int frame_count) {
  if (active_ != nullptr)
    active_->audio_submit_(stereo_buf, frame_count);
}
void PaxLoader::svc_input_gamepad_read(pax_gamepad_state_t *state) {
  if (active_ != nullptr)
    active_->read_input_(state);
  else if (state != nullptr)
    std::memset(state, 0, sizeof(*state));
}
int PaxLoader::svc_game_menu_register(const char *label, pax_game_menu_action_fn_t callback, void *context) {
  if (active_ == nullptr)
    return -1;
  portENTER_CRITICAL(&active_->game_menu_button_lock_);
  active_->game_menu_callback_ = callback;
  active_->game_menu_context_ = context;
  active_->game_menu_task_ = callback != nullptr ? xTaskGetCurrentTaskHandle() : nullptr;
  active_->game_menu_action_pending_ = false;
  active_->game_menu_busy_ = false;
  std::snprintf(active_->game_menu_provider_label_, sizeof(active_->game_menu_provider_label_), "%s",
                label != nullptr ? label : "APP OPTIONS");
  portEXIT_CRITICAL(&active_->game_menu_button_lock_);
  if (callback != nullptr)
    ESP_LOGI(TAG, "Shared game menu registered: %s", active_->game_menu_provider_label_);
  return 0;
}

int PaxLoader::svc_game_menu_set_page(const pax_game_menu_page_t *page) {
  if (active_ == nullptr || (page != nullptr && (page->count == 0 || page->count > PAX_GAME_MENU_MAX_ITEMS)))
    return -1;
  auto *self = active_;
  const bool locked = self->display_mutex_ != nullptr &&
                      xSemaphoreTakeRecursive(self->display_mutex_, portMAX_DELAY) == pdTRUE;
  if (!self->game_menu_open_ || self->game_menu_frame_ == nullptr) {
    if (locked)
      xSemaphoreGiveRecursive(self->display_mutex_);
    return -1;
  }
  self->game_menu_app_page_ = page != nullptr;
  if (page != nullptr) {
    self->game_menu_page_ = *page;
    self->game_menu_page_.title[sizeof(page->title) - 1] = '\0';
    self->game_menu_page_.status[sizeof(page->status) - 1] = '\0';
    for (uint32_t i = 0; i < page->count; ++i)
      self->game_menu_page_.items[i].label[sizeof(page->items[i].label) - 1] = '\0';
    if (page->preview != nullptr) {
      uint16_t *preview = self->game_menu_frame_ + GAME_MENU_FRAME_PIXELS;
      std::memcpy(preview, page->preview, GAME_MENU_PREVIEW_PIXELS * sizeof(uint16_t));
      self->game_menu_page_.preview = preview;
    }
    self->game_menu_item_count_ = static_cast<uint8_t>(page->count);
    self->game_menu_selection_ = static_cast<uint8_t>(std::min(page->selected, page->count - 1));
    self->game_menu_font_scale_ = self->game_menu_width_ >= 360 ? 2 : 1;
    const int label_width = self->game_menu_width_ - 50 - (page->preview != nullptr ? 76 : 0);
    for (uint32_t i = 0; i < page->count; ++i) {
      if (std::strlen(self->game_menu_page_.items[i].label) * 6 * self->game_menu_font_scale_ >
          static_cast<size_t>(label_width))
        self->game_menu_font_scale_ = 1;
    }
  } else {
    self->game_menu_page_ = {};
    self->game_menu_item_count_ = GAME_MENU_NES_ITEM_COUNT;
    self->game_menu_selection_ = GAME_MENU_BASE_ITEM_COUNT;
    self->game_menu_font_scale_ = self->game_menu_width_ >= 360 ? 2 : 1;
  }
  self->game_menu_row_height_ = (self->game_menu_height_ - 84) / self->game_menu_item_count_;
  self->game_menu_frame_dirty_ = true;
  portENTER_CRITICAL(&self->game_menu_button_lock_);
  self->game_menu_busy_ = false;
  portEXIT_CRITICAL(&self->game_menu_button_lock_);
  self->redraw_control_overlay_now_(true);
  if (locked)
    xSemaphoreGiveRecursive(self->display_mutex_);
  ESP_LOGI(TAG, "Shared game menu page: %s (%u rows), overlay remains open",
           page != nullptr ? self->game_menu_page_.title : "GAME MENU", self->game_menu_item_count_);
  return 0;
}

void PaxLoader::svc_game_menu_close() {
  if (active_ != nullptr)
    active_->close_game_menu_();
}

int PaxLoader::svc_input_l3_read() {
  if (active_ == nullptr)
    return 0;
  active_->wait_for_game_menu_();
  if (active_->game_menu_quarantine_)
    return 0;
  if (active_->close_buttons_() & 4)
    return 1;
  return active_->toggle_button_ != nullptr && active_->toggle_button_->get_state_default(false) ? 1 : 0;
}
int PaxLoader::svc_input_mouse_read(int *dx, int *dy, int *buttons) {
#ifdef PAX_LOADER_USE_USB_HIDX
  if (active_ != nullptr)
    return active_->read_mouse_(dx, dy, buttons);
#endif
  if (dx != nullptr) *dx = 0;
  if (dy != nullptr) *dy = 0;
  if (buttons != nullptr) *buttons = 0;
  return 0;
}
int PaxLoader::svc_input_mouse_scroll_read(int *wheel) {
  if (active_ != nullptr)
    return active_->read_mouse_scroll_(wheel);
  if (wheel != nullptr) *wheel = 0;
  return 0;
}
int PaxLoader::svc_input_keyboard_read(pax_keyboard_event_t *event) {
  return active_ != nullptr ? active_->read_keyboard_(event) : 0;
}
int PaxLoader::svc_ui_call_sync(pax_ui_call_fn_t callback, void *arg, uint32_t timeout_ms) {
  PaxLoader *self = active_;
  if (self == nullptr || callback == nullptr || self->ui_call_done_ == nullptr)
    return -1;

  // The completion semaphore is binary.  Drain a completion left by the
  // previous call before publishing the next request.
  (void)xSemaphoreTake(self->ui_call_done_, 0);

  portENTER_CRITICAL(&self->ui_call_lock_);
  if (self->ui_call_state_ != 0) {
    portEXIT_CRITICAL(&self->ui_call_lock_);
    return -2;
  }
  self->ui_call_callback_ = callback;
  self->ui_call_arg_ = arg;
  self->ui_call_result_ = -1;
  self->ui_call_cancelled_ = false;
  self->ui_call_state_ = 1;
  portEXIT_CRITICAL(&self->ui_call_lock_);

  const TickType_t wait_ticks = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  if (xSemaphoreTake(self->ui_call_done_, wait_ticks) != pdTRUE) {
    bool cancelled = false;
    bool executing = false;
    portENTER_CRITICAL(&self->ui_call_lock_);
    if (self->ui_call_state_ == 1) {
      // The callback has not started.  It is safe to cancel the request.
      self->ui_call_cancelled_ = true;
      self->ui_call_callback_ = nullptr;
      self->ui_call_arg_ = nullptr;
      self->ui_call_result_ = -3;
      self->ui_call_state_ = 0;
      cancelled = true;
    } else if (self->ui_call_state_ == 2) {
      // Once the ESPHome task has entered the callback, wait for it to
      // finish.  Returning early could let the loader unmap the PAX while
      // LVGL still has a callback into the app's code.
      executing = true;
    }
    portEXIT_CRITICAL(&self->ui_call_lock_);
    if (cancelled)
      return -3;
    if (executing)
      (void)xSemaphoreTake(self->ui_call_done_, portMAX_DELAY);
  }

  int result = -1;
  portENTER_CRITICAL(&self->ui_call_lock_);
  result = self->ui_call_result_;
  if (self->ui_call_state_ == 3)
    self->ui_call_state_ = 0;
  portEXIT_CRITICAL(&self->ui_call_lock_);
  return result;
}

int PaxLoader::svc_touch_read(int *x, int *y) { return active_ != nullptr ? active_->read_touch_(x, y) : 0; }

// ── UDP and TCP for apps (multiplayer) ──────────────────────────────────────
// Sockets are plain lwIP descriptors. The loader remembers them so a crashing
// or careless app cannot leak them past its exit. 16: an 8-player TCP game
// needs a listener, up to 7 peers and a UDP discovery socket.
static constexpr int APP_SOCKET_MAX = 16;
static int s_app_sockets[APP_SOCKET_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

static bool app_socket_slot_free() {
  for (int fd : s_app_sockets) {
    if (fd < 0)
      return true;
  }
  return false;
}

static void remember_app_socket(int fd) {
  for (int &slot : s_app_sockets) {
    if (slot < 0) {
      slot = fd;
      return;
    }
  }
}

static bool is_app_socket(int handle) {
  if (handle < 0)
    return false;
  for (int fd : s_app_sockets) {
    if (fd == handle)
      return true;
  }
  return false;
}

static bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void set_tcp_nodelay(int fd) {
  const int yes = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

int PaxLoader::svc_net_udp_open(uint16_t port, int broadcast) {
  if (!app_socket_slot_free())
    return -1;
  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0)
    return -1;
  const int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (broadcast)
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    ESP_LOGW(TAG, "PAX UDP: cannot bind port %u (errno %d)", port, errno);
    ::close(fd);
    return -1;
  }
  remember_app_socket(fd);
  ESP_LOGI(TAG, "PAX UDP: port %u open%s", port, broadcast ? " (broadcast)" : "");
  return fd;
}

int PaxLoader::svc_net_tcp_connect(uint32_t ip, uint16_t port) {
  if (!app_socket_slot_free())
    return -1;
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  set_tcp_nodelay(fd);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = htonl(ip);
  if (!set_nonblocking(fd) ||
      (::connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof(to)) != 0 && errno != EINPROGRESS)) {
    ESP_LOGW(TAG, "PAX TCP: connect to %s:%u failed (errno %d)", inet_ntoa(to.sin_addr), port, errno);
    ::close(fd);
    return -1;
  }
  remember_app_socket(fd);
  ESP_LOGI(TAG, "PAX TCP: connecting to %s:%u", inet_ntoa(to.sin_addr), port);
  return fd;
}

int PaxLoader::svc_net_tcp_listen(uint16_t port) {
  if (!app_socket_slot_free())
    return -1;
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  const int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0 ||
      !set_nonblocking(fd)) {
    ESP_LOGW(TAG, "PAX TCP: cannot listen on port %u (errno %d)", port, errno);
    ::close(fd);
    return -1;
  }
  remember_app_socket(fd);
  ESP_LOGI(TAG, "PAX TCP: listening on port %u", port);
  return fd;
}

int PaxLoader::svc_net_tcp_accept(int handle, uint32_t *ip, uint16_t *port) {
  if (!is_app_socket(handle))
    return -1;
  sockaddr_in from{};
  socklen_t from_len = sizeof(from);
  const int fd = ::accept(handle, reinterpret_cast<sockaddr *>(&from), &from_len);
  if (fd < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN) ? -2 : -1;
  if (!app_socket_slot_free() || !set_nonblocking(fd)) {
    ::close(fd);
    return -1;
  }
  set_tcp_nodelay(fd);
  remember_app_socket(fd);
  if (ip != nullptr)
    *ip = ntohl(from.sin_addr.s_addr);
  if (port != nullptr)
    *port = ntohs(from.sin_port);
  ESP_LOGI(TAG, "PAX TCP: accepted %s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
  return fd;
}

int PaxLoader::svc_net_tcp_send(int handle, const void *buf, int len) {
  if (!is_app_socket(handle) || buf == nullptr || len < 0)
    return -1;
  const int sent = ::send(handle, buf, len, 0);
  if (sent < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOMEM) ? 0 : -1;
  return sent;
}

int PaxLoader::svc_net_tcp_recv(int handle, void *buf, int len) {
  if (!is_app_socket(handle) || buf == nullptr || len <= 0)
    return -1;
  const int got = ::recv(handle, buf, len, 0);
  if (got < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN) ? -2 : -1;
  return got;
}

static constexpr int APP_TLS_HANDLE_BASE = 0x10000;  // above every lwIP descriptor
static int poll_app_tls(int handle);

int PaxLoader::svc_net_poll(int handle) {
  if (handle >= APP_TLS_HANDLE_BASE)
    return poll_app_tls(handle);
  if (!is_app_socket(handle))
    return -1;
  fd_set readable, writable, failed;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  FD_SET(handle, &readable);
  FD_SET(handle, &writable);
  FD_SET(handle, &failed);
  timeval now{0, 0};
  if (::select(handle + 1, &readable, &writable, &failed, &now) < 0)
    return -1;
  int error = 0;
  socklen_t error_len = sizeof(error);
  ::getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &error_len);
  return (FD_ISSET(handle, &readable) ? 1 : 0) | (FD_ISSET(handle, &writable) ? 2 : 0) |
         (FD_ISSET(handle, &failed) || error != 0 ? 4 : 0);
}

// ESP-IDF can retain DNS on the interface after a DHCP restart while its
// global resolver slots are empty. Reuse the active interface's own servers;
// never replace a configured resolver or introduce a different DNS provider.
static bool restore_active_dns_if_missing_() {
#ifdef CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
  esp_netif_t *netif = esp_netif_get_default_netif();
  esp_netif_ip_info_t ip{};
  if (netif == nullptr || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0)
    return false;
  for (int slot = ESP_NETIF_DNS_MAIN; slot <= ESP_NETIF_DNS_FALLBACK; ++slot) {
    esp_netif_dns_info_t dns{};
    if (esp_netif_get_dns_info(nullptr, static_cast<esp_netif_dns_type_t>(slot), &dns) != ESP_OK ||
        !ESP_IP_IS_ANY(dns.ip)) return false;
  }
  bool restored = false;
  for (int slot = ESP_NETIF_DNS_MAIN; slot <= ESP_NETIF_DNS_FALLBACK; ++slot) {
    esp_netif_dns_info_t dns{};
    if (esp_netif_get_dns_info(netif, static_cast<esp_netif_dns_type_t>(slot), &dns) == ESP_OK &&
        !ESP_IP_IS_ANY(dns.ip) &&
        esp_netif_set_dns_info(nullptr, static_cast<esp_netif_dns_type_t>(slot), &dns) == ESP_OK)
      restored = true;
  }
  if (restored) ESP_LOGI(TAG, "PAX DNS: restored resolver servers from active interface");
  return restored;
#else
  return false;
#endif
}

int PaxLoader::svc_net_resolve(const char *host, uint32_t *ip) {
  if (host == nullptr || *host == '\0' || ip == nullptr)
    return 0;
  in_addr parsed{};
  if (inet_aton(host, &parsed) != 0) {
    *ip = ntohl(parsed.s_addr);
    return 1;
  }
  (void)restore_active_dns_if_missing_();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *found = nullptr;
  const int lookup_result = getaddrinfo(host, nullptr, &hints, &found);
  if (lookup_result != 0 || found == nullptr) {
#ifdef CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
    esp_netif_dns_info_t global_dns{}, interface_dns{};
    esp_netif_get_dns_info(nullptr, ESP_NETIF_DNS_MAIN, &global_dns);
    esp_netif_get_dns_info(esp_netif_get_default_netif(), ESP_NETIF_DNS_MAIN, &interface_dns);
    ESP_LOGW(TAG, "PAX DNS: interface=%08x resolver=%08x", static_cast<unsigned>(ntohl(interface_dns.ip.u_addr.ip4.addr)),
             static_cast<unsigned>(ntohl(global_dns.ip.u_addr.ip4.addr)));
#endif
    ESP_LOGW(TAG, "PAX DNS: %s lookup failed (result=%d, errno=%d, free_internal=%u)",
             host, lookup_result, errno, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    if (found != nullptr) freeaddrinfo(found);
    return 0;
  }
  *ip = ntohl(reinterpret_cast<sockaddr_in *>(found->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(found);
  return 1;
}

int PaxLoader::svc_net_udp_send(int handle, const void *buf, int len, uint32_t ip, uint16_t port) {
  if (handle < 0 || buf == nullptr || len < 0)
    return -1;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = htonl(ip);
  const int sent = ::sendto(handle, buf, len, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
  if (sent < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOMEM) ? 0 : -1;
  return sent;
}

int PaxLoader::svc_net_udp_recv(int handle, void *buf, int len, uint32_t *ip, uint16_t *port) {
  if (handle < 0 || buf == nullptr || len <= 0)
    return -1;
  sockaddr_in from{};
  socklen_t from_len = sizeof(from);
  const int got = ::recvfrom(handle, buf, len, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
  if (got < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
  if (ip != nullptr)
    *ip = ntohl(from.sin_addr.s_addr);
  if (port != nullptr)
    *port = ntohs(from.sin_port);
  return got;
}

void PaxLoader::svc_net_udp_close(int handle) {
  for (int &fd : s_app_sockets) {
    if (fd == handle && fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
}

int PaxLoader::svc_net_ipv4(uint32_t *ip, uint32_t *netmask) {
  esp_netif_t *netif = esp_netif_get_default_netif();
  esp_netif_ip_info_t info{};
  if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0)
    return 0;
  if (ip != nullptr)
    *ip = ntohl(info.ip.addr);
  if (netmask != nullptr)
    *netmask = ntohl(info.netmask.addr);
  return 1;
}

int PaxLoader::svc_network_config_get(pax_network_config_t *out) {
  if (out == nullptr)
    return -1;
  std::memset(out, 0, sizeof(*out));
  esp_netif_t *netif = esp_netif_get_default_netif();
  if (netif == nullptr)
    return -1;

#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr && ethernet::global_eth_component->is_connected())
    out->network_type = PAX_NETWORK_ETHERNET;
#endif
#ifdef USE_WIFI
  if (out->network_type == PAX_NETWORK_NONE && wifi::global_wifi_component != nullptr &&
      wifi::global_wifi_component->is_connected())
    out->network_type = PAX_NETWORK_WIFI;
#endif

  esp_netif_ip_info_t ip{};
  if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
    out->ipv4 = ntohl(ip.ip.addr);
    out->netmask = ntohl(ip.netmask.addr);
    out->gateway = ntohl(ip.gw.addr);
    out->connected = ip.ip.addr != 0;
  }
  esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;
  if (esp_netif_dhcpc_get_status(netif, &dhcp_status) == ESP_OK)
    out->dhcp_enabled = dhcp_status == ESP_NETIF_DHCP_STARTED;

  esp_netif_dns_info_t dns{};
  if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
      dns.ip.type == IPADDR_TYPE_V4)
    out->dns = ntohl(dns.ip.u_addr.ip4.addr);
  return 0;
}

int PaxLoader::svc_network_config_set(const pax_network_config_t *config) {
  if (config == nullptr || PaxLoader::active_ == nullptr)
    return -1;
  esp_netif_t *netif = esp_netif_get_default_netif();
  if (netif == nullptr)
    return -2;

  if (!config->dhcp_enabled) {
    if (config->ipv4 == 0 || config->netmask == 0)
      return -1;
    const uint32_t inverse_mask = ~config->netmask;
    if ((inverse_mask & (inverse_mask + 1U)) != 0)
      return -1;
  }

  esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;
  esp_err_t err = esp_netif_dhcpc_get_status(netif, &dhcp_status);
  if (err != ESP_OK)
    return -1;
  // Reapplying/restoring DHCP must not clear the lease and DNS servers on
  // an interface whose DHCP client is already running.
  if (!(config->dhcp_enabled && dhcp_status == ESP_NETIF_DHCP_STARTED)) {
    if (dhcp_status == ESP_NETIF_DHCP_STARTED) {
      err = esp_netif_dhcpc_stop(netif);
      if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
        return -1;
    }

    esp_netif_ip_info_t ip{};
    if (!config->dhcp_enabled) {
      ip.ip.addr = htonl(config->ipv4);
      ip.netmask.addr = htonl(config->netmask);
      ip.gw.addr = htonl(config->gateway);
    }
    err = esp_netif_set_ip_info(netif, &ip);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set IPv4 information: %s", esp_err_to_name(err));
      return -1;
    }

    if (config->dhcp_enabled) {
      err = esp_netif_dhcpc_start(netif);
      if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "Failed to restart DHCP client: %s", esp_err_to_name(err));
        return -1;
      }
    } else if (config->dns != 0) {
      esp_netif_dns_info_t dns{};
      dns.ip.type = IPADDR_TYPE_V4;
      dns.ip.u_addr.ip4.addr = htonl(config->dns);
      err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set DNS server: %s", esp_err_to_name(err));
        return -1;
      }
    }
  }
  (void)restore_active_dns_if_missing_();

  SavedNetworkConfig saved{};
  saved.magic = NETWORK_PREFERENCE_MAGIC;
  saved.dhcp_enabled = config->dhcp_enabled != 0;
  saved.ipv4 = config->ipv4;
  saved.netmask = config->netmask;
  saved.gateway = config->gateway;
  saved.dns = config->dns;
  if (esphome::global_preferences == nullptr)
    return -3;
  auto preference = esphome::global_preferences->make_preference<SavedNetworkConfig>(
      NETWORK_PREFERENCE_KEY, true);
  if (!preference.save(&saved) || !esphome::global_preferences->sync()) {
    ESP_LOGE(TAG, "Network config applied but could not persist it to preferences");
    return -3;
  }
  return 0;
}

static bool storage_path_accessible_(const char *path) {
  DIR *directory = opendir(path);
  if (directory == nullptr)
    return false;
  closedir(directory);
  return true;
}

int PaxLoader::svc_storage_status_get(int32_t storage, pax_storage_status_t *out) {
  if (out == nullptr || storage < 0 || storage >= PAX_STORAGE_COUNT ||
      PaxLoader::active_ == nullptr)
    return -1;
  std::memset(out, 0, sizeof(*out));
  const char *path = storage == PAX_STORAGE_SD ? "/sdcard" : "/usb0";
  std::snprintf(out->mount_path, sizeof(out->mount_path), "%s", path);

  bool present = false;
  bool mounted = false;
  if (storage == PAX_STORAGE_SD) {
    mounted = storage_path_accessible_(path);
    present = mounted;
  } else {
#ifdef PAX_LOADER_USE_USB_MSC
    if (PaxLoader::active_->usb_msc_ != nullptr) {
      present = PaxLoader::active_->usb_msc_->is_connected();
      mounted = PaxLoader::active_->usb_msc_->is_mounted() && storage_path_accessible_(path);
    }
#endif
  }

  out->available = present;
  out->mounted = mounted;
  if (out->mounted) {
    const esp_err_t err = esp_vfs_fat_info(path, &out->total_bytes, &out->free_bytes);
    if (err != ESP_OK) {
      out->total_bytes = 0;
      out->free_bytes = 0;
      ESP_LOGW(TAG, "Storage capacity unavailable at %s: %s", path, esp_err_to_name(err));
    }
  }
  return 0;
}

int PaxLoader::svc_storage_set_mounted(int32_t storage, int32_t mounted) {
  // A transfer owns files on the volume. Never unmount or remount underneath
  // its install/upload task, including when the launcher has been reopened.
  if (pax_c6::busy() || pax_firmware::busy() || (PaxLoader::active_ != nullptr &&
      (PaxLoader::active_->install_task_handle_ != nullptr || PaxLoader::active_->web_storage_busy_))) return -4;
  if (storage < 0 || storage >= PAX_STORAGE_COUNT || PaxLoader::active_ == nullptr)
    return -1;
  const char *path = storage == PAX_STORAGE_SD ? "/sdcard" : "/usb0";
  if (mounted) {
    if (storage_path_accessible_(path))
      return 0;
    if (storage == PAX_STORAGE_SD) {
#ifdef PAX_LOADER_USE_SD_MMC
      if (PaxLoader::active_->sd_mmc_ == nullptr)
        return -2;
      PaxLoader::active_->sd_mmc_->mount();
#else
      return -2;
#endif
    } else {
#ifdef PAX_LOADER_USE_USB_MSC
      auto *usb = PaxLoader::active_->usb_msc_;
      if (usb == nullptr)
        return -2;
      if (!usb->is_connected())
        return -2;
      if (!usb->mount())
        return -3;
#else
      return -2;
#endif
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (storage == PAX_STORAGE_USB) {
#ifdef PAX_LOADER_USE_USB_MSC
      auto *usb = PaxLoader::active_->usb_msc_;
      return usb != nullptr && usb->is_mounted() && storage_path_accessible_(path) ? 0 : -1;
#else
      return -2;
#endif
    }
    return storage_path_accessible_(path) ? 0 : -1;
  }

  if (!storage_path_accessible_(path))
    return 0;
  if (storage == PAX_STORAGE_SD) {
#ifdef PAX_LOADER_USE_SD_MMC
    if (PaxLoader::active_->sd_mmc_ == nullptr)
      return -2;
    PaxLoader::active_->sd_mmc_->unmount();
#else
    return -2;
#endif
  } else {
#ifdef PAX_LOADER_USE_USB_MSC
    if (PaxLoader::active_->usb_msc_ == nullptr)
      return -2;
    PaxLoader::active_->usb_msc_->unmount();
#else
    return -2;
#endif
  }
  vTaskDelay(pdMS_TO_TICKS(100));
  if (storage == PAX_STORAGE_USB) {
#ifdef PAX_LOADER_USE_USB_MSC
    auto *usb = PaxLoader::active_->usb_msc_;
    return usb != nullptr && !usb->is_mounted() ? 0 : -1;
#else
    return -2;
#endif
  }
  return storage_path_accessible_(path) ? -1 : 0;
}

void PaxLoader::close_app_sockets_() {
  for (int &fd : s_app_sockets) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
}

// ── TLS (HTTPS) connections for apps ────────────────────────────────────────
// esp-tls does the work. Name lookup, the TCP connect and the handshake (the
// server's certificate checked against the firmware's CA bundle, SNI and the
// expected name from `host`) run on a short-lived task per connection, so the
// app's task never waits on DNS, the network or the handshake's crypto. Once
// ready, the socket is switched to non-blocking and the app's calls read and
// write through esp_tls_conn_read/write, which return at once.
//
// Each session costs mbedTLS's record buffers (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
// and _OUT_CONTENT_LEN, 16 KB and 4 KB by default) plus its context, about
// 25 KB of internal RAM, more during the handshake, and an lwIP socket (the
// component reserves APP_TLS_MAX in __init__.py).
static constexpr int APP_TLS_MAX = 4;
static constexpr int APP_TLS_TIMEOUT_MS = 15000;
static constexpr uint32_t APP_TLS_TASK_STACK = 8192;
static constexpr size_t APP_TLS_HOST_MAX = 253;  // the longest DNS name

enum AppTlsState : int8_t { TLS_CONNECTING = 0, TLS_READY = 1, TLS_FAILED = -1, TLS_CLOSED = 2 };

struct AppTlsSession {
  int handle{-1};         // the app's handle; -1 when the app holds none
  bool worker{false};     // a connect task still owns this slot
  AppTlsState state{TLS_FAILED};
  esp_tls_t *tls{nullptr};
};

struct AppTlsJob {
  int slot;
  int handle;
  uint16_t port;
  char host[APP_TLS_HOST_MAX + 1];
};

static AppTlsSession s_app_tls[APP_TLS_MAX];
static SemaphoreHandle_t s_app_tls_lock = nullptr;  // created by populate_services
static uint32_t s_app_tls_generation = 0;

// Every session change and every esp_tls call on an app's session happens
// under s_app_tls_lock, so a close from one task cannot free a session another
// is reading. The calls under it do not block (the sockets are non-blocking).
class AppTlsLock {
 public:
  explicit AppTlsLock(TickType_t wait = pdMS_TO_TICKS(1000)) : lock_(s_app_tls_lock) {
    if (this->lock_ != nullptr && xSemaphoreTake(this->lock_, wait) != pdTRUE)
      this->lock_ = nullptr;
  }
  ~AppTlsLock() {
    if (this->lock_ != nullptr)
      xSemaphoreGive(this->lock_);
  }
  AppTlsLock(const AppTlsLock &) = delete;
  AppTlsLock &operator=(const AppTlsLock &) = delete;
  bool held() const { return this->lock_ != nullptr; }

 private:
  SemaphoreHandle_t lock_;  // the lock taken (close_app_tls_ may replace s_app_tls_lock)
};

static AppTlsSession *find_app_tls(int handle) {
  if (handle < APP_TLS_HANDLE_BASE)
    return nullptr;
  for (auto &session : s_app_tls) {
    if (session.handle == handle)
      return &session;
  }
  return nullptr;
}

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
static void app_tls_connect_task(void *arg) {
  auto *job = static_cast<AppTlsJob *>(arg);
  const int64_t started_us = esp_timer_get_time();
  esp_tls_t *tls = nullptr;
  bool ready = false;
  uint32_t ip = 0;
  if (!PaxLoader::svc_net_resolve(job->host, &ip)) {
    ESP_LOGW(TAG, "PAX TLS: %s not found", job->host);
  } else if ((tls = esp_tls_init()) == nullptr) {
    ESP_LOGW(TAG, "PAX TLS: out of memory for %s", job->host);
  } else {
    char address[16];
    std::snprintf(address, sizeof(address), "%u.%u.%u.%u", static_cast<unsigned>(ip >> 24),
                  static_cast<unsigned>((ip >> 16) & 0xFF), static_cast<unsigned>((ip >> 8) & 0xFF),
                  static_cast<unsigned>(ip & 0xFF));
    esp_tls_cfg_t cfg{};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.common_name = job->host;  // SNI and the name the certificate must carry
    cfg.timeout_ms = APP_TLS_TIMEOUT_MS;
    int fd = -1;
    if (esp_tls_conn_new_sync(address, static_cast<int>(std::strlen(address)), job->port, &cfg, tls) == 1 &&
        esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK && fd >= 0 && set_nonblocking(fd)) {
      ready = true;
      ESP_LOGI(TAG, "PAX TLS: %s:%u ready (%lld ms)", job->host, job->port,
               static_cast<long long>((esp_timer_get_time() - started_us) / 1000));
    } else {
      esp_tls_error_handle_t error = nullptr;
      if (esp_tls_get_error_handle(tls, &error) == ESP_OK && error != nullptr) {
        ESP_LOGW(TAG, "PAX TLS: %s:%u failed: %s (TLS error -0x%04x, certificate flags 0x%x)", job->host, job->port,
                 esp_err_to_name(error->last_error), static_cast<unsigned>(-error->esp_tls_error_code),
                 static_cast<unsigned>(error->esp_tls_flags));
      } else {
        ESP_LOGW(TAG, "PAX TLS: %s:%u failed", job->host, job->port);
      }
    }
  }

  bool kept = false;
  for (;;) {
    AppTlsLock lock;  // retried, so a lock replaced by close_app_tls_ is picked up
    if (!lock.held())
      continue;
    AppTlsSession &session = s_app_tls[job->slot];
    session.worker = false;
    if (session.handle == job->handle) {  // the app has not closed it meanwhile
      session.state = ready ? TLS_READY : TLS_FAILED;
      if (ready) {
        session.tls = tls;
        kept = true;
      }
    }
    break;
  }
  if (!kept && tls != nullptr)
    esp_tls_conn_destroy(tls);
  std::free(job);
  vTaskDelete(nullptr);
}
#endif

int PaxLoader::svc_net_tls_connect(const char *host, uint16_t port) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (host == nullptr || *host == '\0' || port == 0 || std::strlen(host) > APP_TLS_HOST_MAX)
    return -1;
  auto *job = static_cast<AppTlsJob *>(std::calloc(1, sizeof(AppTlsJob)));
  if (job == nullptr)
    return -1;
  AppTlsLock lock;
  int slot = -1;
  for (int i = 0; lock.held() && i < APP_TLS_MAX; i++) {
    if (s_app_tls[i].handle < 0 && !s_app_tls[i].worker && s_app_tls[i].tls == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    ESP_LOGW(TAG, "PAX TLS: no free session for %s (at most %d)", host, APP_TLS_MAX);
    std::free(job);
    return -1;
  }
  // A new number each time, so a stale handle never reaches a later session.
  s_app_tls_generation = (s_app_tls_generation + 1) & 0xFFFF;
  const int handle = APP_TLS_HANDLE_BASE + static_cast<int>(s_app_tls_generation) * APP_TLS_MAX + slot;
  job->slot = slot;
  job->handle = handle;
  job->port = port;
  std::memcpy(job->host, host, std::strlen(host) + 1);
  AppTlsSession &session = s_app_tls[slot];
  session.handle = handle;
  session.worker = true;
  session.state = TLS_CONNECTING;
  session.tls = nullptr;
  if (xTaskCreatePinnedToCore(&app_tls_connect_task, "pax_tls", APP_TLS_TASK_STACK, job, 5, nullptr,
                              tskNO_AFFINITY) != pdPASS) {
    ESP_LOGW(TAG, "PAX TLS: cannot start a connect task for %s", host);
    session = AppTlsSession{};
    std::free(job);
    return -1;
  }
  ESP_LOGI(TAG, "PAX TLS: connecting to %s:%u", host, port);
  return handle;
#else
  ESP_LOGW(TAG, "PAX TLS: unavailable, this firmware has no CA bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)");
  return -1;
#endif
}

int PaxLoader::svc_net_tls_status(int handle) {
  AppTlsLock lock;
  const AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
  if (session == nullptr)
    return -1;
  switch (session->state) {
    case TLS_CONNECTING:
      return 0;
    case TLS_READY:
    case TLS_CLOSED:
      return 1;
    default:
      return -1;
  }
}

int PaxLoader::svc_net_tls_send(int handle, const void *buf, int len) {
  if (buf == nullptr || len < 0)
    return -1;
  AppTlsLock lock;
  AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
  if (session == nullptr)
    return -1;
  if (session->state == TLS_CONNECTING)
    return 0;
  if (session->state != TLS_READY || session->tls == nullptr)
    return -1;
  if (len == 0)
    return 0;
  const ssize_t sent = esp_tls_conn_write(session->tls, buf, static_cast<size_t>(len));
  if (sent > 0)
    return static_cast<int>(sent);
  if (sent == 0 || sent == ESP_TLS_ERR_SSL_WANT_WRITE || sent == ESP_TLS_ERR_SSL_WANT_READ)
    return 0;
  ESP_LOGW(TAG, "PAX TLS: send failed (-0x%04x)", static_cast<unsigned>(-sent));
  session->state = TLS_FAILED;
  return -1;
}

int PaxLoader::svc_net_tls_recv(int handle, void *buf, int len) {
  if (buf == nullptr || len <= 0)
    return -1;
  AppTlsLock lock;
  AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
  if (session == nullptr)
    return -1;
  if (session->state == TLS_CONNECTING)
    return 0;
  if (session->state != TLS_READY || session->tls == nullptr)
    return -1;
  const ssize_t got = esp_tls_conn_read(session->tls, buf, static_cast<size_t>(len));
  if (got > 0)
    return static_cast<int>(got);
  if (got == ESP_TLS_ERR_SSL_WANT_READ || got == ESP_TLS_ERR_SSL_WANT_WRITE)
    return 0;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
  if (got == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)  // TLS 1.3 housekeeping, no data
    return 0;
#endif
  // 0 is the server's close_notify; a plain TCP close without one ends the
  // stream too (HTTP servers often do that after the response).
  bool closed = got == 0;
#ifdef MBEDTLS_ERR_SSL_CONN_EOF
  closed = closed || got == MBEDTLS_ERR_SSL_CONN_EOF;
#endif
#ifdef MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY
  closed = closed || got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
#endif
  if (!closed)
    ESP_LOGW(TAG, "PAX TLS: receive failed (-0x%04x)", static_cast<unsigned>(-got));
  session->state = closed ? TLS_CLOSED : TLS_FAILED;
  return -1;
}

void PaxLoader::svc_net_tls_close(int handle) {
  esp_tls_t *tls = nullptr;
  {
    AppTlsLock lock;
    AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
    if (session == nullptr)
      return;
    // A connect task still running sees the handle gone and frees its own
    // connection; the slot stays taken until then.
    tls = session->tls;
    session->handle = -1;
    session->tls = nullptr;
    session->state = TLS_FAILED;
  }
  if (tls != nullptr)
    esp_tls_conn_destroy(tls);
}

// net_poll for a TLS handle; see psram_app.h.
static int poll_app_tls(int handle) {
  AppTlsLock lock;
  const AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
  if (session == nullptr)
    return -1;
  switch (session->state) {
    case TLS_CONNECTING:
      return 0;
    case TLS_CLOSED:
      return 1;  // net_tls_recv reports the close
    case TLS_READY:
      break;
    default:
      return 4;
  }
  int fd = -1;
  if (session->tls == nullptr || esp_tls_get_conn_sockfd(session->tls, &fd) != ESP_OK || fd < 0)
    return 4;
  int ready = esp_tls_get_bytes_avail(session->tls) > 0 ? 1 : 0;
  fd_set readable, writable, failed;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  FD_SET(fd, &readable);
  FD_SET(fd, &writable);
  FD_SET(fd, &failed);
  timeval now{0, 0};
  if (::select(fd + 1, &readable, &writable, &failed, &now) < 0)
    return ready | 4;
  return ready | (FD_ISSET(fd, &readable) ? 1 : 0) | (FD_ISSET(fd, &writable) ? 2 : 0) |
         (FD_ISSET(fd, &failed) ? 4 : 0);
}

void PaxLoader::close_app_tls_() {
  if (s_app_tls_lock == nullptr)
    return;
  esp_tls_t *open[APP_TLS_MAX] = {};
  {
    AppTlsLock lock(pdMS_TO_TICKS(2000));
    if (!lock.held()) {
      // Only a task deleted inside a TLS call leaves the lock taken. Give up on
      // its sessions (their memory is lost, the rest works) and start a new
      // lock, so later apps still get TLS. Connect tasks still running find
      // their handle gone and free their own connections.
      ESP_LOGW(TAG, "PAX TLS: lock still taken after the app exited; abandoning its sessions");
      for (auto &session : s_app_tls) {
        session.handle = -1;
        session.tls = nullptr;
        session.state = TLS_FAILED;
      }
      s_app_tls_lock = xSemaphoreCreateMutex();
      return;
    }
    for (int i = 0; i < APP_TLS_MAX; i++) {
      AppTlsSession &session = s_app_tls[i];
      if (session.handle >= 0)
        ESP_LOGI(TAG, "PAX TLS: closing a session the app left open");
      open[i] = session.tls;
      session.handle = -1;
      session.tls = nullptr;
      session.state = TLS_FAILED;
    }
  }
  for (esp_tls_t *tls : open) {
    if (tls != nullptr)
      esp_tls_conn_destroy(tls);
  }
}

// ── App hand-off (app_open, app_get_arg, app_set_resume_arg) ────────────────
// app_open runs on the calling app's task: it finds the target, queues the
// request in chain_ and asks the app to close. finish_app_ (the main loop)
// then takes the next launch from chain_ (continue_chain_).

// An HTTP catalog page fetched for app_open on a task of the loader's own (the
// calling app's stack may be small or in PSRAM). The job outlives a caller
// that stops waiting.
struct CatalogLookup {
  std::string url;
  std::string html;
  esp_err_t result{ESP_FAIL};
  volatile bool done{false};
};

static void catalog_lookup_task(void *arg) {
  auto *slot = static_cast<std::shared_ptr<CatalogLookup> *>(arg);
  std::shared_ptr<CatalogLookup> job = std::move(*slot);
  delete slot;
  job->result = fetch_http_text(job->url.c_str(), &job->html);
  job->done = true;
  job.reset();
  vTaskDelete(nullptr);
}

static constexpr int64_t CATALOG_LOOKUP_TIMEOUT_US = 20000000;

std::vector<std::string> PaxLoader::folder_paxs_(const std::string &folder) const {
  std::vector<std::string> paxs;
  for (const auto &place : this->catalog_folders_(folder)) {
    DIR *dir = opendir(runtime_path(place.second.c_str()).c_str());
    if (dir == nullptr)
      continue;
    while (dirent *entry = readdir(dir)) {
      const std::string file = entry->d_name;
      if (handoff::is_pax_file(file))
        paxs.push_back(place.second + file);
    }
    closedir(dir);
  }
  return paxs;
}

std::string PaxLoader::resolve_app_(const std::string &name) {
  // The installed copy, as the store's Launch button prefers it.
  const std::string installed = this->install_dir_ + "/" + name + ".pax";
  struct stat st{};
  if (stat(runtime_path(installed.c_str()).c_str(), &st) == 0 && S_ISREG(st.st_mode))
    return installed;
  // Then the library sources in order. catalogs_ and a lone catalog_url_ are
  // only set up from YAML, so they are safe to read from the app's task.
  std::vector<std::string> sources;
  for (const auto &catalog : this->catalogs_)
    sources.push_back(catalog.url);
  if (sources.empty() && !this->catalog_url_.empty())
    sources.push_back(this->catalog_url_);
  for (const auto &source : sources) {
    std::vector<std::string> candidates;
    if (!source.empty() && source[0] == '/') {
      candidates = this->folder_paxs_(source);
    } else if (is_network_url(source.c_str()) && network::is_connected()) {
      auto job = std::make_shared<CatalogLookup>();
      job->url = source;
      auto *slot = new std::shared_ptr<CatalogLookup>(job);
      if (xTaskCreatePinnedToCore(&catalog_lookup_task, "pax_lookup", 8192, slot, 4, nullptr, 0) != pdPASS) {
        delete slot;
        ESP_LOGW(TAG, "App hand-off: could not start the catalog lookup for %s", source.c_str());
        continue;
      }
      const int64_t start = esp_timer_get_time();
      while (!job->done && esp_timer_get_time() - start < CATALOG_LOOKUP_TIMEOUT_US)
        vTaskDelay(pdMS_TO_TICKS(20));
      if (!job->done) {
        ESP_LOGW(TAG, "App hand-off: catalog %s did not answer in time", source.c_str());
        continue;
      }
      if (job->result != ESP_OK) {
        ESP_LOGW(TAG, "App hand-off: catalog %s failed: %s", source.c_str(), esp_err_to_name(job->result));
        continue;
      }
      for (const auto &entry : parse_pax_catalog(job->html, source))
        candidates.push_back(entry.second);
    }
    const std::string found = handoff::best_match(candidates, name);
    if (!found.empty())
      return found;
  }
  return {};
}

int PaxLoader::svc_app_open(const char *target, const char *arg, int return_after) {
  PaxLoader *self = active_;
  if (self == nullptr || !self->launched_ || self->handoff_mutex_ == nullptr || target == nullptr)
    return -1;
  const std::string wanted(target, strnlen(target, handoff::MAX_TARGET + 1));
  const size_t arg_len = arg != nullptr ? strnlen(arg, handoff::MAX_ARG + 1) : 0;
  if (arg_len > handoff::MAX_ARG) {
    ESP_LOGW(TAG, "App hand-off to %.64s refused: the argument is longer than %u bytes", wanted.c_str(),
             static_cast<unsigned>(handoff::MAX_ARG));
    return -1;
  }
  if (self->global_close_requested_ || self->toggle_close_requested_) {
    ESP_LOGW(TAG, "App hand-off to %.64s refused: the app is already closing", wanted.c_str());
    return -1;
  }
  std::string source;
  switch (handoff::classify(wanted)) {
    case handoff::Target::URL:
      if (network::is_connected())
        source = wanted;
      else
        ESP_LOGW(TAG, "App hand-off to %s refused: the device is offline", wanted.c_str());
      break;
    case handoff::Target::PATH: {
      struct stat st{};
      if (stat(runtime_path(wanted.c_str()).c_str(), &st) == 0 && S_ISREG(st.st_mode))
        source = wanted;
      else
        ESP_LOGW(TAG, "App hand-off: %s not found", wanted.c_str());
      break;
    }
    case handoff::Target::NAME:
      source = self->resolve_app_(wanted);
      if (source.empty())
        ESP_LOGW(TAG, "App hand-off: no app %s installed or in the library sources", wanted.c_str());
      break;
    default:
      ESP_LOGW(TAG, "App hand-off refused: '%.64s' is not an app name, .pax URL or /sd path", wanted.c_str());
      break;
  }
  if (source.empty())
    return -1;

  bool accepted = false;
  if (xSemaphoreTake(self->handoff_mutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
    const handoff::Frame caller{self->running_source_, self->resume_arg_};
    const handoff::Frame next{source, arg_len > 0 ? std::string(arg, arg_len) : std::string()};
    accepted = self->chain_.request(caller, next, return_after != 0);
    xSemaphoreGive(self->handoff_mutex_);
  }
  if (!accepted) {
    ESP_LOGW(TAG, "App hand-off to %s refused: another one is under way, or %u apps already wait to be returned to",
             source.c_str(), static_cast<unsigned>(handoff::MAX_DEPTH));
    return -1;
  }
  ESP_LOGI(TAG, "App hand-off: %s opens %s%s; closing it", self->running_source_.c_str(), source.c_str(),
           return_after != 0 ? " and comes back afterwards" : "");
  self->begin_close_();
  return 0;
}

int PaxLoader::svc_app_get_arg(char *buf, int len) {
  PaxLoader *self = active_;
  // launch_arg_ is set before the app starts and not changed while it runs.
  return handoff::copy_arg(self != nullptr ? self->launch_arg_ : std::string(), buf, len);
}

int PaxLoader::svc_app_set_resume_arg(const char *arg) {
  PaxLoader *self = active_;
  const size_t len = arg != nullptr ? strnlen(arg, handoff::MAX_ARG + 1) : 0;
  if (self == nullptr || self->handoff_mutex_ == nullptr || len > handoff::MAX_ARG)
    return -1;
  if (xSemaphoreTake(self->handoff_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE)
    return -1;
  self->resume_arg_.assign(arg != nullptr ? arg : "", len);
  xSemaphoreGive(self->handoff_mutex_);
  return 0;
}

int PaxLoader::svc_file_list_dir(const char *path, char *buf, int len) {
  if (path == nullptr)
    return -1;
  const std::string folder = runtime_path(path);
  DIR *dir = opendir(folder.c_str());
  if (dir == nullptr)
    return -1;
  int count = 0;
  size_t used = 0;
  const size_t size = buf != nullptr && len > 0 ? static_cast<size_t>(len) : 0;
  while (dirent *entry = readdir(dir)) {
    const char *name = entry->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
      continue;
    bool is_dir = entry->d_type == DT_DIR;
    if (entry->d_type == DT_UNKNOWN) {
      struct stat st{};
      const std::string full = folder + (folder.empty() || folder.back() == '/' ? "" : "/") + name;
      is_dir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    const size_t need = std::strlen(name) + (is_dir ? 1 : 0) + 1;
    if (used + need > size)
      continue;  // does not fit; a shorter name later might
    std::memcpy(buf + used, name, need - 1 - (is_dir ? 1 : 0));
    used += need - 1 - (is_dir ? 1 : 0);
    if (is_dir)
      buf[used++] = '/';
    buf[used++] = '\0';
    count++;
  }
  closedir(dir);
  return count;
}

// ── File management services ───────────────────────────────────────────────
// The card (/sd) and the loader's data roots only; pax_files.h checks the path.

std::string PaxLoader::app_file_path_(const char *path, std::string *root) {
  PaxLoader *self = active_;
  if (self == nullptr)
    return {};
  std::vector<std::string> roots = self->data_roots_();
  roots.emplace_back("/sd");
  std::string clean, matched;
  if (!files::clean_app_path(path, roots, &clean, &matched)) {
    ESP_LOGW(TAG, "App file path refused: %.80s", path != nullptr ? path : "(null)");
    return {};
  }
  if (root != nullptr)
    *root = runtime_path(matched.c_str());
  return runtime_path(clean.c_str());
}

// A storage root is a mount point, which FAT cannot stat: it counts as a
// folder when it can be listed.
static bool stat_app_path(const std::string &fs_path, const std::string &root, struct stat *st) {
  if (fs_path != root)
    return stat(fs_path.c_str(), st) == 0;
  DIR *dir = opendir(fs_path.c_str());
  if (dir == nullptr)
    return false;
  closedir(dir);
  *st = {};
  st->st_mode = S_IFDIR;
  return true;
}

int PaxLoader::svc_file_mkdir(const char *path) {
  std::string root;
  const std::string fs_path = app_file_path_(path, &root);
  struct stat st{};
  if (fs_path.empty())
    return -1;
  if (fs_path == root)
    return stat_app_path(fs_path, root, &st) ? 0 : -1;
  // Each folder below the root, from the top down.
  for (size_t slash = fs_path.find('/', root.size() + 1);; slash = fs_path.find('/', slash + 1)) {
    const std::string part = slash == std::string::npos ? fs_path : fs_path.substr(0, slash);
    if (stat(part.c_str(), &st) == 0) {
      if (!S_ISDIR(st.st_mode))
        return -1;  // a file is in the way
    } else if (mkdir(part.c_str(), 0775) != 0 && errno != EEXIST) {
      ESP_LOGW(TAG, "App mkdir %s failed (errno %d)", part.c_str(), errno);
      return -1;
    }
    if (slash == std::string::npos)
      return 0;
  }
}

int PaxLoader::svc_file_remove(const char *path) {
  std::string root;
  const std::string fs_path = app_file_path_(path, &root);
  struct stat st{};
  if (fs_path.empty() || fs_path == root || stat(fs_path.c_str(), &st) != 0)
    return -1;
  const bool removed = S_ISDIR(st.st_mode) ? remove_empty_folder(fs_path) : unlink(fs_path.c_str()) == 0;
  if (!removed)
    ESP_LOGW(TAG, "App remove %s failed (errno %d)", fs_path.c_str(), errno);
  return removed ? 0 : -1;
}

int PaxLoader::svc_file_rename(const char *from, const char *to) {
  std::string from_root, to_root;
  const std::string source = app_file_path_(from, &from_root);
  const std::string target = app_file_path_(to, &to_root);
  struct stat st{};
  if (source.empty() || target.empty() || source == from_root || target == to_root ||
      stat(source.c_str(), &st) != 0)
    return -1;
  if (stat(target.c_str(), &st) == 0)
    return -1;  // never replaces: FAT refuses that anyway, so every file system behaves alike
  if (std::rename(source.c_str(), target.c_str()) != 0) {
    ESP_LOGW(TAG, "App rename %s -> %s failed (errno %d)", source.c_str(), target.c_str(), errno);
    return -1;
  }
  return 0;
}

int PaxLoader::svc_file_stat(const char *path, pax_file_stat_t *out) {
  if (out == nullptr)
    return -1;
  std::memset(out, 0, sizeof(*out));
  std::string root;
  const std::string fs_path = app_file_path_(path, &root);
  struct stat st{};
  if (fs_path.empty() || !stat_app_path(fs_path, root, &st))
    return -1;
  out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
  out->size = out->is_dir ? 0 : static_cast<uint64_t>(st.st_size);
  out->mtime = st.st_mtime > 0 ? static_cast<int64_t>(st.st_mtime) : 0;
  return 0;
}

static uint16_t zip_u16_(const unsigned char *p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

static uint32_t zip_u32_(const unsigned char *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static bool zip_extension_(const std::string &name, const char *extensions, std::string *out) {
  if (extensions == nullptr || out == nullptr || name.empty() || name.back() == '/')
    return false;
  const size_t slash = name.find_last_of("/\\");
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot <= slash))
    return false;
  std::string ext = name.substr(dot);
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::string list = extensions;
  for (char &c : list)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (list.find(ext + "|") == std::string::npos)
    return false;
  *out = std::move(ext);
  return true;
}

int PaxLoader::svc_file_zip_extract_first(const char *archive, const char *extensions,
                                           const char *cache_dir, const char *cache_stem,
                                           char *out_path, size_t out_size) {
  return svc_file_zip_extract_first_impl(archive, extensions, cache_dir, cache_stem,
                                         out_path, out_size, nullptr, nullptr);
}

int PaxLoader::svc_file_zip_extract_first_progress(
    const char *archive, const char *extensions, const char *cache_dir,
    const char *cache_stem, char *out_path, size_t out_size,
    pax_file_progress_fn_t progress, void *context) {
  return svc_file_zip_extract_first_impl(archive, extensions, cache_dir, cache_stem,
                                         out_path, out_size, progress, context);
}

int PaxLoader::svc_file_zip_extract_first_impl(
    const char *archive, const char *extensions, const char *cache_dir,
    const char *cache_stem, char *out_path, size_t out_size,
    pax_file_progress_fn_t progress, void *context) {
  ESP_LOGI(TAG, "ZIP ROM: extractor entered archive=%s extensions=%s cache=%s stem=%s",
           archive != nullptr ? archive : "(null)",
           extensions != nullptr ? extensions : "(null)",
           cache_dir != nullptr ? cache_dir : "(null)",
           cache_stem != nullptr ? cache_stem : "(null)");
  if (archive == nullptr || extensions == nullptr || cache_dir == nullptr ||
      cache_stem == nullptr || out_path == nullptr || out_size == 0 || cache_stem[0] == '\0' ||
      std::strchr(cache_stem, '/') != nullptr || std::strchr(cache_stem, '\\') != nullptr ||
      std::strstr(cache_stem, "..") != nullptr) {
    ESP_LOGI(TAG, "ZIP ROM: invalid extractor arguments");
    return -1;
  }

  const std::string archive_fs = app_file_path_(archive);
  const std::string cache_fs = app_file_path_(cache_dir);
  const int mkdir_result = cache_fs.empty() ? -1 : svc_file_mkdir(cache_dir);
  if (archive_fs.empty() || cache_fs.empty() || mkdir_result != 0) {
    ESP_LOGI(TAG, "ZIP ROM: path/cache setup failed archive=%s fs=%s cache=%s cache_fs=%s mkdir=%d",
             archive, archive_fs.c_str(), cache_dir, cache_fs.c_str(), mkdir_result);
    return -1;
  }

  FILE *zip = std::fopen(archive_fs.c_str(), "rb");
  if (zip == nullptr) {
    ESP_LOGI(TAG, "ZIP ROM: archive open failed %s (errno %d)", archive_fs.c_str(), errno);
    return -1;
  }

  int result = -1;
  const char *failure = "unknown";
  FILE *output = nullptr;
  std::string output_portable;
  std::string output_fs;
  std::string output_tmp_fs;
  do {
    if (std::fseek(zip, 0, SEEK_END) != 0) {
      failure = "seek end";
      break;
    }
    const long archive_size = std::ftell(zip);
    if (archive_size < 22) {
      failure = "archive too small";
      break;
    }

    // A classic ZIP end record is at most 65,557 bytes from EOF (22-byte
    // record plus the maximum 65,535-byte comment). ZIP64 is deliberately
    // not accepted here: console ROM archives are small and the launcher
    // does not need a second, much larger parser for them.
    const long tail_size = std::min<long>(archive_size, 22L + 65535L);
    std::vector<unsigned char> tail(static_cast<size_t>(tail_size));
    if (std::fseek(zip, archive_size - tail_size, SEEK_SET) != 0 ||
        std::fread(tail.data(), 1, tail.size(), zip) != tail.size()) {
      failure = "read archive tail";
      break;
    }
    long eocd = -1;
    for (long i = tail_size - 22; i >= 0; --i) {
      if (zip_u32_(tail.data() + i) == 0x06054b50U) {
        const uint16_t comment = zip_u16_(tail.data() + i + 20);
        if (i + 22L + comment <= tail_size) {
          eocd = i;
          break;
        }
      }
    }
    if (eocd < 0) {
      failure = "missing end record";
      break;
    }

    const uint16_t disk = zip_u16_(tail.data() + eocd + 4);
    const uint16_t central_disk = zip_u16_(tail.data() + eocd + 6);
    const uint16_t entries_disk = zip_u16_(tail.data() + eocd + 8);
    const uint16_t entries = zip_u16_(tail.data() + eocd + 10);
    const uint32_t central_size = zip_u32_(tail.data() + eocd + 12);
    const uint32_t central_offset = zip_u32_(tail.data() + eocd + 16);
    if (disk != 0 || central_disk != 0 || entries_disk != entries || entries == 0xffff ||
        central_size == 0xffffffffU || central_offset == 0xffffffffU ||
        static_cast<uint64_t>(central_offset) + central_size > static_cast<uint64_t>(archive_size)) {
      failure = "invalid central directory";
      break;
    }

    struct ZipEntry {
      uint16_t method = 0;
      uint32_t compressed = 0;
      uint32_t uncompressed = 0;
      uint32_t local_offset = 0;
      std::string extension;
    } selected;
    bool found = false;
    if (std::fseek(zip, static_cast<long>(central_offset), SEEK_SET) != 0) {
      failure = "seek central directory";
      break;
    }
    for (uint32_t index = 0; index < entries; ++index) {
      unsigned char central[46];
      if (std::fread(central, 1, sizeof(central), zip) != sizeof(central) ||
          zip_u32_(central) != 0x02014b50U)
        break;
      const uint16_t name_len = zip_u16_(central + 28);
      const uint16_t extra_len = zip_u16_(central + 30);
      const uint16_t comment_len = zip_u16_(central + 32);
      if (name_len == 0 || name_len > 4096)
        break;
      std::string name(name_len, '\0');
      if (std::fread(name.data(), 1, name.size(), zip) != name.size())
        break;
      if (extra_len != 0 && std::fseek(zip, extra_len, SEEK_CUR) != 0)
        break;
      if (comment_len != 0 && std::fseek(zip, comment_len, SEEK_CUR) != 0)
        break;
      if (found)
        continue;
      std::string extension;
      if (!zip_extension_(name, extensions, &extension))
        continue;
      const uint16_t flags = zip_u16_(central + 8);
      const uint16_t method = zip_u16_(central + 10);
      // Bit 3 means sizes are in a data descriptor, but the central
      // directory still contains the values we need. Encryption is not
      // supported because ROM ZIPs should not require a password.
      if ((flags & 1U) != 0 || (method != 0 && method != 8))
        continue;
      selected.method = method;
      selected.compressed = zip_u32_(central + 20);
      selected.uncompressed = zip_u32_(central + 24);
      selected.local_offset = zip_u32_(central + 42);
      selected.extension = std::move(extension);
      found = true;
    }
    if (!found) {
      failure = "no supported ROM entry";
      break;
    }
    if (progress != nullptr)
      progress(context, 0, selected.uncompressed);

    output_portable = std::string(cache_dir) + "/" + cache_stem + selected.extension;
    if (output_portable.size() + 1 > out_size) {
      failure = "output path too long";
      break;
    }
    output_fs = app_file_path_(output_portable.c_str());
    if (output_fs.empty()) {
      failure = "output path refused";
      break;
    }
    // The cache is disposable, but a reset must not leave a truncated ROM
    // under the final cache name.  Commit it only after the full member has
    // been flushed and synced.
    output_tmp_fs = output_fs + ".part";
    std::remove(output_tmp_fs.c_str());
    output = std::fopen(output_tmp_fs.c_str(), "wb");
    if (output == nullptr) {
      failure = "output open";
      break;
    }

    unsigned char local[30];
    if (std::fseek(zip, static_cast<long>(selected.local_offset), SEEK_SET) != 0 ||
        std::fread(local, 1, sizeof(local), zip) != sizeof(local) ||
        zip_u32_(local) != 0x04034b50U) {
      failure = "invalid local header";
      break;
    }
    const uint16_t local_name_len = zip_u16_(local + 26);
    const uint16_t local_extra_len = zip_u16_(local + 28);
    if (std::fseek(zip, static_cast<long>(local_name_len) + local_extra_len, SEEK_CUR) != 0) {
      failure = "seek ROM data";
      break;
    }

    uint64_t written = 0;
    std::vector<unsigned char> input(4096);
    std::vector<unsigned char> buffer(8192);
    if (selected.method == 0) {
      uint32_t remaining = selected.compressed;
      while (remaining != 0) {
        const size_t want = std::min<size_t>(input.size(), remaining);
        const size_t got = std::fread(input.data(), 1, want, zip);
        if (got == 0 || std::fwrite(input.data(), 1, got, output) != got)
          break;
        remaining -= static_cast<uint32_t>(got);
        written += got;
        if (progress != nullptr)
          progress(context, written, selected.uncompressed);
      }
      if (remaining != 0) {
        failure = "short stored ROM";
        break;
      }
    } else {
      z_stream stream{};
      if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        failure = "inflate init";
        break;
      }
      uint32_t remaining = selected.compressed;
      int inflate_result = Z_OK;
      bool ok = true;
      while (ok && inflate_result != Z_STREAM_END) {
        if (stream.avail_in == 0 && remaining != 0) {
          const size_t want = std::min<size_t>(input.size(), remaining);
          const size_t got = std::fread(input.data(), 1, want, zip);
          if (got == 0) {
            ok = false;
            break;
          }
          remaining -= static_cast<uint32_t>(got);
          stream.next_in = input.data();
          stream.avail_in = static_cast<uInt>(got);
        }
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        // This component carries a small zlib variant whose inflate() also
        // takes the optional CRC-check flag. ZIP's raw deflate stream has no
        // zlib wrapper, so there is no wrapper checksum to verify here.
        inflate_result = inflate(&stream, Z_NO_FLUSH, 0);
        const size_t produced = buffer.size() - stream.avail_out;
        if (produced != 0 && std::fwrite(buffer.data(), 1, produced, output) != produced)
          ok = false;
        written += produced;
        if (produced != 0 && progress != nullptr)
          progress(context, written, selected.uncompressed);
        if (inflate_result != Z_OK && inflate_result != Z_STREAM_END)
          ok = false;
        if (stream.avail_in == 0 && remaining == 0 && inflate_result == Z_OK)
          ok = false;
      }
      inflateEnd(&stream);
      if (!ok || inflate_result != Z_STREAM_END) {
        failure = "inflate data";
        break;
      }
    }
    if (written != selected.uncompressed || !sync_file_(output, output_tmp_fs.c_str())) {
      failure = "ROM size or flush";
      break;
    }
    if (progress != nullptr)
      progress(context, written, selected.uncompressed);
    if (std::fclose(output) != 0) {
      output = nullptr;
      failure = "ROM close";
      break;
    }
    output = nullptr;
    std::remove(output_fs.c_str());
    if (std::rename(output_tmp_fs.c_str(), output_fs.c_str()) != 0) {
      failure = "ROM cache commit";
      break;
    }
    warn_if_not_durable_(output_fs);
    std::memcpy(out_path, output_portable.c_str(), output_portable.size() + 1);
    ESP_LOGI(TAG, "ZIP ROM: %s -> %s", archive, output_portable.c_str());
    result = 0;
  } while (false);

  if (output != nullptr)
    std::fclose(output);
  if (result != 0) {
    ESP_LOGI(TAG, "ZIP ROM: extraction failed archive=%s reason=%s", archive, failure);
  }
  if (result != 0 && !output_tmp_fs.empty())
    unlink(output_tmp_fs.c_str());
  std::fclose(zip);
  return result;
}

void *PaxLoader::svc_file_open(const char *path, const char *mode) {
  if(path && std::strcmp(path,PAX_FIRMWARE_FILE)==0) {
    if(mode && mode[0]=='w' && (pax_c6::busy() || (active_ &&
       (active_->install_task_handle_ || active_->web_storage_busy_))))return nullptr;
    if(mode && mode[0]=='w')(void)restore_active_dns_if_missing_();
    return own_pax_file(pax_firmware::open(mode));
  }
  if(path && std::strcmp(path,PAX_C6_UPDATE_FILE)==0) {
    if(mode && mode[0]=='w' && pax_firmware::busy())return nullptr;
    return own_pax_file(pax_c6::open(mode));
  }
  if(path && std::strcmp(path,PAX_WIFI_SETTINGS_FILE)==0)
    return own_pax_file(pax_wifi::open(mode));
  const std::string mapped = runtime_path(path);
  FILE *file = std::fopen(mapped.c_str(), mode);
  // Apps open their data at fixed /sd/... paths. When a read-only open misses
  // the SD card, try the same path under data_root and the data_search roots,
  // so data kept on (or downloaded to) /usb0 works without changing the app.
  // Writes (saves, configs) always stay where the app asked.
  if (file != nullptr || active_ == nullptr || path == nullptr || mode == nullptr || mode[0] != 'r' ||
      std::strchr(mode, '+') != nullptr || std::strncmp(path, "/sd/", 4) != 0)
    return own_pax_file(file);
  const std::string found = active_->find_data_file_(path + 4);
  if (found.empty() || found == mapped)
    return nullptr;
  ESP_LOGD(TAG, "App read %s -> %s", path, found.c_str());
  return own_pax_file(std::fopen(found.c_str(), mode));
}
int PaxLoader::svc_file_close(void *stream) {
  heap_caps_free(take_owned(pax_files,stream));
  return stream!=nullptr ? std::fclose(static_cast<FILE *>(stream)) : -1;
}
size_t PaxLoader::svc_file_read(void *ptr, size_t size, size_t nmemb, void *stream) {
  return stream != nullptr ? std::fread(ptr, size, nmemb, static_cast<FILE *>(stream)) : 0;
}
size_t PaxLoader::svc_file_write(const void *ptr, size_t size, size_t nmemb, void *stream) {
  return stream != nullptr ? std::fwrite(ptr, size, nmemb, static_cast<FILE *>(stream)) : 0;
}
int PaxLoader::svc_file_seek(void *stream, long offset, int whence) {
  return stream != nullptr ? std::fseek(static_cast<FILE *>(stream), offset, whence) : -1;
}
long PaxLoader::svc_file_tell(void *stream) { return stream != nullptr ? std::ftell(static_cast<FILE *>(stream)) : -1; }

// ── App heap (pax_memory.h) ──
// ESPHome's CONFIG_SPIRAM_USE_CAPS_ALLOC makes plain malloc() internal RAM
// only, so the app's heap services pick the heap themselves: PSRAM, and
// internal RAM only above memory::INTERNAL_RESERVE.
static_assert(PAX_MEM_CAP_DMA == memory::APP_CAP_DMA && PAX_MEM_CAP_SPIRAM == memory::APP_CAP_SPIRAM &&
                  PAX_MEM_CAP_INTERNAL == memory::APP_CAP_INTERNAL,
              "pax_memory.h must know psram_app.h's PAX_MEM_CAP_* bits");

// ESP-IDF heap caps for pax_memory.h's HEAP_* flags.
static uint32_t idf_heap_caps(uint32_t heap) {
  uint32_t caps = MALLOC_CAP_8BIT;
  if (heap & memory::HEAP_SPIRAM)
    caps |= MALLOC_CAP_SPIRAM;
  if (heap & memory::HEAP_INTERNAL)
    caps |= MALLOC_CAP_INTERNAL;
  if (heap & memory::HEAP_DMA)
    caps |= MALLOC_CAP_DMA;
  return caps;
}

static bool internal_heap_allows(size_t size) {
  return memory::internal_fits(size, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// Counts the app's blocks that went to internal RAM (PSRAM was full) and the
// ones it could not have, for the heap log lines; the first of each in a run
// is logged at once.
static void app_heap_note(bool internal, size_t size) {
  std::atomic<uint32_t> &count = internal ? s_app_heap_internal : s_app_heap_failed;
  if (count.fetch_add(1) == 0) {
    ESP_LOGW(TAG, "PAX heap: %s %u bytes (PSRAM free %u B, largest %u B; internal free %u B)",
             internal ? "PSRAM is full; internal RAM for" : "no memory for", static_cast<unsigned>(size),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  }
}

// A block from the first heap in `plan` that has room.
static void *app_heap_alloc(const memory::Plan &plan, size_t size, bool zero) {
  for (int i = 0; i < plan.count; i++) {
    const memory::Attempt &attempt = plan.attempts[i];
    if (attempt.guarded && !internal_heap_allows(size))
      continue;
    const uint32_t caps = idf_heap_caps(attempt.heap);
    void *block = nullptr;
    if ((attempt.heap & memory::HEAP_DMA) != 0 && size != 0) {
      block = zero ? heap_caps_aligned_calloc(canvas::CACHE_LINE, 1, size, caps)
                   : heap_caps_aligned_alloc(canvas::CACHE_LINE, size, caps);
    } else {
      block = zero ? heap_caps_calloc(1, size, caps) : heap_caps_malloc(size, caps);
    }
    if (block != nullptr) {
      if (attempt.guarded)
        app_heap_note(true, size);
      return own_pax_allocation(block,size);
    }
  }
  if (size != 0)
    app_heap_note(false, size);
  return nullptr;
}

void *PaxLoader::svc_mem_caps_alloc(size_t size, uint32_t caps) {
  return app_heap_alloc(memory::caps_plan(caps, size), size, false);
}

void *PaxLoader::svc_mem_alloc(size_t size) { return app_heap_alloc(memory::plain_plan(size), size, false); }
void *PaxLoader::svc_mem_calloc(size_t n, size_t size) {
  size_t total = 0;
  if (!memory::calloc_bytes(n, size, &total))
    return nullptr;
  return app_heap_alloc(memory::plain_plan(total), total, true);
}
// Grows or shrinks in PSRAM; a block that was in internal RAM moves there.
void *PaxLoader::svc_mem_realloc(void *ptr, size_t size) {
  if(ptr==nullptr)return svc_mem_alloc(size);
  if(size==0) {svc_mem_free(ptr);return nullptr;}
  auto *record=take_owned(pax_allocations,ptr);
  if(record==nullptr)record=new_ownership_record(ptr,0);
  if(record==nullptr)return nullptr;  // Original block remains valid.
  const memory::Plan plan=memory::plain_plan(size);
  for(int i=0;i<plan.count;i++) {
    const memory::Attempt &attempt=plan.attempts[i];
    if(attempt.guarded && !internal_heap_allows(size))continue;
    void *block=heap_caps_realloc(ptr,size,idf_heap_caps(attempt.heap));
    if(block!=nullptr) {
      if(attempt.guarded)app_heap_note(true,size);
      record->pointer=block;record->bytes=size;insert_owned(pax_allocations,record);
      return block;
    }
  }
  insert_owned(pax_allocations,record);
  app_heap_note(false,size);
  return nullptr;
}
void PaxLoader::svc_mem_free(void *ptr) {
  heap_caps_free(take_owned(pax_allocations,ptr));
  heap_caps_free(ptr);
}

int PaxLoader::svc_log_vprintf(const char *fmt, va_list args) {
  char buffer[256];
  const int result = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  if (result > 0) {
    // stdout is buffered on the PAX worker and can disappear from the
    // monitor until a later flush. Route app logs through ESPHome's logger so
    // every PAX gets timestamped, immediately visible serial output.
    ESP_LOGI(TAG, "PAX: %s", buffer);
    if (PaxLoader::active_ != nullptr)
      PaxLoader::active_->append_report_log_(buffer);
  }
  return result;
}
int PaxLoader::svc_log_printf(const char *fmt, ...) {
  PaxServiceCallGuard guard;
  va_list args;
  va_start(args, fmt);
  const int result = svc_log_vprintf(fmt, args);
  va_end(args);
  return result;
}
void PaxLoader::svc_delay_ms(int ms) {
  // A PAX runs synchronously on ESPHome's loopTask. That means the normal
  // Application::loop() watchdog feed is not reached while the app is
  // active. Background PAX tasks call this hook too, but they are not
  // registered with ESPHome's task watchdog; calling App.feed_wdt() from
  // those tasks produces "task not found" and does not feed loopTask.
#ifdef USE_ESP32
  if (esp_task_wdt_status(nullptr) == ESP_OK)
    App.feed_wdt();
#else
  App.feed_wdt();
#endif
  vTaskDelay(pdMS_TO_TICKS(std::max(ms, 1)));
#ifdef USE_ESP32
  if (esp_task_wdt_status(nullptr) == ESP_OK)
    App.feed_wdt();
#else
  App.feed_wdt();
#endif
}
int64_t PaxLoader::svc_get_time_us() { return esp_timer_get_time(); }

static char s_rom_path[256] = {};
char *PaxLoader::svc_settings_rom_path_get() { return s_rom_path; }
void PaxLoader::svc_settings_rom_path_set(const char *path) {
  if (path == nullptr)
    return;
  std::strncpy(s_rom_path, path, sizeof(s_rom_path) - 1);
  s_rom_path[sizeof(s_rom_path) - 1] = '\0';
}
void PaxLoader::play_ui_sound(UiSound sound, uint8_t param) {
  if (!this->ui_sounds_ready_) {
    ESP_LOGD(TAG, "UI sound %d skipped: audio is not ready", static_cast<int>(sound));
    return;
  }
  if (this->speaker_ == nullptr) {
    ESP_LOGW(TAG, "UI sound %d skipped: no speaker is attached", static_cast<int>(sound));
    return;
  }
  if (this->master_volume_ <= 0) {
    ESP_LOGD(TAG, "UI sound %d skipped: master volume is 0", static_cast<int>(sound));
    return;
  }
  const std::string launcher_name = "launcher_ui.pax";
  const bool launcher_ui_active = this->launched_ &&
      this->running_app_source_.size() >= launcher_name.size() &&
      this->running_app_source_.compare(this->running_app_source_.size() - launcher_name.size(),
                                        launcher_name.size(), launcher_name) == 0;
  if ((this->app_handle_ != nullptr || this->pax_task_handle_ != nullptr || this->pax_loading_) &&
      !launcher_ui_active) {
    ESP_LOGD(TAG, "UI sound %d skipped: a PAX is running or loading", static_cast<int>(sound));
    return;
  }

  if (!this->speaker_->is_running() || this->audio_sample_rate_ != UiSoundGenerator::SAMPLE_RATE) {
    this->audio_sample_rate_ = UiSoundGenerator::SAMPLE_RATE;
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 2, UiSoundGenerator::SAMPLE_RATE));
    this->speaker_->set_mute_state(false);
    this->speaker_->set_volume(static_cast<float>(this->master_volume_) / 100.0f);
    this->speaker_->start();
  }

  // Button callbacks can run on LVGL's small-stack task.  Keeping the 6.4 KB
  // stereo PCM buffer here on the stack can overflow that task as soon as a
  // sound is triggered by touch.  Allocate it from PSRAM instead; the
  // resampler copies the data into its own ring buffer before this returns.
  constexpr size_t UI_SOUND_MAX_FRAMES = 1600;
  auto *buffer = static_cast<int16_t *>(heap_caps_malloc(
      UI_SOUND_MAX_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr)
    buffer = static_cast<int16_t *>(heap_caps_malloc(UI_SOUND_MAX_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    ESP_LOGW(TAG, "UI sound %d skipped: could not allocate PCM buffer", static_cast<int>(sound));
    return;
  }

  const size_t frames = UiSoundGenerator::generate(sound, param, buffer, UI_SOUND_MAX_FRAMES);
  if (frames == 0)
  {
    heap_caps_free(buffer);
    return;
  }

  size_t written = 0;
  const size_t bytes = frames * 2 * sizeof(int16_t);
  const uint8_t *pcm = reinterpret_cast<const uint8_t *>(buffer);
  int retries = 0;
  while (written < bytes && retries < 15) {
    const size_t chunk = this->speaker_->play(pcm + written, bytes - written, pdMS_TO_TICKS(20));
    if (chunk > 0) {
      written += chunk;
      retries = 0;
    } else {
      retries++;
    }
  }
  ESP_LOGD(TAG, "UI sound %d: %u/%u bytes played (retries=%d)", static_cast<int>(sound), (unsigned)written, (unsigned)bytes, retries);
  heap_caps_free(buffer);
}

void PaxLoader::set_master_volume(int32_t level) {
  const int32_t old_level = this->master_volume_;
  this->master_volume_ = std::max<int32_t>(0, std::min<int32_t>(100, level));
  if (this->speaker_ != nullptr)
    this->speaker_->set_volume(static_cast<float>(this->master_volume_) / 100.0f);
  ESP_LOGI(TAG, "PAX master volume: %ld%%", static_cast<long>(this->master_volume_));
  if (this->master_volume_ != old_level) {
    static int64_t last_tick = 0;
    const int64_t now = esp_timer_get_time();
    if (now - last_tick > 45000) {
      last_tick = now;
      this->play_ui_sound(UI_SOUND_VOLUME, static_cast<uint8_t>(this->master_volume_));
    }
  }
}

void PaxLoader::set_brightness_level(int32_t level) {
  const int32_t old_level = s_brightness;
  s_brightness = std::max<int32_t>(5, std::min<int32_t>(100, level));
#ifdef PAX_LOADER_USE_LVGL
  if (this->catalog_brightness_number_ != nullptr) {
    auto call = this->catalog_brightness_number_->make_call();
    call.set_value(s_brightness);
    call.perform();
  }
#endif
  if (s_brightness != old_level) {
    static int64_t last_tick = 0;
    const int64_t now = esp_timer_get_time();
    if (now - last_tick > 45000) {
      last_tick = now;
      this->play_ui_sound(UI_SOUND_VOLUME, static_cast<uint8_t>(s_brightness));
    }
  }
}

void PaxLoader::set_auto_brightness_enabled(bool enabled) {
  if (enabled && (this->ambient_light_sensor_ == nullptr || this->catalog_brightness_number_ == nullptr)) {
    ESP_LOGW(TAG, "Auto brightness unavailable: light sensor or brightness number is not configured");
    enabled = false;
  }
  this->auto_brightness_enabled_ = enabled;
  this->next_auto_brightness_us_ = 0;
#ifdef PAX_LOADER_USE_LVGL
  if (this->drawer_auto_brightness_label_ != nullptr) {
    lv_label_set_text(this->drawer_auto_brightness_label_,
                      enabled ? "AUTO-BRIGHTNESS: ON" : "AUTO-BRIGHTNESS: OFF");
  }
#endif
  ESP_LOGI(TAG, "Auto brightness: %s", enabled ? "on" : "off");
}

void PaxLoader::update_auto_brightness_() {
  if (!this->auto_brightness_enabled_ || this->ambient_light_sensor_ == nullptr ||
      this->catalog_brightness_number_ == nullptr)
    return;
  const int64_t now = esp_timer_get_time();
  if (this->next_auto_brightness_us_ != 0 && now < this->next_auto_brightness_us_)
    return;
  this->next_auto_brightness_us_ = now + 500000;
  const float lux = this->ambient_light_sensor_->state;
  if (!std::isfinite(lux))
    return;
  // A logarithmic curve keeps the display usable in a dark room while still
  // reaching full brightness in daylight, without visible sensor-noise flicker.
  const float normalized = std::max(0.0f, std::min(1.0f, std::log10(std::max(1.0f, lux)) / std::log10(500.0f)));
  const int32_t target = 5 + static_cast<int32_t>(normalized * 95.0f + 0.5f);
  const int32_t current = std::isfinite(this->catalog_brightness_number_->state)
                              ? static_cast<int32_t>(this->catalog_brightness_number_->state)
                              : 100;
  if (std::abs(target - current) < 2)
    return;
  auto call = this->catalog_brightness_number_->make_call();
  call.set_value(target);
  call.perform();
}

int32_t PaxLoader::svc_settings_volume_get() {
  return PaxLoader::active_ != nullptr ? PaxLoader::active_->master_volume_ : 100;
}
void PaxLoader::svc_settings_volume_set(int32_t level) {
  if (PaxLoader::active_ != nullptr)
    PaxLoader::active_->set_master_volume(level);
}
int32_t PaxLoader::svc_settings_brightness_get() {
#ifdef PAX_LOADER_USE_LVGL
  // Template Number restores the backlight before the loader starts. Read
  // that value rather than returning the overlay's power-on default.
  if (active_ != nullptr && active_->catalog_brightness_number_ != nullptr &&
      std::isfinite(active_->catalog_brightness_number_->state))
    s_brightness = std::clamp<int32_t>(static_cast<int32_t>(active_->catalog_brightness_number_->state), 5, 100);
#endif
  return s_brightness;
}
void PaxLoader::svc_settings_brightness_set(int32_t level) {
  if (PaxLoader::active_ != nullptr)
    PaxLoader::active_->set_brightness_level(level);
  else
    s_brightness = std::max<int32_t>(5, std::min<int32_t>(100, level));
}

int PaxLoader::svc_device_date_get(char *out, uint32_t capacity) {
  if (out == nullptr || capacity == 0) return -1;
  out[0] = '\0';
  const time_t now = std::time(nullptr);
  struct tm local{};
  if (now < MIN_VALID_TLS_TIME || localtime_r(&now, &local) == nullptr) return -2;
  char date[64];
  const size_t length = std::strftime(date, sizeof(date), "%A\n%d %B %Y", &local);
  if (length == 0 || length >= capacity) return -1;
  std::memcpy(out, date, length + 1);
  return 0;
}

int PaxLoader::svc_device_status_get(pax_device_status_t *out) {
  if (out == nullptr)
    return -1;
  std::memset(out, 0, sizeof(*out));
  out->battery_percent = -1;
  const time_t now = std::time(nullptr);
  struct tm local{};
  if (now >= MIN_VALID_TLS_TIME && localtime_r(&now, &local) != nullptr)
    std::snprintf(out->local_time, sizeof(out->local_time), "%d:%02d %s",
                  ((local.tm_hour % 12) == 0 ? 12 : local.tm_hour % 12), local.tm_min,
                  local.tm_hour >= 12 ? "PM" : "AM");
#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr && ethernet::global_eth_component->is_connected())
    out->network_type = PAX_NETWORK_ETHERNET;
#endif
#ifdef USE_WIFI
  if (out->network_type == PAX_NETWORK_NONE && wifi::global_wifi_component != nullptr &&
      wifi::global_wifi_component->is_connected())
    out->network_type = PAX_NETWORK_WIFI;
#endif
  return 0;
}

int32_t PaxLoader::svc_settings_auto_brightness_get() {
  return PaxLoader::active_ != nullptr && PaxLoader::active_->auto_brightness_enabled() ? 1 : 0;
}

void PaxLoader::svc_settings_auto_brightness_set(int32_t enabled) {
  if (PaxLoader::active_ != nullptr)
    PaxLoader::active_->set_auto_brightness_enabled(enabled != 0);
}

void PaxLoader::svc_ui_sound(uint8_t sound, uint8_t param) {
  if (PaxLoader::active_ == nullptr || sound > UI_SOUND_VOLUME)
    return;
  PaxLoader::active_->play_ui_sound(static_cast<UiSound>(sound), param);
}

int PaxLoader::svc_task_create(void (*fn)(void *), const char *name, uint32_t stack_depth, void *arg,
                                int priority, void *out_handle, int core) {
  if (out_handle != nullptr)
    *static_cast<TaskHandle_t *>(out_handle) = nullptr;
  if (fn == nullptr || active_ == nullptr || active_->global_close_requested_)
    return -1;
  auto *start = static_cast<PaxTaskStart *>(malloc(sizeof(PaxTaskStart)));
  if (start == nullptr)
    return -1;
  *start = {fn, arg};
  TaskHandle_t handle = nullptr;
  const BaseType_t affinity = core < 0 ? tskNO_AFFINITY : static_cast<BaseType_t>(core);
  const bool caps = stack_depth > 32768;
  const BaseType_t result = caps ? xTaskCreatePinnedToCoreWithCaps(
      pax_child_task, name, stack_depth, nullptr, priority, &handle, affinity,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : xTaskCreatePinnedToCore(
      pax_child_task, name, stack_depth, nullptr, priority, &handle, affinity);
  if (result != pdPASS || handle == nullptr) {
    free(start);
    return -1;
  }
  if (!remember_pax_task(handle, caps, start)) {
    if (caps) vTaskDeleteWithCaps(handle); else vTaskDelete(handle);
    free(start);
    ESP_LOGE("PAX", "App task registry is full");
    return -1;
  }
  if (out_handle != nullptr)
    *static_cast<TaskHandle_t *>(out_handle) = handle;
  return 0;
}
void PaxLoader::svc_task_delete(void *handle) {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  TaskHandle_t target = handle != nullptr ? static_cast<TaskHandle_t>(handle) : self;
  if (target == self) {
    delete_pax_task(target);  // A self-delete cannot run a guard destructor.
  } else {
    PaxServiceCallGuard guard;
    delete_pax_task(target);
  }
}

void PaxLoader::populate_services(app_services_t *services) {
  std::memset(services, 0, sizeof(*services));
  services->abi_version = PAX_ABI_VERSION;
  services->display_get_framebuffer = &PaxServiceGate<&PaxLoader::svc_display_get_framebuffer>::call;
  services->display_get_emu_buffer = &PaxServiceGate<&PaxLoader::svc_display_get_emu_buffer>::call;
  services->display_flush = &PaxServiceGate<&PaxLoader::svc_display_flush>::call;
  services->display_emu_flush = &PaxServiceGate<&PaxLoader::svc_display_emu_flush>::call;
  services->display_clear = &PaxServiceGate<&PaxLoader::svc_display_clear>::call;
  services->display_set_scale = &PaxServiceGate<&PaxLoader::svc_display_set_scale>::call;
  services->display_write_frame_rgb565 = &PaxServiceGate<&PaxLoader::svc_display_write_frame_rgb565>::call;
  services->display_write_frame_custom = &PaxServiceGate<&PaxLoader::svc_display_write_frame_custom>::call;
  services->display_write_rect = &PaxServiceGate<&PaxLoader::svc_display_write_rect>::call;
  services->sprite_blit = &PaxServiceGate<&PaxLoader::svc_sprite_blit>::call;
  services->fb_copy = &PaxServiceGate<&PaxLoader::svc_fb_copy>::call;
  services->png_load_rgb565 = &PaxServiceGate<&PaxLoader::svc_png_load_rgb565>::call;
  services->png_load_rgb565_blended = &PaxServiceGate<&PaxLoader::svc_png_load_rgb565_blended>::call;
  services->display_lock = &PaxServiceGate<&PaxLoader::svc_display_lock>::call;
  services->display_unlock = &PaxServiceGate<&PaxLoader::svc_display_unlock>::call;
  services->audio_init = &PaxServiceGate<&PaxLoader::svc_audio_init>::call;
  services->audio_submit = &PaxServiceGate<&PaxLoader::svc_audio_submit>::call;
  services->input_gamepad_read = &PaxServiceGate<&PaxLoader::svc_input_gamepad_read>::call;
  services->input_l3_read = &PaxServiceGate<&PaxLoader::svc_input_l3_read>::call;
  services->file_open = &PaxServiceGate<&PaxLoader::svc_file_open>::call;
  services->file_close = &PaxServiceGate<&PaxLoader::svc_file_close>::call;
  services->file_read = &PaxServiceGate<&PaxLoader::svc_file_read>::call;
  services->file_write = &PaxServiceGate<&PaxLoader::svc_file_write>::call;
  services->file_seek = &PaxServiceGate<&PaxLoader::svc_file_seek>::call;
  services->file_tell = &PaxServiceGate<&PaxLoader::svc_file_tell>::call;
  services->mem_alloc = &PaxServiceGate<&PaxLoader::svc_mem_alloc>::call;
  services->mem_calloc = &PaxServiceGate<&PaxLoader::svc_mem_calloc>::call;
  services->mem_realloc = &PaxServiceGate<&PaxLoader::svc_mem_realloc>::call;
  services->mem_free = &PaxServiceGate<&PaxLoader::svc_mem_free>::call;
  services->mem_caps_alloc = &PaxServiceGate<&PaxLoader::svc_mem_caps_alloc>::call;
  services->log_printf = &PaxLoader::svc_log_printf;
  services->log_vprintf = &PaxServiceGate<&PaxLoader::svc_log_vprintf>::call;
  services->delay_ms = &PaxServiceGate<&PaxLoader::svc_delay_ms>::call;
  services->get_time_us = &PaxServiceGate<&PaxLoader::svc_get_time_us>::call;
  services->settings_rom_path_get = &PaxServiceGate<&PaxLoader::svc_settings_rom_path_get>::call;
  services->settings_rom_path_set = &PaxServiceGate<&PaxLoader::svc_settings_rom_path_set>::call;
  services->settings_volume_get = &PaxServiceGate<&PaxLoader::svc_settings_volume_get>::call;
  services->settings_volume_set = &PaxServiceGate<&PaxLoader::svc_settings_volume_set>::call;
  services->settings_brightness_get = &PaxServiceGate<&PaxLoader::svc_settings_brightness_get>::call;
  services->settings_brightness_set = &PaxServiceGate<&PaxLoader::svc_settings_brightness_set>::call;
  services->task_create = &PaxServiceGate<&PaxLoader::svc_task_create>::call;
  services->task_delete = &PaxLoader::svc_task_delete;
  services->touch_read = &PaxServiceGate<&PaxLoader::svc_touch_read>::call;
  services->input_mouse_read = &PaxServiceGate<&PaxLoader::svc_input_mouse_read>::call;
  services->input_mouse_scroll_read = &PaxServiceGate<&PaxLoader::svc_input_mouse_scroll_read>::call;
  services->input_keyboard_read = &PaxServiceGate<&PaxLoader::svc_input_keyboard_read>::call;
  services->net_udp_open = &PaxServiceGate<&PaxLoader::svc_net_udp_open>::call;
  services->net_udp_send = &PaxServiceGate<&PaxLoader::svc_net_udp_send>::call;
  services->net_udp_recv = &PaxServiceGate<&PaxLoader::svc_net_udp_recv>::call;
  services->net_udp_close = &PaxServiceGate<&PaxLoader::svc_net_udp_close>::call;
  services->net_ipv4 = &PaxServiceGate<&PaxLoader::svc_net_ipv4>::call;
  services->net_tcp_connect = &PaxServiceGate<&PaxLoader::svc_net_tcp_connect>::call;
  services->net_tcp_listen = &PaxServiceGate<&PaxLoader::svc_net_tcp_listen>::call;
  services->net_tcp_accept = &PaxServiceGate<&PaxLoader::svc_net_tcp_accept>::call;
  services->net_tcp_send = &PaxServiceGate<&PaxLoader::svc_net_tcp_send>::call;
  services->net_tcp_recv = &PaxServiceGate<&PaxLoader::svc_net_tcp_recv>::call;
  services->net_poll = &PaxServiceGate<&PaxLoader::svc_net_poll>::call;
  services->net_resolve = &PaxServiceGate<&PaxLoader::svc_net_resolve>::call;
  services->jpeg_decode_rgb565 = &PaxServiceGate<&PaxLoader::svc_jpeg_decode_rgb565>::call;
  services->hardware_count = &PaxServiceGate<&PaxLoader::svc_hardware_count>::call;
  services->hardware_info = &PaxServiceGate<&PaxLoader::svc_hardware_info>::call;
  services->hardware_read = &PaxServiceGate<&PaxLoader::svc_hardware_read>::call;
  services->hardware_write = &PaxServiceGate<&PaxLoader::svc_hardware_write>::call;
  services->display_get_size = &PaxServiceGate<&PaxLoader::svc_display_get_size>::call;
  services->display_set_canvas = &PaxServiceGate<&PaxLoader::svc_display_set_canvas>::call;
  services->midi_read = &PaxServiceGate<&PaxLoader::svc_midi_read>::call;
  services->midi_write = &PaxServiceGate<&PaxLoader::svc_midi_write>::call;
  services->touch_read_points = &PaxServiceGate<&PaxLoader::svc_touch_read_points>::call;
  if (s_app_tls_lock == nullptr)
    s_app_tls_lock = xSemaphoreCreateMutex();
  services->net_tls_connect = &PaxServiceGate<&PaxLoader::svc_net_tls_connect>::call;
  services->net_tls_status = &PaxServiceGate<&PaxLoader::svc_net_tls_status>::call;
  services->net_tls_send = &PaxServiceGate<&PaxLoader::svc_net_tls_send>::call;
  services->net_tls_recv = &PaxServiceGate<&PaxLoader::svc_net_tls_recv>::call;
  services->net_tls_close = &PaxServiceGate<&PaxLoader::svc_net_tls_close>::call;
  services->app_open = &PaxServiceGate<&PaxLoader::svc_app_open>::call;
  services->app_get_arg = &PaxServiceGate<&PaxLoader::svc_app_get_arg>::call;
  services->app_set_resume_arg = &PaxServiceGate<&PaxLoader::svc_app_set_resume_arg>::call;
  services->file_list_dir = &PaxServiceGate<&PaxLoader::svc_file_list_dir>::call;
  services->file_mkdir = &PaxServiceGate<&PaxLoader::svc_file_mkdir>::call;
  services->file_remove = &PaxServiceGate<&PaxLoader::svc_file_remove>::call;
  services->file_rename = &PaxServiceGate<&PaxLoader::svc_file_rename>::call;
  services->file_stat = &PaxServiceGate<&PaxLoader::svc_file_stat>::call;
  services->file_zip_extract_first = &PaxServiceGate<&PaxLoader::svc_file_zip_extract_first>::call;
  services->file_zip_extract_first_progress = &PaxServiceGate<&PaxLoader::svc_file_zip_extract_first_progress>::call;
  services->game_menu_register = &PaxServiceGate<&PaxLoader::svc_game_menu_register>::call;
  services->game_menu_set_page = &PaxServiceGate<&PaxLoader::svc_game_menu_set_page>::call;
  services->game_menu_close = &PaxServiceGate<&PaxLoader::svc_game_menu_close>::call;
  services->ui_call_sync = &PaxServiceGate<&PaxLoader::svc_ui_call_sync>::call;
  services->store_install_start = &PaxServiceGate<&PaxLoader::svc_store_install_start>::call;
  services->store_install_status = &PaxServiceGate<&PaxLoader::svc_store_install_status>::call;
  services->device_status_get = &PaxServiceGate<&PaxLoader::svc_device_status_get>::call;
  services->device_date_get = &PaxServiceGate<&PaxLoader::svc_device_date_get>::call;
  services->settings_auto_brightness_get = &PaxServiceGate<&PaxLoader::svc_settings_auto_brightness_get>::call;
  services->settings_auto_brightness_set = &PaxServiceGate<&PaxLoader::svc_settings_auto_brightness_set>::call;
  services->archive_download_start = &PaxServiceGate<&PaxLoader::svc_archive_download_start>::call;
  services->ui_sound = &PaxServiceGate<&PaxLoader::svc_ui_sound>::call;
  services->network_config_get = &PaxServiceGate<&PaxLoader::svc_network_config_get>::call;
  services->network_config_set = &PaxServiceGate<&PaxLoader::svc_network_config_set>::call;
  services->storage_status_get = &PaxServiceGate<&PaxLoader::svc_storage_status_get>::call;
  services->storage_set_mounted = &PaxServiceGate<&PaxLoader::svc_storage_set_mounted>::call;
  services->archive_auth_set = &PaxServiceGate<&PaxLoader::svc_archive_auth_set>::call;
  services->archive_auth_get = &PaxServiceGate<&PaxLoader::svc_archive_auth_get>::call;
}

esp_err_t psram_app_load(const char *path, psram_app_handle_t *out_handle) {
  if (out_handle == nullptr || path == nullptr)
    return ESP_ERR_INVALID_ARG;
  *out_handle = nullptr;
  ESP_LOGI(TAG, "Loading PAX: %s", path);

  FILE *file = std::fopen(path, "rb");
  if (file == nullptr)
    return ESP_ERR_NOT_FOUND;
  pax_header_t header{};
  if (std::fread(&header, 1, sizeof(header), file) != sizeof(header)) {
    std::fclose(file);
    return ESP_ERR_INVALID_SIZE;
  }
  if (header.magic != PAX_MAGIC || header.version != PAX_ABI_VERSION || header.text_size == 0 ||
      header.entry_off >= header.text_size) {
    ESP_LOGE(TAG, "Invalid PAX header: magic=0x%08lx version=%lu text=%lu entry=%lu",
             static_cast<unsigned long>(header.magic), static_cast<unsigned long>(header.version),
             static_cast<unsigned long>(header.text_size), static_cast<unsigned long>(header.entry_off));
    std::fclose(file);
    return ESP_ERR_INVALID_ARG;
  }

  const size_t load_size = static_cast<size_t>(header.text_size) + header.data_size;
  const size_t total = load_size + header.bss_size;
  if (load_size < header.text_size || total < load_size) {
    std::fclose(file);
    return ESP_ERR_INVALID_SIZE;
  }
  const size_t allocated = align_up(total, MMU_PAGE_SIZE);
  auto app = static_cast<psram_app_handle_t>(std::calloc(1, sizeof(struct psram_app)));
  if (app == nullptr) {
    std::fclose(file);
    return ESP_ERR_NO_MEM;
  }
  app->header = header;
  app->code_alloc = allocated;
  app->code_buf = heap_caps_aligned_alloc(MMU_PAGE_SIZE, allocated, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (app->code_buf == nullptr) {
    std::free(app);
    std::fclose(file);
    return ESP_ERR_NO_MEM;
  }
  if (std::fread(app->code_buf, 1, load_size, file) != load_size) {
    heap_caps_free(app->code_buf);
    std::free(app);
    std::fclose(file);
    return ESP_ERR_INVALID_SIZE;
  }
  std::memset(static_cast<uint8_t *>(app->code_buf) + load_size, 0, allocated - load_size);
  std::fclose(file);

  esp_cache_msync(app->code_buf, app->code_alloc,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  *out_handle = app;
  ESP_LOGI(TAG, "PAX loaded: text=%lu data=%lu bss=%lu entry=%lu",
           static_cast<unsigned long>(header.text_size), static_cast<unsigned long>(header.data_size),
           static_cast<unsigned long>(header.bss_size), static_cast<unsigned long>(header.entry_off));
  return ESP_OK;
}

static esp_err_t http_finish(esp_http_client_handle_t client, esp_err_t result) {
  if (client != nullptr) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
  }
  return result;
}

// ESP-IDF intentionally uses a small set of transport errors.  In
// particular, ESP_ERR_HTTP_CONNECT does not distinguish DNS, Wi-Fi, a
// refused server connection, or a TLS handshake failure.  Keep the original
// code for reports and logs, but give the launcher a useful explanation.
static const char *http_error_hint(esp_err_t err) {
  switch (err) {
    case ESP_ERR_INVALID_STATE:
      return "device is offline or Wi-Fi is not connected";
    case ESP_ERR_HTTP_CONNECT:
      return "could not connect (check Wi-Fi, DNS, server, or TLS)";
    case ESP_ERR_HTTP_CONNECTING:
      return "connection timed out while opening";
    case ESP_ERR_HTTP_READ_TIMEOUT:
    case ESP_ERR_TIMEOUT:
      return "server timed out while responding";
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
      return "server closed the connection";
    case ESP_ERR_HTTP_EAGAIN:
      return "network is temporarily busy";
    case ESP_ERR_HTTP_FETCH_HEADER:
      return "server returned no usable HTTP headers";
    case ESP_ERR_HTTP_WRITE_DATA:
      return "could not send the HTTP request";
    case ESP_ERR_HTTP_INCOMPLETE_DATA:
      return "server sent an incomplete response";
    case ESP_ERR_HTTP_MAX_REDIRECT:
      return "too many server redirects";
    case ESP_ERR_HTTP_INVALID_TRANSPORT:
      return "unsupported URL scheme (use http or https)";
    case ESP_ERR_INVALID_RESPONSE:
      return "server returned an unexpected HTTP response";
    case ESP_ERR_NOT_FOUND:
      return "file was not found on the server (HTTP 404)";
    case ESP_ERR_NO_MEM:
      return "not enough free memory";
    case ESP_ERR_INVALID_SIZE:
      return "server response is too large or the size is wrong";
    case ESP_ERR_INVALID_ARG:
      return "invalid URL or request";
    case ESP_ERR_INVALID_CRC:
      return "download checksum did not match";
    case ESP_FAIL:
      return "operation failed";
    default:
      return "network request failed";
  }
}

static void log_http_error(const char *operation, const char *url, esp_http_client_handle_t client, esp_err_t err) {
  const int transport_errno = client != nullptr ? esp_http_client_get_errno(client) : 0;
  if (transport_errno != 0) {
    ESP_LOGE(TAG, "%s: %s (%s, 0x%x; errno %d: %s) URL=%s", operation, http_error_hint(err),
             esp_err_to_name(err), static_cast<unsigned>(err), transport_errno, std::strerror(transport_errno),
             url != nullptr ? url : "(null)");
  } else {
    ESP_LOGE(TAG, "%s: %s (%s, 0x%x) URL=%s", operation, http_error_hint(err), esp_err_to_name(err),
             static_cast<unsigned>(err), url != nullptr ? url : "(null)");
  }
}

static esp_err_t http_read_exact(esp_http_client_handle_t client, uint8_t *buffer, size_t length) {
  if (client == nullptr || (buffer == nullptr && length != 0))
    return ESP_ERR_INVALID_ARG;

  size_t offset = 0;
  uint32_t idle_reads = 0;
  while (offset < length) {
    const int requested = static_cast<int>(std::min<size_t>(length - offset, INT_MAX));
    const int result = esp_http_client_read(client, reinterpret_cast<char *>(buffer + offset), requested);
    if (result > 0) {
      offset += static_cast<size_t>(result);
      idle_reads = 0;
      continue;
    }

    // A slow Wi-Fi server may briefly have no bytes available. The HTTP
    // client reports both zero and EAGAIN while a response is still open.
    if (result == 0 || result == -ESP_ERR_HTTP_EAGAIN) {
      if (++idle_reads > 750)  // 15 seconds at 20 ms per retry
        return ESP_ERR_TIMEOUT;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    return static_cast<esp_err_t>(-result);
  }
  return ESP_OK;
}

// ── Test reports (report_url) ──────────────────────────────────────────────
// One JSON POST per app run, so a store test can be judged without someone
// copying the serial log by hand.

static void json_append_escaped(std::string *out, const std::string &value) {
  out->push_back('"');
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (c < 0x20 || c >= 0x80) {
          // Logs are expected to be ASCII; escape anything else so the body is
          // always valid JSON even if an app prints raw bytes.
          char escaped[7];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out->append(escaped);
        } else {
          out->push_back(ch);
        }
    }
  }
  out->push_back('"');
}

struct PaxReport {
  std::string url;
  std::string body;
};

void PaxLoader::begin_report_(const std::string &source) {
  if (this->report_url_.empty() || this->report_mutex_ == nullptr)
    return;
  if (xSemaphoreTake(this->report_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    this->report_log_.clear();
    this->report_source_ = source;
    this->report_started_us_ = esp_timer_get_time();
    xSemaphoreGive(this->report_mutex_);
  }
}

void PaxLoader::append_report_log_(const char *line) {
  if (this->report_url_.empty() || this->report_mutex_ == nullptr || line == nullptr)
    return;
  // Called from the PAX worker; never block it for long.
  if (xSemaphoreTake(this->report_mutex_, pdMS_TO_TICKS(5)) != pdTRUE)
    return;
  this->report_log_.append(line);
  if (this->report_log_.empty() || this->report_log_.back() != '\n')
    this->report_log_.push_back('\n');
  if (this->report_log_.size() > this->report_log_bytes_)
    this->report_log_.erase(0, this->report_log_.size() - this->report_log_bytes_);
  xSemaphoreGive(this->report_mutex_);
}

void PaxLoader::send_report_(const char *outcome, int result, const std::string &source) {
  if (this->report_url_.empty() || this->report_mutex_ == nullptr)
    return;
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Test report not sent: network is down");
    return;
  }

  std::string log;
  int64_t started_us = 0;
  if (xSemaphoreTake(this->report_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    log.swap(this->report_log_);
    started_us = this->report_started_us_;
    xSemaphoreGive(this->report_mutex_);
  }
  const int64_t runtime_ms = started_us > 0 ? (esp_timer_get_time() - started_us) / 1000 : 0;
  const size_t slash = source.find_last_of('/');
  const std::string file = slash == std::string::npos ? source : source.substr(slash + 1);
  const bool is_load_error = std::strcmp(outcome, "load_failed") == 0;

  auto *report = new PaxReport{this->report_url_, {}};  // NOLINT(cppcoreguidelines-owning-memory)
  std::string &body = report->body;
  body.reserve(log.size() + 512);
  body.append("{\"device\":");
  json_append_escaped(&body, App.get_name());
  body.append(",\"app\":");
  json_append_escaped(&body, file);
  body.append(",\"source\":");
  json_append_escaped(&body, source);
  body.append(",\"outcome\":");
  json_append_escaped(&body, outcome);
  body.append(",\"result\":");
  body.append(std::to_string(result));
  if (is_load_error) {
    body.append(",\"error\":");
    json_append_escaped(&body, esp_err_to_name(static_cast<esp_err_t>(result)));
  }
  body.append(",\"runtime_ms\":");
  body.append(std::to_string(runtime_ms));
  body.append(",\"abi\":");
  body.append(std::to_string(PAX_ABI_VERSION));
  body.append(",\"log\":");
  json_append_escaped(&body, log);
  body.push_back('}');

  // The HTTP(S) request can take seconds; keep it off the ESPHome loop.
  if (xTaskCreatePinnedToCore(&PaxLoader::report_task_entry_, "pax_report", 8192, report, 3, nullptr, 0) !=
      pdPASS) {
    ESP_LOGW(TAG, "Test report not sent: could not create task");
    delete report;  // NOLINT(cppcoreguidelines-owning-memory)
    return;
  }
  ESP_LOGI(TAG, "Sending test report: %s %s (result %d, %lld ms, %u log bytes)", file.c_str(), outcome, result,
           static_cast<long long>(runtime_ms), static_cast<unsigned>(log.size()));
}

void PaxLoader::report_task_entry_(void *arg) {
  auto *report = static_cast<PaxReport *>(arg);
  esp_http_client_config_t config{};
  config.url = report->url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.disable_auto_redirect = false;
  config.max_redirection_count = HTTP_MAX_REDIRECTIONS;
  config.keep_alive_enable = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (std::strncmp(report->url.c_str(), "https://", 8) == 0)
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  (void)restore_active_dns_if_missing_();
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Test report not sent: out of memory");
  } else {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, report->body.data(), static_cast<int>(report->body.size()));
    const esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Test report sent (HTTP %d)", esp_http_client_get_status_code(client));
    } else {
      ESP_LOGW(TAG, "Test report failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
  }
  delete report;  // NOLINT(cppcoreguidelines-owning-memory)
  vTaskDelete(nullptr);
}


static std::string catalog_url_for_href(const std::string &base_url, const std::string &href);

struct HttpRedirectCapture {
  std::string location;
};

static esp_err_t http_event_redirect_capture(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (evt->header_key != nullptr && strcasecmp(evt->header_key, "Location") == 0) {
      auto *capture = static_cast<HttpRedirectCapture *>(evt->user_data);
      if (capture != nullptr && evt->header_value != nullptr) {
        capture->location = evt->header_value;
      }
    }
  }
  return ESP_OK;
}

static esp_err_t fetch_http_text_impl(const char *url, std::string *out, size_t max_bytes, uint8_t redirect_count = 0);

esp_err_t fetch_http_text(const char *url, std::string *out, size_t max_bytes) {
  return fetch_http_text_impl(url, out, max_bytes, 0);
}

static esp_err_t fetch_http_text_impl(const char *url, std::string *out, size_t max_bytes, uint8_t redirect_count) {
  if (url == nullptr || out == nullptr || !is_network_url(url))
    return ESP_ERR_INVALID_ARG;
  out->clear();
  if (!network::is_connected())
    return ESP_ERR_INVALID_STATE;

  // mbedTLS checks certificate validity against the system clock. On a
  // freshly booted ESP32 without an RTC, SNTP may still be completing while
  // the UI is already usable. Give the configured SNTP component a short
  // window to establish the clock instead of returning a generic unavailable
  // error on the first HTTPS request.
  if (std::strncmp(url, "https://", 8) == 0 && std::time(nullptr) < MIN_VALID_TLS_TIME) {
    for (uint8_t attempt = 0; attempt < 100 && std::time(nullptr) < MIN_VALID_TLS_TIME; attempt++)
      vTaskDelay(pdMS_TO_TICKS(100));
    if (std::time(nullptr) < MIN_VALID_TLS_TIME)
      ESP_LOGW(TAG, "HTTPS requested before SNTP clock sync; certificate validation may fail: %s", url);
  }

  HttpRedirectCapture redirect_capture;
  esp_http_client_config_t config{};
  config.url = url;
  config.event_handler = http_event_redirect_capture;
  config.user_data = &redirect_capture;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.buffer_size = HTTP_READ_BUFFER_SIZE;
  config.buffer_size_tx = 2048;
  config.disable_auto_redirect = true;
  config.max_redirection_count = HTTP_MAX_REDIRECTIONS;
  config.keep_alive_enable = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (std::strncmp(url, "https://", 8) == 0)
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

  (void)restore_active_dns_if_missing_();
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr)
    return ESP_ERR_NO_MEM;
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    log_http_error("HTTP open failed", url, client, err);
    return http_finish(client, err);
  }

  int64_t content_length = -1;
  for (uint8_t attempt = 0; attempt < 6; attempt++) {
    content_length = esp_http_client_fetch_headers(client);
    if (content_length != -ESP_ERR_HTTP_EAGAIN)
      break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (content_length == -ESP_ERR_HTTP_EAGAIN) {
    log_http_error("HTTP header fetch timed out", url, client, ESP_ERR_TIMEOUT);
    return http_finish(client, ESP_ERR_TIMEOUT);
  }
  const int status_code = esp_http_client_get_status_code(client);
  if (status_code == 404) {
    // Callers decide whether a missing file is an error (the catalog) or
    // normal (an app without a data list).
    ESP_LOGD(TAG, "HTTP 404: %s", url);
    return http_finish(client, ESP_ERR_NOT_FOUND);
  }
  if (status_code >= 300 && status_code < 400) {
    if (redirect_count < HTTP_MAX_REDIRECTIONS && !redirect_capture.location.empty()) {
      const std::string next_url = catalog_url_for_href(url, redirect_capture.location);
      ESP_LOGI(TAG, "Following HTTP redirect %u: %s -> %s", static_cast<unsigned>(redirect_count + 1), url,
               next_url.c_str());
      http_finish(client, ESP_OK);
      return fetch_http_text_impl(next_url.c_str(), out, max_bytes, redirect_count + 1);
    }
    ESP_LOGE(TAG, "HTTP redirect %d has no usable Location or exceeded %u hops: %s", status_code,
             static_cast<unsigned>(HTTP_MAX_REDIRECTIONS), url);
    return http_finish(client, ESP_ERR_HTTP_MAX_REDIRECT);
  }
  if (status_code != 200) {
    ESP_LOGE(TAG, "HTTP status %d: %s", status_code, url);
    return http_finish(client, ESP_ERR_INVALID_RESPONSE);
  }
  if (content_length > static_cast<int64_t>(max_bytes)) {
    ESP_LOGE(TAG, "%s is too large: %lld bytes", url, static_cast<long long>(content_length));
    return http_finish(client, ESP_ERR_INVALID_SIZE);
  }

  char buffer[2048];
  uint32_t idle_reads = 0;
  while (true) {
    const int result = esp_http_client_read(client, buffer, sizeof(buffer));
    if (result > 0) {
      if (out->size() + static_cast<size_t>(result) > max_bytes)
        return http_finish(client, ESP_ERR_INVALID_SIZE);
      out->append(buffer, static_cast<size_t>(result));
      idle_reads = 0;
      continue;
    }
    if (result == 0)
      break;
    if (result == -ESP_ERR_HTTP_EAGAIN) {
      if (++idle_reads > 750)
        return http_finish(client, ESP_ERR_TIMEOUT);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    const esp_err_t read_error = static_cast<esp_err_t>(-result);
    log_http_error("HTTP response read failed", url, client, read_error);
    return http_finish(client, read_error);
  }
  http_finish(client, ESP_OK);
  return ESP_OK;
}

static std::string lower_copy(const std::string &value) {
  std::string result = value;
  for (char &character : result)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return result;
}

static std::string catalog_url_for_href(const std::string &base_url, const std::string &href) {
  if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0)
    return href;
  if (href.rfind("//", 0) == 0)
    return "https:" + href;
  if (!href.empty() && href[0] == '/') {
    const size_t scheme_end = base_url.find("://");
    const size_t host_end = scheme_end == std::string::npos ? std::string::npos : base_url.find('/', scheme_end + 3);
    return base_url.substr(0, host_end) + href;
  }
  return base_url + (base_url.empty() || base_url.back() == '/' ? "" : "/") + href;
}

static bool is_archive_https_url_(const std::string &url) {
  if (url.rfind("https://", 0) != 0)
    return false;
  const size_t begin = 8;
  const size_t end = url.find_first_of("/:?#", begin);
  const std::string host = lower_copy(url.substr(begin, end == std::string::npos ? end : end - begin));
  return host == "archive.org" ||
         (host.size() > 12 && host.compare(host.size() - 12, 12, ".archive.org") == 0);
}

static std::string archive_percent_decode(const std::string &value);

static std::string archive_encode_path(const std::string &value) {
  static constexpr char HEX[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~' ||
        character == '/') {
      result.push_back(static_cast<char>(character));
    } else {
      result.push_back('%');
      result.push_back(HEX[character >> 4]);
      result.push_back(HEX[character & 0x0F]);
    }
  }
  return result;
}

static std::string archive_encode_query(const std::string &value) {
  static constexpr char HEX[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
      result.push_back(static_cast<char>(character));
    } else {
      result.push_back('%');
      result.push_back(HEX[character >> 4]);
      result.push_back(HEX[character & 0x0F]);
    }
  }
  return result;
}

// Archive.org has used both direct /download/... links and
// view_archive.php?archive=...&file=... links for the same "View Contents"
// page. The latter is a browser page, not a ROM download. Convert it to the
// direct member URL before it reaches the SD downloader.
static std::string archive_url_for_href(const std::string &base_url, const std::string &href) {
  const std::string absolute = catalog_url_for_href(base_url, href);
  const size_t query_start = absolute.find('?');
  if (query_start == std::string::npos)
    return absolute;

  const std::string query = absolute.substr(query_start + 1);
  auto query_value = [&query](const char *name) {
    const std::string key = std::string(name) + "=";
    size_t start = 0;
    while (start <= query.size()) {
      const size_t end = query.find('&', start);
      const std::string part = query.substr(start, end == std::string::npos ? end : end - start);
      if (part.rfind(key, 0) == 0)
        return part.substr(key.size());
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
    return std::string();
  };

  const std::string archive = archive_percent_decode(query_value("archive"));
  const std::string member = archive_percent_decode(query_value("file"));
  const size_t items = archive.find("/items/");
  if (items == std::string::npos || member.empty())
    return absolute;
  const std::string item_path = archive.substr(items + 7);
  if (item_path.empty())
    return absolute;

  return "http://archive.org/download/" + archive_encode_path(item_path) + "/" + archive_encode_path(member);
}

static std::string archive_download_base_url(const std::string &url) {
  const size_t marker = url.find("/download/");
  if (marker == std::string::npos)
    return {};
  const size_t identifier_start = marker + 10;
  const size_t identifier_end = url.find('/', identifier_start);
  if (identifier_end == std::string::npos || identifier_end == identifier_start)
    return {};
  return url.substr(0, identifier_end + 1);
}

static std::vector<std::pair<std::string, std::string>> parse_pax_catalog(const std::string &html,
                                                                             const std::string &base_url) {
  std::vector<std::pair<std::string, std::string>> entries;
  const std::string lowered = lower_copy(html);
  size_t search_from = 0;
  while (true) {
    const size_t href_pos = lowered.find("href", search_from);
    if (href_pos == std::string::npos)
      break;
    size_t cursor = href_pos + 4;
    while (cursor < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[cursor])))
      cursor++;
    if (cursor >= lowered.size() || lowered[cursor] != '=') {
      search_from = cursor;
      continue;
    }
    cursor++;
    while (cursor < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[cursor])))
      cursor++;
    if (cursor >= lowered.size() || (lowered[cursor] != '\'' && lowered[cursor] != '"')) {
      search_from = cursor;
      continue;
    }
    const char quote = lowered[cursor++];
    const size_t end = lowered.find(quote, cursor);
    if (end == std::string::npos)
      break;
    std::string href = html.substr(cursor, end - cursor);
    const size_t entity = href.find("&amp;");
    if (entity != std::string::npos)
      href.replace(entity, 5, "&");
    const size_t fragment = href.find_first_of("?#");
    if (fragment != std::string::npos)
      href.resize(fragment);
    const std::string href_lower = lower_copy(href);
    if (href_lower.size() >= 4 && href_lower.compare(href_lower.size() - 4, 4, ".pax") == 0) {
      const std::string url = catalog_url_for_href(base_url, href);
      size_t slash = href.find_last_of('/');
      const std::string name = slash == std::string::npos ? href : href.substr(slash + 1);
      bool duplicate = false;
      for (const auto &existing : entries) {
        if (existing.second == url) {
          duplicate = true;
          break;
        }
      }
      if (!name.empty() && !duplicate)
        entries.emplace_back(name, url);
    }
    search_from = end + 1;
  }
  return entries;
}

static std::string archive_percent_decode(const std::string &value) {
  std::string result;
  result.reserve(value.size());
  for (size_t i = 0; i < value.size(); i++) {
    if (value[i] == '%' && i + 2 < value.size() && std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
      char hex[3] = {value[i + 1], value[i + 2], '\0'};
      result.push_back(static_cast<char>(std::strtoul(hex, nullptr, 16)));
      i += 2;
    } else if (value[i] == '+') {
      result.push_back(' ');
    } else {
      result.push_back(value[i]);
    }
  }
  return result;
}

// Archive.org's View Contents page has changed its HTML a few times. Keep the
// device parser deliberately small: it accepts ordinary hrefs as well as the
// view_archive.php?archive=...&file=... form, and filters only by the
// configured extension list. A missing/blocked listing simply produces an
// empty native list and never turns an error page into a downloadable file.
static std::vector<std::pair<std::string, std::string>> parse_archive_listing(const std::string &html,
                                                                                const std::string &base_url,
                                                                                const std::string &extensions) {
  std::vector<std::pair<std::string, std::string>> entries;
  const std::string lowered = lower_copy(html);
  size_t search_from = 0;
  while (true) {
    const size_t href_pos = lowered.find("href", search_from);
    if (href_pos == std::string::npos)
      break;
    size_t cursor = href_pos + 4;
    while (cursor < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[cursor])))
      cursor++;
    if (cursor >= lowered.size() || lowered[cursor] != '=') {
      search_from = cursor;
      continue;
    }
    cursor++;
    while (cursor < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[cursor])))
      cursor++;
    if (cursor >= lowered.size() || (lowered[cursor] != '\'' && lowered[cursor] != '"')) {
      search_from = cursor;
      continue;
    }
    const char quote = lowered[cursor++];
    const size_t end = lowered.find(quote, cursor);
    if (end == std::string::npos)
      break;
    std::string href = html.substr(cursor, end - cursor);
    for (size_t entity = href.find("&amp;"); entity != std::string::npos; entity = href.find("&amp;", entity)) {
      href.replace(entity, 5, "&");
      entity++;
    }
    if (href.empty() || href[0] == '#') {
      search_from = end + 1;
      continue;
    }
    std::string label = href;
    const size_t file_query = label.find("file=");
    if (file_query != std::string::npos)
      label = label.substr(file_query + 5);
    const size_t query = label.find_first_of("?#&");
    if (query != std::string::npos)
      label.resize(query);
    label = archive_percent_decode(label);
    const size_t slash = label.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < label.size())
      label = label.substr(slash + 1);
    if (label.empty() || label == "." || label == ".." || label == "Parent Directory" ||
        label.find("..") != std::string::npos || label.back() == '/') {
      search_from = end + 1;
      continue;
    }
    const std::string label_lower = lower_copy(label);
    bool extension_match = extensions.empty();
    size_t ext = extensions.find('|');
    size_t start = 0;
    while (!extension_match) {
      const std::string wanted = lower_copy(extensions.substr(start, ext == std::string::npos ? ext : ext - start));
      if (!wanted.empty() && label_lower.size() >= wanted.size() &&
          label_lower.compare(label_lower.size() - wanted.size(), wanted.size(), wanted) == 0)
        extension_match = true;
      if (ext == std::string::npos)
        break;
      start = ext + 1;
      ext = extensions.find('|', start);
    }
    if (!extension_match) {
      search_from = end + 1;
      continue;
    }
    std::string url = archive_url_for_href(base_url, href);
    // archive.org CDN (ia*.us.archive.org) uses a GoDaddy G2 cert not included in the ESP-IDF
    // mbedTLS bundle, causing ESP_ERR_HTTP_CONNECT on TLS handshake. The CDN supports plain HTTP,
    // so downgrade download URLs to avoid the cert chain issue entirely.
    if (url.rfind("https://archive.org/download/", 0) == 0)
      url.replace(0, 5, "http");
    bool duplicate = false;
    for (const auto &existing : entries)
      duplicate = duplicate || existing.second == url;
    if (!duplicate)
      entries.emplace_back(label, url);
    search_from = end + 1;
  }
  std::sort(entries.begin(), entries.end());
  return entries;
}

// ── Launch progress and app data (data_root) ───────────────────────────────

void PaxLoader::set_progress_(bool active, uint32_t done, uint32_t total, const char *format, ...) {
  char status[PROGRESS_STATUS_SIZE];
  va_list args;
  va_start(args, format);
  std::vsnprintf(status, sizeof(status), format, args);
  va_end(args);
  portENTER_CRITICAL(&this->progress_lock_);
  std::memcpy(this->progress_status_, status, sizeof(status));
  this->progress_active_ = active;
  this->progress_done_ = done;
  this->progress_total_ = total;
  this->progress_seq_ = this->progress_seq_ + 1;
  portEXIT_CRITICAL(&this->progress_lock_);
}

float PaxLoader::get_load_progress() {
  portENTER_CRITICAL(&this->progress_lock_);
  const bool active = this->progress_active_;
  const uint32_t done = this->progress_done_;
  const uint32_t total = this->progress_total_;
  portEXIT_CRITICAL(&this->progress_lock_);
  if (!active || total == 0)
    return -1.0f;
  return static_cast<float>(static_cast<double>(done) / static_cast<double>(total));
}

std::string PaxLoader::get_load_status() {
  char status[PROGRESS_STATUS_SIZE];
  portENTER_CRITICAL(&this->progress_lock_);
  std::memcpy(status, this->progress_status_, sizeof(status));
  portEXIT_CRITICAL(&this->progress_lock_);
  status[sizeof(status) - 1] = '\0';
  return status;
}

#ifdef PAX_LOADER_USE_LVGL
// Plain objects and a label, like the page widgets, so this works whichever
// LVGL widgets the config enables.
void PaxLoader::ensure_progress_overlay_() {
  if (this->overlay_panel_ != nullptr)
    return;
  lv_obj_t *panel = lv_obj_create(lv_layer_top());
  lv_obj_set_size(panel, 560, 84);
  lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -28);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B1626), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_radius(panel, 14, 0);
  lv_obj_set_style_pad_all(panel, 14, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);  // never blocks touches on the page
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *label = lv_label_create(panel);
  lv_obj_set_width(label, lv_pct(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(label, lv_color_hex(0xE2E8F0), 0);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *track = lv_obj_create(panel);
  lv_obj_set_size(track, lv_pct(100), 10);
  lv_obj_align(track, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_pad_all(track, 0, 0);
  lv_obj_set_style_border_width(track, 0, 0);
  lv_obj_set_style_radius(track, 5, 0);
  lv_obj_set_style_bg_color(track, lv_color_hex(0x1E293B), 0);
  lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *fill = lv_obj_create(track);
  lv_obj_set_size(fill, 0, lv_pct(100));
  lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_border_width(fill, 0, 0);
  lv_obj_set_style_radius(fill, 5, 0);
  lv_obj_set_style_bg_color(fill, lv_color_hex(0x38BDF8), 0);
  lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);

  this->overlay_panel_ = panel;
  this->overlay_label_ = label;
  this->overlay_fill_ = fill;
}
#endif

void PaxLoader::update_progress_ui_() {
#ifdef PAX_LOADER_USE_LVGL
  if (this->lvgl_ == nullptr || !this->lvgl_->is_loop_started() || this->lvgl_->is_paused())
    return;
  // Redraw when the progress changed, or when another page came on screen
  // (whether the page's own widgets or the loader's panel shows it).
  lv_obj_t *screen = lv_screen_active();
  const uint32_t seq = this->progress_seq_;
  if (seq == this->progress_ui_seq_ && screen == this->progress_ui_screen_)
    return;
  this->progress_ui_seq_ = seq;
  this->progress_ui_screen_ = screen;

  char status[PROGRESS_STATUS_SIZE];
  portENTER_CRITICAL(&this->progress_lock_);
  std::memcpy(status, this->progress_status_, sizeof(status));
  const bool active = this->progress_active_;
  const uint32_t done = this->progress_done_;
  const uint32_t total = this->progress_total_;
  portEXIT_CRITICAL(&this->progress_lock_);
  status[sizeof(status) - 1] = '\0';

  // The page's widgets when they are on screen (and no store detail page covers
  // them), otherwise the loader's panel.
  lv_obj_t *page_widget = this->progress_fill_ != nullptr ? this->progress_fill_ : this->progress_label_;
  const bool page_shows =
      page_widget != nullptr && lv_obj_get_screen(page_widget) == screen && this->detail_panel_ == nullptr;
  if (!page_shows) {
    const bool show = active || status[0] != '\0';
    if (!show && this->overlay_panel_ == nullptr)
      return;
    this->ensure_progress_overlay_();
    if (!show) {
      lv_obj_add_flag(this->overlay_panel_, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    const int32_t percent =
        active && total > 0 ? static_cast<int32_t>(std::min<uint64_t>(100, static_cast<uint64_t>(done) * 100 / total)) : 0;
    lv_obj_set_width(this->overlay_fill_, lv_pct(percent));
    lv_obj_set_style_bg_opa(lv_obj_get_parent(this->overlay_fill_), active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_label_set_text(this->overlay_label_, status[0] != '\0' ? status : "Loading...");
    lv_obj_remove_flag(this->overlay_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(this->overlay_panel_);  // above a store detail page
    if (active) {
      this->cancel_timeout("pax_progress_overlay");
    } else {
      // A failure message floats over every page, so let it go after a while.
      this->set_timeout("pax_progress_overlay", 8000, [this]() {
        if (this->overlay_panel_ != nullptr)
          lv_obj_add_flag(this->overlay_panel_, LV_OBJ_FLAG_HIDDEN);
      });
    }
    return;
  }
  if (this->overlay_panel_ != nullptr)
    lv_obj_add_flag(this->overlay_panel_, LV_OBJ_FLAG_HIDDEN);

  if (this->progress_fill_ != nullptr) {
    lv_obj_t *track = lv_obj_get_parent(this->progress_fill_);
    lv_obj_t *shown = track != nullptr ? track : this->progress_fill_;
    if (active && total > 0) {
      const int32_t percent = static_cast<int32_t>(std::min<uint64_t>(100, static_cast<uint64_t>(done) * 100 / total));
      lv_obj_set_width(this->progress_fill_, lv_pct(percent));
      lv_obj_remove_flag(shown, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(shown, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (this->progress_label_ != nullptr) {
    if (status[0] != '\0') {
      lv_label_set_text(this->progress_label_, status);
      lv_obj_remove_flag(this->progress_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(this->progress_label_, LV_OBJ_FLAG_HIDDEN);
    }
  }
#endif
}

// Creates the folders between `root` and the file `target` (root itself must
// already exist: it is the card's mount point or a folder on it).
bool make_parent_dirs(const std::string &root, const std::string &target) {
  size_t slash = target.find('/');
  while (slash != std::string::npos) {
    const std::string dir = root + "/" + target.substr(0, slash);
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0) {
      if (mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Cannot create %s (errno %d)", dir.c_str(), errno);
        return false;
      }
    } else if (!S_ISDIR(st.st_mode)) {
      ESP_LOGE(TAG, "%s exists and is not a folder", dir.c_str());
      return false;
    }
    slash = target.find('/', slash + 1);
  }
  return true;
}

// The toolchain's C library has no rmdir(). The FAT VFS driver's rmdir and
// unlink both come down to FatFs f_unlink, which deletes a folder only when
// it is empty, so unlink() does the same job; the folder check keeps it to
// folders.
bool remove_empty_folder(const std::string &path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    return false;
  DIR *dir = opendir(path.c_str());
  if (dir == nullptr)
    return false;
  bool empty = true;
  while (dirent *entry = readdir(dir)) {
    if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
      empty = false;
      break;
    }
  }
  closedir(dir);
  return empty && unlink(path.c_str()) == 0;
}

// Returns where `target` (relative, e.g. roms/doom/doom1.wad) already exists:
// under data_root first, then each data_search root; "" when nowhere.
std::vector<std::string> PaxLoader::data_roots_() const {
  std::vector<std::string> roots{this->data_root_};
  for (const auto &root : this->data_search_) {
    if (std::find(roots.begin(), roots.end(), root) == roots.end())
      roots.push_back(root);
  }
  return roots;
}

std::string PaxLoader::find_data_file_(const std::string &target) const {
  for (const auto &root : this->data_roots_()) {
    const std::string path = runtime_path(root.c_str()) + "/" + target;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
      return path;
  }
  return {};
}

esp_err_t PaxLoader::sync_app_data_(const std::string &pax_url, const std::string &app) {
  if (!this->download_data_)
    return ESP_OK;
  const std::string app_name = app.empty() ? canvas::app_key(pax_url) : app;
  const std::string list_url = data::list_url_for(pax_url);
  if (list_url.empty())
    return ESP_OK;

  this->set_progress_(true, 0, 0, "%s", "Checking app data...");
  std::string text;
  esp_err_t err = fetch_http_text(list_url.c_str(), &text);
  if (err == ESP_ERR_NOT_FOUND) {
    ESP_LOGD(TAG, "No data list for this app: %s", list_url.c_str());
    return ESP_OK;
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not read the data list for %s: %s (%s, 0x%x), URL=%s", app_name.c_str(),
             http_error_hint(err), esp_err_to_name(err), static_cast<unsigned>(err), list_url.c_str());
    this->set_progress_(false, 0, 0, "Data list: %.48s (%s)", http_error_hint(err), esp_err_to_name(err));
    return err;
  }
  std::vector<data::DataFile> files;
  std::string error;
  if (!data::parse_list(text, &files, &error)) {
    ESP_LOGE(TAG, "Bad data list %s: %s", list_url.c_str(), error.c_str());
    this->set_progress_(false, 0, 0, "Bad data list: %.60s", error.c_str());
    return ESP_ERR_INVALID_RESPONSE;
  }

  // A file that is already on any storage root (data_root, data_search) is
  // kept whatever its size: it may be the
  // user's own copy (for example a full game instead of the shareware one).
  // Downloads go to <file>.part and are renamed only once verified, so an
  // interrupted download never leaves a partial file under the real name.
  const std::string root = runtime_path(this->data_root_.c_str());
  std::vector<size_t> missing;
  uint64_t total = 0;
  for (size_t i = 0; i < files.size(); i++) {
    const std::string found = this->find_data_file_(files[i].target);
    if (!found.empty()) {
      ESP_LOGI(TAG, "Have %s at %s; not downloading", files[i].target.c_str(), found.c_str());
      continue;
    }
    missing.push_back(i);
    total += files[i].size;
  }
  if (missing.empty()) {
    ESP_LOGI(TAG, "App data present: all %u file(s) found", static_cast<unsigned>(files.size()));
    return ESP_OK;
  }
  if (total > UINT32_MAX) {
    this->set_progress_(false, 0, 0, "%s", "App data is too large");
    return ESP_ERR_INVALID_SIZE;
  }
  ESP_LOGI(TAG, "Downloading %u of %u app data file(s), %lu bytes, into %s", static_cast<unsigned>(missing.size()),
           static_cast<unsigned>(files.size()), static_cast<unsigned long>(total), root.c_str());

  uint32_t done = 0;
  for (size_t n = 0; n < missing.size(); n++) {
    const data::DataFile &file = files[missing[n]];
    if (!make_parent_dirs(root, file.target)) {
      this->set_progress_(false, 0, 0, "Cannot write to %.40s - is the card in?", this->data_root_.c_str());
      return ESP_ERR_NOT_FOUND;
    }
    err = this->download_data_file_(file, root + "/" + file.target, done, static_cast<uint32_t>(total), n + 1,
                                    missing.size());
    if (err != ESP_OK) {
      if (err == ESP_ERR_INVALID_STATE) {
        this->set_progress_(false, 0, 0, "%s", "Download cancelled");
      } else {
        this->set_progress_(false, 0, 0, "Download: %.30s - %.38s (%s)", file.target.c_str(), http_error_hint(err),
                            esp_err_to_name(err));
      }
      return err;
    }
    done += file.size;
    // Remembered for Uninstall, which deletes only what the store put here.
    this->record_download_(app_name, this->data_root_ + "/" + file.target, file.size, file.sha256);
  }
  ESP_LOGI(TAG, "App data ready under %s", root.c_str());
  return ESP_OK;
}

esp_err_t PaxLoader::download_data_file_(const data::DataFile &file, const std::string &path, uint32_t done_before,
                                          uint32_t total, size_t index, size_t count) {
  ESP_LOGI(TAG, "Downloading %s (%lu bytes) from %s", path.c_str(), static_cast<unsigned long>(file.size),
           file.url.c_str());
  if (!network::is_connected())
    return ESP_ERR_INVALID_STATE;
  if (file.url.rfind("https://", 0) == 0) {
    for (uint8_t attempt = 0; attempt < 100 && std::time(nullptr) < MIN_VALID_TLS_TIME; attempt++)
      vTaskDelay(pdMS_TO_TICKS(100));
  }
  std::string current_url = file.url;
  esp_http_client_handle_t client = nullptr;
  int64_t content_length = -1;
  esp_err_t err = ESP_OK;
  for (uint8_t redirect = 0; redirect <= HTTP_MAX_REDIRECTIONS; redirect++) {
    HttpRedirectCapture redirect_capture;
    esp_http_client_config_t config{};
    config.url = current_url.c_str();
    config.event_handler = http_event_redirect_capture;
    config.user_data = &redirect_capture;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.buffer_size = HTTP_READ_BUFFER_SIZE;
    config.buffer_size_tx = 4096;
    config.disable_auto_redirect = true;
    config.max_redirection_count = HTTP_MAX_REDIRECTIONS;
    config.keep_alive_enable = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (current_url.rfind("https://", 0) == 0)
      config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    (void)restore_active_dns_if_missing_();
    client = esp_http_client_init(&config);
    if (client == nullptr)
      return ESP_ERR_NO_MEM;
    if (is_archive_https_url_(current_url)) {
      char cookie_header[1100]{};
      if (PaxLoader::svc_archive_auth_get(cookie_header, sizeof(cookie_header)) > 0) {
        err = esp_http_client_set_header(client, "Cookie", cookie_header);
        std::memset(cookie_header, 0, sizeof(cookie_header));
        if (err != ESP_OK)
          return http_finish(client, err);
      }
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      log_http_error("Data download HTTP open failed", current_url.c_str(), client, err);
      return http_finish(client, err);
    }
    content_length = -1;
    for (uint8_t attempt = 0; attempt < 6; attempt++) {
      content_length = esp_http_client_fetch_headers(client);
      if (content_length != -ESP_ERR_HTTP_EAGAIN)
        break;
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (content_length == -ESP_ERR_HTTP_EAGAIN)
      return http_finish(client, ESP_ERR_TIMEOUT);
    const int status_code = esp_http_client_get_status_code(client);
    if (status_code >= 300 && status_code < 400) {
      if (redirect < HTTP_MAX_REDIRECTIONS && !redirect_capture.location.empty()) {
        current_url = catalog_url_for_href(current_url, redirect_capture.location);
        ESP_LOGI(TAG, "Following download redirect %u: -> %s", static_cast<unsigned>(redirect + 1), current_url.c_str());
        http_finish(client, ESP_OK);
        client = nullptr;
        continue;
      }
      ESP_LOGE(TAG, "Download redirect %d has no usable Location or exceeded %u hops: %s", status_code,
               static_cast<unsigned>(HTTP_MAX_REDIRECTIONS), current_url.c_str());
      return http_finish(client, ESP_ERR_HTTP_MAX_REDIRECT);
    }
    if (status_code != 200) {
      ESP_LOGE(TAG, "HTTP status %d: %s", status_code, current_url.c_str());
      return http_finish(client, ESP_ERR_INVALID_RESPONSE);
    }
    break;
  }
  if (content_length > 0 && file.size != 0 && static_cast<uint64_t>(content_length) != file.size) {
    ESP_LOGE(TAG, "%s is %lld bytes, the list says %lu", file.url.c_str(), static_cast<long long>(content_length),
             static_cast<unsigned long>(file.size));
    return http_finish(client, ESP_ERR_INVALID_SIZE);
  }
  if (content_length > 1024LL * 1024LL * 1024LL)
    return http_finish(client, ESP_ERR_INVALID_SIZE);
  const uint32_t expected_size =
      file.size != 0 ? file.size : (content_length > 0 ? static_cast<uint32_t>(content_length) : 0);
  const uint32_t progress_total = total != 0 ? total : expected_size;
  const int64_t download_started_us = esp_timer_get_time();
  if (content_length > 0 && file.size == 0) {
    ESP_LOGI(TAG, "Resolved download size for %s: %llu bytes",
             file.url.c_str(), static_cast<unsigned long long>(content_length));
  } else if (content_length <= 0 && file.size == 0) {
    ESP_LOGW(TAG, "Download size is not provided by %s; progress will show the live byte rate",
             file.url.c_str());
  }
  if (progress_total > 0) {
    this->set_progress_(true, done_before, progress_total, "%u/%u %.32s  %.1f / %.1f MB  0.00 MB/s",
                        static_cast<unsigned>(index), static_cast<unsigned>(count), file.target.c_str(),
                        done_before / 1048576.0, progress_total / 1048576.0);
  } else {
    this->set_progress_(true, done_before, 0, "%u/%u %.32s  %.1f MB  0.00 MB/s  size unknown",
                        static_cast<unsigned>(index), static_cast<unsigned>(count), file.target.c_str(),
                        done_before / 1048576.0);
  }

  // A reset can happen after the old target was moved to its backup but
  // before the staged file was committed. Recover the old target first, then
  // discard any unverified download left behind by that interrupted attempt.
  recover_staged_file_(path, ".upload.bak");
  struct stat existing_target{};
  if (stat(path.c_str(), &existing_target) == 0 && !S_ISREG(existing_target.st_mode)) {
    ESP_LOGE(TAG, "Download target is not a regular file: %s", path.c_str());
    return ESP_ERR_INVALID_ARG;
  }
  const std::string part = path + ".part";
  FILE *out = std::fopen(part.c_str(), "wb");
#ifdef PAX_LOADER_USE_SD_MMC
  // A card can remain visible to directory reads after an eject/reinsert or
  // after the SD controller has lost its write state.  In that case FatFs
  // reports EIO while creating the staging file.  Reinitialise the ESPHome
  // SD component once and retry before reporting the install as missing.
  if (out == nullptr && this->sd_mmc_ != nullptr && path.rfind("/sdcard/", 0) == 0 &&
      (errno == EIO || errno == ENODEV || errno == EROFS)) {
    const int first_errno = errno;
    ESP_LOGW(TAG, "Cannot create %s (errno %d); remounting SD card and retrying", part.c_str(), first_errno);
    this->sd_mmc_->unmount();
    vTaskDelay(pdMS_TO_TICKS(250));
    this->sd_mmc_->mount();
    vTaskDelay(pdMS_TO_TICKS(250));
    errno = 0;
    out = std::fopen(part.c_str(), "wb");
    if (out != nullptr)
      ESP_LOGI(TAG, "Created %s after SD remount", part.c_str());
  }
#endif
  if (out == nullptr) {
    ESP_LOGE(TAG, "Cannot create %s (errno %d)", part.c_str(), errno);
    return http_finish(client, ESP_ERR_NOT_FOUND);
  }
  auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(HTTP_READ_BUFFER_SIZE, MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    std::fclose(out);
    std::remove(part.c_str());
    return http_finish(client, ESP_ERR_NO_MEM);
  }

  const char *slash = std::strrchr(file.target.c_str(), '/');
  const char *name = slash != nullptr ? slash + 1 : file.target.c_str();
  data::Sha256 hash;
  uint32_t received = 0;
  uint32_t idle_reads = 0;
  while (file.size == 0 || received < expected_size) {
    if (this->global_close_requested_) {
      err = ESP_ERR_INVALID_STATE;
      break;
    }
    size_t chunk = std::min<size_t>(HTTP_READ_BUFFER_SIZE, expected_size - received);
    if (file.size == 0) {
      const int result = esp_http_client_read(client, reinterpret_cast<char *>(buffer), HTTP_READ_BUFFER_SIZE);
      if (result > 0) {
        chunk = static_cast<size_t>(result);
        idle_reads = 0;
      } else if (result == 0) {
        break;
      } else if (result == -ESP_ERR_HTTP_EAGAIN && ++idle_reads <= 750) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      } else {
        err = result == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : static_cast<esp_err_t>(-result);
        break;
      }
    } else {
      err = http_read_exact(client, buffer, chunk);
      if (err != ESP_OK)
        break;
    }
    if (std::fwrite(buffer, 1, chunk, out) != chunk) {
      ESP_LOGE(TAG, "Write to %s failed (errno %d); is the card full?", part.c_str(), errno);
      err = ESP_FAIL;
      break;
    }
    hash.update(buffer, chunk);
    received += static_cast<uint32_t>(chunk);
    const uint32_t overall = done_before + received;
    const int64_t elapsed_us = std::max<int64_t>(1, esp_timer_get_time() - download_started_us);
    const double megabytes_per_second =
        static_cast<double>(received) * 1000000.0 / static_cast<double>(elapsed_us) / 1048576.0;
    if (progress_total > 0) {
      this->set_progress_(true, overall, progress_total, "%u/%u %.32s  %.1f / %.1f MB  %.2f MB/s",
                          static_cast<unsigned>(index), static_cast<unsigned>(count), name, overall / 1048576.0,
                          progress_total / 1048576.0, megabytes_per_second);
    } else {
      this->set_progress_(true, overall, 0, "%u/%u %.32s  %.1f MB  %.2f MB/s  size unknown",
                          static_cast<unsigned>(index), static_cast<unsigned>(count), name, overall / 1048576.0,
                          megabytes_per_second);
    }
  }
  if (err == ESP_OK && file.size != 0 && received != expected_size)
    err = ESP_ERR_INVALID_SIZE;
  heap_caps_free(buffer);
  if (err == ESP_OK && !sync_file_(out, part.c_str()))
    err = ESP_FAIL;
  if (std::fclose(out) != 0 && err == ESP_OK)
    err = ESP_FAIL;
  out = nullptr;
  http_finish(client, ESP_OK);

  if (err == ESP_OK && !file.sha256.empty()) {
    const std::string digest = hash.hex();
    if (digest != file.sha256) {
      ESP_LOGE(TAG, "%s: sha256 %s, expected %s", path.c_str(), digest.c_str(), file.sha256.c_str());
      err = ESP_ERR_INVALID_CRC;
    }
  }
  if (err == ESP_OK) {
    err = commit_upload_(path, part);
  }
  if (err != ESP_OK) {
    std::remove(part.c_str());
    return err;
  }
  ESP_LOGI(TAG, "Saved %s%s", path.c_str(), file.sha256.empty() ? "" : " (sha256 ok)");
  return ESP_OK;
}


bool PaxLoader::request_firmware_update(uint32_t command, uint64_t expected_build) {
  if (pax_c6::busy() || this->install_task_handle_ || this->web_storage_busy_) return false;
  (void)restore_active_dns_if_missing_();
  pax_firmware_request_t request{PAX_FIRMWARE_MAGIC,command,expected_build};
  FILE *file=pax_firmware::open("wb");if(!file)return false;
  bool ok=fwrite(&request,1,sizeof(request),file)==sizeof(request);
  return fclose(file)==0 && ok;
}

bool PaxLoader::begin_web_transfer() {
  if (pax_firmware::busy() || this->web_storage_busy_ || this->launched_ || this->pax_loading_ || this->launch_pending_ ||
      this->install_task_handle_ != nullptr || this->archive_loading_)
    return false;
  this->global_close_requested_ = false;
  this->web_storage_busy_ = true;
  return true;
}

void PaxLoader::end_web_transfer() {
  // Starter downloads share the loader's progress panel. The last file can
  // finish with active=true; clear it before handing the screen back.
  this->set_progress_(false, 0, 0, "%s", "");
  this->web_storage_busy_ = false;
  this->refresh_catalog();
}

esp_err_t PaxLoader::download_web_file(const data::DataFile &file) {
  if (!this->web_storage_busy_ || !data::safe_target(file.target) || !data::is_sha256_hex(file.sha256) ||
      !is_network_url(file.url.c_str()) || file.size == 0 || file.size > data::MAX_FILE_SIZE)
    return ESP_ERR_INVALID_ARG;
  const std::string root = runtime_path("/sd");
#ifdef PAX_LOADER_USE_SD_MMC
  struct stat mounted{};
  if (this->sd_mmc_ != nullptr && stat(root.c_str(), &mounted) != 0)
    this->sd_mmc_->mount();
#endif
  if (!make_parent_dirs(root, file.target))
    return ESP_ERR_NOT_FOUND;
  return this->download_data_file_(file, root + "/" + file.target, 0, file.size, 1, 1);
}

esp_err_t PaxLoader::receive_web_file(const std::string &path, uint32_t size, const std::string &sha256,
                                     WebRead read, void *context) {
  if (!this->web_storage_busy_ || path.rfind("/sd/", 0) != 0 || !data::safe_target(path.substr(4)) ||
      size > data::MAX_FILE_SIZE || !data::is_sha256_hex(sha256) || read == nullptr)
    return ESP_ERR_INVALID_ARG;
  const std::string root = runtime_path("/sd");
#ifdef PAX_LOADER_USE_SD_MMC
  struct stat mounted{};
  if (this->sd_mmc_ != nullptr && stat(root.c_str(), &mounted) != 0)
    this->sd_mmc_->mount();
#endif
  if (!make_parent_dirs(root, path.substr(4)))
    return ESP_ERR_NOT_FOUND;
  const std::string target = runtime_path(path.c_str());
  recover_staged_file_(target, ".upload.bak");
  struct stat existing{};
  if (stat(target.c_str(), &existing) == 0 && !S_ISREG(existing.st_mode))
    return ESP_ERR_INVALID_ARG;
  const std::string part = target + ".part";
  FILE *out = std::fopen(part.c_str(), "wb");
  if (out == nullptr)
    return ESP_ERR_NOT_FOUND;
  uint8_t buffer[4096];
  data::Sha256 hash;
  uint32_t done = 0;
  esp_err_t err = ESP_OK;
  this->set_progress_(true, 0, size, "Uploading %.60s", path.c_str());
  while (done < size) {
    const int count = read(context, buffer, std::min<size_t>(sizeof(buffer), size - done));
    if (count <= 0 || static_cast<size_t>(count) > std::min<size_t>(sizeof(buffer), size - done)) {
      err = ESP_ERR_INVALID_SIZE;
      break;
    }
    if (std::fwrite(buffer, 1, count, out) != static_cast<size_t>(count)) {
      err = ESP_FAIL;
      break;
    }
    hash.update(buffer, count);
    done += count;
    this->set_progress_(true, done, size, "Uploading %.48s (%lu / %lu bytes)", path.c_str(),
                        static_cast<unsigned long>(done), static_cast<unsigned long>(size));
  }
  if (err == ESP_OK && hash.hex() != sha256)
    err = ESP_ERR_INVALID_CRC;
  if (err == ESP_OK && !sync_file_(out, part.c_str()))
    err = ESP_FAIL;
  if (std::fclose(out) != 0 && err == ESP_OK)
    err = ESP_FAIL;
  if (err == ESP_OK)
    err = commit_upload_(target, part);
  if (err != ESP_OK)
    std::remove(part.c_str());
  this->set_progress_(false, done, size, "%s %.48s: %s", err == ESP_OK ? "Saved" : "Upload failed",
                      path.c_str(), esp_err_to_name(err));
  return err;
}

esp_err_t psram_app_load_url(const char *url, psram_app_handle_t *out_handle) {
  if (url == nullptr || out_handle == nullptr || !is_network_url(url))
    return ESP_ERR_INVALID_ARG;
  *out_handle = nullptr;

  ESP_LOGI(TAG, "Streaming PAX from URL: %s", url);
  if (!network::is_connected())
    return ESP_ERR_INVALID_STATE;

  esp_http_client_config_t config{};
  config.url = url;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.buffer_size = HTTP_READ_BUFFER_SIZE;
  config.buffer_size_tx = 4096;
  config.disable_auto_redirect = false;
  config.max_redirection_count = HTTP_MAX_REDIRECTIONS;
  config.keep_alive_enable = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (std::strncmp(url, "https://", 8) == 0)
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

  (void)restore_active_dns_if_missing_();
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr)
    return ESP_ERR_NO_MEM;

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    log_http_error("PAX HTTP open failed", url, client, err);
    return http_finish(client, err);
  }

  int64_t content_length = -1;
  for (uint8_t attempt = 0; attempt < 6; attempt++) {
    content_length = esp_http_client_fetch_headers(client);
    if (content_length != -ESP_ERR_HTTP_EAGAIN)
      break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (content_length == -ESP_ERR_HTTP_EAGAIN)
    return http_finish(client, ESP_ERR_TIMEOUT);

  const int status_code = esp_http_client_get_status_code(client);
  if (status_code != 200) {
    ESP_LOGE(TAG, "Network PAX HTTP status: %d", status_code);
    return http_finish(client, ESP_ERR_INVALID_RESPONSE);
  }

  pax_header_t header{};
  err = http_read_exact(client, reinterpret_cast<uint8_t *>(&header), sizeof(header));
  if (err != ESP_OK)
    return http_finish(client, err);

  if (header.magic != PAX_MAGIC || header.version != PAX_ABI_VERSION || header.flags != 0 ||
      header.text_size == 0 || header.entry_off >= header.text_size) {
    ESP_LOGE(TAG, "Invalid network PAX header: magic=0x%08lx version=%lu text=%lu entry=%lu flags=0x%08lx",
             static_cast<unsigned long>(header.magic), static_cast<unsigned long>(header.version),
             static_cast<unsigned long>(header.text_size), static_cast<unsigned long>(header.entry_off),
             static_cast<unsigned long>(header.flags));
    return http_finish(client, ESP_ERR_INVALID_ARG);
  }

  const uint64_t load_size_64 = static_cast<uint64_t>(header.text_size) + header.data_size;
  const uint64_t total_size_64 = load_size_64 + header.bss_size;
  const uint64_t wire_size_64 = static_cast<uint64_t>(PAX_HEADER_SIZE) + load_size_64;
  if (load_size_64 < header.text_size || total_size_64 < load_size_64 ||
      total_size_64 > MAX_NETWORK_PAX_SIZE || load_size_64 > SIZE_MAX || total_size_64 > SIZE_MAX ||
      (content_length > 0 && static_cast<uint64_t>(content_length) != wire_size_64)) {
    ESP_LOGE(TAG, "Network PAX size rejected: wire=%lld expected=%llu total=%llu",
             static_cast<long long>(content_length), static_cast<unsigned long long>(wire_size_64),
             static_cast<unsigned long long>(total_size_64));
    return http_finish(client, ESP_ERR_INVALID_SIZE);
  }

  const size_t load_size = static_cast<size_t>(load_size_64);
  const size_t total_size = static_cast<size_t>(total_size_64);
  const size_t allocated = align_up(total_size, MMU_PAGE_SIZE);
  if (allocated == 0 || allocated > MAX_NETWORK_PAX_SIZE)
    return http_finish(client, ESP_ERR_INVALID_SIZE);

  auto app = static_cast<psram_app_handle_t>(std::calloc(1, sizeof(struct psram_app)));
  if (app == nullptr)
    return http_finish(client, ESP_ERR_NO_MEM);
  app->header = header;
  app->code_alloc = allocated;
  app->code_buf = heap_caps_aligned_alloc(MMU_PAGE_SIZE, allocated, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (app->code_buf == nullptr) {
    std::free(app);
    return http_finish(client, ESP_ERR_NO_MEM);
  }

  auto *transfer_buffer = static_cast<uint8_t *>(heap_caps_malloc(HTTP_READ_BUFFER_SIZE, MALLOC_CAP_8BIT));
  if (transfer_buffer == nullptr) {
    heap_caps_free(app->code_buf);
    std::free(app);
    return http_finish(client, ESP_ERR_NO_MEM);
  }

  const char *slash = std::strrchr(url, '/');
  const char *file_name = slash != nullptr ? slash + 1 : url;
  size_t received = 0;
  const int64_t download_started_us = esp_timer_get_time();
  if (PaxLoader::active() != nullptr) {
    PaxLoader::active()->set_progress_(true, 0, static_cast<uint32_t>(load_size),
                                        "Loading %.40s  0 / %u KB  0.00 MB/s", file_name,
                                        static_cast<unsigned>(load_size / 1024));
  }
  while (received < load_size) {
    const size_t chunk = std::min(HTTP_READ_BUFFER_SIZE, load_size - received);
    err = http_read_exact(client, transfer_buffer, chunk);
    if (err != ESP_OK)
      break;
    std::memcpy(static_cast<uint8_t *>(app->code_buf) + received, transfer_buffer, chunk);
    received += chunk;
    if (PaxLoader::active() != nullptr) {
      const int64_t elapsed_us = std::max<int64_t>(1, esp_timer_get_time() - download_started_us);
      const double megabytes_per_second =
          static_cast<double>(received) * 1000000.0 / static_cast<double>(elapsed_us) / 1048576.0;
      PaxLoader::active()->set_progress_(true, static_cast<uint32_t>(received), static_cast<uint32_t>(load_size),
                                          "Loading %.40s  %u / %u KB  %.2f MB/s", file_name,
                                          static_cast<unsigned>(received / 1024),
                                          static_cast<unsigned>(load_size / 1024), megabytes_per_second);
    }
    if ((received % (256 * 1024)) < chunk || received == load_size)
      ESP_LOGI(TAG, "Network PAX download: %u/%u bytes", static_cast<unsigned>(received),
               static_cast<unsigned>(load_size));
  }
  heap_caps_free(transfer_buffer);
  if (err != ESP_OK) {
    heap_caps_free(app->code_buf);
    std::free(app);
    return http_finish(client, err);
  }

  std::memset(static_cast<uint8_t *>(app->code_buf) + load_size, 0, allocated - load_size);
  esp_cache_msync(app->code_buf, app->code_alloc,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  http_finish(client, ESP_OK);
  *out_handle = app;
  ESP_LOGI(TAG, "Network PAX loaded: text=%lu data=%lu bss=%lu entry=%lu",
           static_cast<unsigned long>(header.text_size), static_cast<unsigned long>(header.data_size),
           static_cast<unsigned long>(header.bss_size), static_cast<unsigned long>(header.entry_off));
  return ESP_OK;
}

// The PAX writes its .data and .bss through exec_ptr, so the data cache can
// still hold dirty lines for that alias when the app returns. esp_mmu_unmap()
// removes the MMU entries without writing them back. A later eviction then
// targets an address with no mapping, and the PSRAM controller aborts the
// system ("MSPI PSRAM error"); Red Alert's large globals hit this on every
// close. Write the alias back and drop its lines before unmapping.
static void unmap_exec_alias(psram_app_handle_t handle) {
  const esp_err_t err = esp_cache_msync(handle->exec_ptr, handle->code_alloc,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                                            ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "PAX cache writeback before unmap failed: %s", esp_err_to_name(err));
  esp_mmu_unmap(handle->exec_ptr);
  handle->exec_ptr = nullptr;
  handle->mapped = false;
}

int psram_app_run(psram_app_handle_t handle) {
  if (handle == nullptr || handle->code_buf == nullptr || PaxLoader::active() == nullptr)
    return -1;

  esp_paddr_t physical = 0;
  mmu_target_t target = MMU_TARGET_PSRAM0;
  esp_err_t err = esp_mmu_vaddr_to_paddr(handle->code_buf, &physical, &target);
  if (err != ESP_OK)
    return -1;

  const mmu_mem_caps_t caps = static_cast<mmu_mem_caps_t>(
      MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT);
  err = esp_mmu_map(physical, handle->code_alloc, MMU_TARGET_PSRAM0, caps,
                    ESP_MMU_MMAP_FLAG_PADDR_SHARED, &handle->exec_ptr);
  if (err != ESP_OK)
    return -1;
  handle->mapped = true;
  esp_cache_msync(handle->exec_ptr, handle->code_alloc,
                  ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);

  // Zeroed room after the table: an app built against a newer psram_app.h
  // reads NULL for services appended after this loader, not stack garbage.
  struct {
    app_services_t services;
    void *appended_later[PAX_SERVICES_SPARE_SLOTS];
  } table{};
  PaxLoader::populate_services(&table.services);
  auto entry = reinterpret_cast<pax_entry_fn_t>(static_cast<uint8_t *>(handle->exec_ptr) + handle->header.entry_off);
  ESP_LOGI(TAG, "Calling PAX entry point at %p", reinterpret_cast<void *>(entry));
  const int result = entry(&table.services);

  unmap_exec_alias(handle);
  return result;
}

void psram_app_unload(psram_app_handle_t handle) {
  if (handle == nullptr)
    return;
  if (handle->mapped && handle->exec_ptr != nullptr)
    unmap_exec_alias(handle);
  if (handle->code_buf != nullptr)
    heap_caps_free(handle->code_buf);
  std::free(handle);
}

esp_err_t psram_app_selftest(void) {
  ESP_LOGW(TAG, "PSRAM self-test is not exposed by the ESPHome component");
  return ESP_ERR_NOT_SUPPORTED;
}

}  // namespace pax_loader
}  // namespace esphome

// psram_app.h exposes the loader API with C linkage because the same header
// is included by the separately linked PAX applications.  The implementation
// above lives in the ESPHome namespace, so provide the global C ABI entry
// points expected by the launcher's calls and by the linker.
extern "C" esp_err_t psram_app_load(const char *path, psram_app_handle_t *out_handle) {
  return esphome::pax_loader::psram_app_load(path, out_handle);
}

extern "C" esp_err_t psram_app_load_url(const char *url, psram_app_handle_t *out_handle) {
  return esphome::pax_loader::psram_app_load_url(url, out_handle);
}

extern "C" int psram_app_run(psram_app_handle_t handle) {
  return esphome::pax_loader::psram_app_run(handle);
}

extern "C" void psram_app_unload(psram_app_handle_t handle) {
  esphome::pax_loader::psram_app_unload(handle);
}

extern "C" esp_err_t psram_app_selftest(void) {
  return esphome::pax_loader::psram_app_selftest();
}
