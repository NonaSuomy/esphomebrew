// ESPHOMEBREW store view: app listings (<app>.json next to each .pax), an
// icon grid with INSTALLED / UPDATE badges, a detail page and Install.
//
// Everything that touches LVGL runs on the ESPHome main loop (the LVGL
// thread); listings are fetched and apps installed in their own tasks, which
// hand results back through flags the loop polls.

#include "pax_loader.h"
#include "pax_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/log.h"
#include "mbedtls/base64.h"

#include "PNGdec.h"

namespace esphome {
namespace pax_loader {

static const char *const TAG = "pax_store";

// A listing with a 256x256 icon is about 90 KB of JSON.
static constexpr size_t INFO_MAX_BYTES = 192 * 1024;
static constexpr uint16_t TILE_W = 176;
static constexpr uint16_t TILE_H = 188;
static constexpr uint16_t TILE_GAP = 12;
static constexpr uint16_t TILE_ICON = 112;
// Keep the catalog cache compact. The PNG is decoded to this size and is
// centered in the 112px tile; the detail page can still use the small source
// PNG for its larger preview.
static constexpr uint16_t TILE_ICON_STORAGE = 64;
static constexpr uint16_t DETAIL_ICON = 192;
// A few common covers (including Quake) compress to slightly more than 16 KB.
// Retain these small sources so the detail page can render them at full size;
// the cap still prevents large catalog art from accumulating in PSRAM.
static constexpr size_t DETAIL_ICON_PNG_MAX_BYTES = 32 * 1024;

static bool write_text_file(const std::string &path, const std::string &text);

static bool sync_store_file_(FILE *file, const char *path) {
  if (file == nullptr || std::fflush(file) != 0) {
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

static bool sync_store_path_(const std::string &path) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return false;
  const bool ok = fsync(fd) == 0;
  close(fd);
  return ok;
}

static constexpr uint32_t COLOR_TILE = 0x111C33;
static constexpr uint32_t COLOR_TILE_BORDER = 0x1E2A44;
static constexpr uint32_t COLOR_ACCENT = 0x38BDF8;
static constexpr uint32_t COLOR_PAGE = 0x08111F;
static constexpr uint32_t COLOR_TEXT = 0xE2E8F0;
static constexpr uint32_t COLOR_MUTED = 0x94A3B8;
static constexpr uint32_t COLOR_INSTALLED = 0x22C55E;
static constexpr uint32_t COLOR_UPDATE = 0xF59E0B;
// Transparent icon pixels are blended onto the tile colour (PNGdec takes 0xBBGGRR).
static constexpr uint32_t ICON_BLEND_BGR = 0x331C11;

// Store tiles, detail actions, drawers, and ROM rows are created after the
// YAML page, so LVGL cannot add them to the keypad group during code
// generation. Put every dynamic focus target in the default group explicitly;
// otherwise a USB/gamepad d-pad can only reach the static toolbar widgets.
static void add_to_default_group(lv_obj_t *object) {
  if (object == nullptr)
    return;
  lv_group_t *group = lv_group_get_default();
  if (group != nullptr)
    lv_group_add_obj(group, object);
}

enum DetailAction : uint8_t {
  ACTION_STREAM,
  ACTION_INSTALL,
  ACTION_LAUNCH,
  ACTION_ROM,
  ACTION_SCREEN,
  ACTION_FAVORITE,
  ACTION_BACK,
  ACTION_UNINSTALL,
};
// The buttons of Uninstall's confirmation.
enum DialogAction : uint8_t { DIALOG_TOGGLE_DATA, DIALOG_CONFIRM, DIALOG_CANCEL };

// The listing next to a .pax: the same name with .json.
static std::string sidecar_for(const std::string &pax) {
  const std::string path = pax.substr(0, pax.find_first_of("?#"));
  if (path.size() < 4)
    return {};
  std::string ext = path.substr(path.size() - 4);
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".pax" ? path.substr(0, path.size() - 4) + ".json" : std::string();
}

// The catalog parses each listing to collect the metadata and tile icon. Do
// not retain the raw JSON here: even after removing the embedded icon, keeping
// one sidecar string per app makes a large local catalog consume enough heap
// to make later ArduinoJson documents fail with NoMemory. Install can refetch
// the sibling sidecar from the same local/network source when it needs to
// preserve the full listing next to the PAX.
// <install_dir>/<app>.installed.json: the data files the store downloaded.
static constexpr const char *MANIFEST_SUFFIX = ".installed.json";

static bool is_manifest_name(const std::string &file) {
  const size_t n = std::strlen(MANIFEST_SUFFIX);
  return file.size() > n && file.compare(file.size() - n, n, MANIFEST_SUFFIX) == 0;
}

static esp_err_t read_small_file(const std::string &path, std::string *out, size_t max_bytes) {
  // Do not use fread() here. Newlib lazily creates a FreeRTOS semaphore for
  // every FILE lock; when TLS, USB and I2S are temporarily competing for the
  // last internal blocks, that lazy allocation can call abort() on OOM. The
  // descriptor API has no per-FILE lock and is sufficient for these small,
  // read-only JSON sidecars.
  const int fd = open(runtime_path(path.c_str()).c_str(), O_RDONLY);
  if (fd < 0)
    return ESP_ERR_NOT_FOUND;
  out->clear();
  char buffer[1024];
  while (true) {
    const ssize_t result = read(fd, buffer, sizeof(buffer));
    if (result == 0)
      break;
    if (result < 0) {
      close(fd);
      return ESP_FAIL;
    }
    const size_t got = static_cast<size_t>(result);
    if (out->size() + got > max_bytes) {
      close(fd);
      return ESP_ERR_INVALID_SIZE;
    }
    out->append(buffer, got);
  }
  close(fd);
  return ESP_OK;
}

// A listing's canvas sizes: ["1024x600", "800x480"].
static void parse_canvas_sizes(JsonArray sizes, std::vector<std::string> *out) {
  for (JsonVariant entry : sizes) {
    const char *value = entry | "";
    int w = 0, h = 0;
    if (canvas::parse_size(value, &w, &h))
      out->emplace_back(canvas::format_size(w, h));
  }
}

static std::string parse_canvas_size(const char *value) {
  int w = 0, h = 0;
  return value != nullptr && canvas::parse_size(value, &w, &h) ? canvas::format_size(w, h) : std::string();
}

// icon: decode the base64 icon too (not needed to read a size at launch).
static bool parse_app_info(const std::string &text, PaxLoader::AppInfo *info, bool icon = true) {
  return json::parse_json(text, [info, icon](JsonObject root) -> bool {
    info->name = root["name"] | "";
    info->title = root["title"] | "";
    info->version = root["version"] | "";
    info->author = root["author"] | "";
    info->category = root["category"] | "";
    info->license = root["license"] | "";
    info->about = root["about"] | (root["description"] | "");
    info->changelog = root["changelog"] | "";
    info->size = root["size"] | 0u;
    info->data_size = root["data_size"] | 0u;
    info->sha256 = root["sha256"] | "";
    JsonArray controls = root["controls"].as<JsonArray>();
    for (JsonVariant line : controls) {
      const char *value = line | "";
      if (*value != '\0')
        info->controls.emplace_back(value);
    }
    // "canvas": true, or the sizes the app draws: ["1024x600", "800x480"], or
    // {"sizes": [...], "recommended": "800x480"} (no sizes: any). The store
    // publishes the recommendation as "canvas_recommended", next to a plain
    // "canvas" that older loaders read.
    if (root["canvas"].is<bool>()) {
      info->canvas_any = root["canvas"].as<bool>();
    } else if (root["canvas"].is<JsonObject>()) {
      JsonArray sizes = root["canvas"]["sizes"].as<JsonArray>();  // named: no dangling-reference warning
      parse_canvas_sizes(sizes, &info->canvas_sizes);
      info->canvas_any = sizes.isNull();
      info->canvas_recommended = parse_canvas_size(root["canvas"]["recommended"] | "");
    } else {
      JsonArray sizes = root["canvas"].as<JsonArray>();
      parse_canvas_sizes(sizes, &info->canvas_sizes);
    }
    if (info->canvas_recommended.empty())
      info->canvas_recommended = parse_canvas_size(root["canvas_recommended"] | "");
    // "requires": what the app needs on the card. The data files the store
    // downloads count too (the store lists them there; a listing that only
    // has them under "data" gets them from there).
    JsonArray required = root["requires"].as<JsonArray>();
    for (JsonVariant entry : required) {
      PaxLoader::AppInfo::RequiredFile file;
      file.path = entry["path"] | "";
      file.note = entry["note"] | "";
      file.optional = entry["optional"] | false;
      file.download = entry["download"] | false;
      if (files::parse_required(file.path, nullptr) && info->required_files.size() < 96)
        info->required_files.push_back(std::move(file));
    }
    JsonArray data_files = root["data"]["files"].as<JsonArray>();
    for (JsonVariant entry : data_files) {
      PaxLoader::AppInfo::RequiredFile file;
      file.path = std::string("/sd/") + (entry["target"] | "");
      file.download = true;
      bool listed = false;
      for (const auto &have : info->required_files)
        listed = listed || have.path == file.path;
      if (!listed && files::parse_required(file.path, nullptr) && info->required_files.size() < 96)
        info->required_files.push_back(std::move(file));
    }
    const char *project = root["upstream"]["project"] | "";
    const char *upstream_version = root["upstream"]["version"] | "";
    if (*project != '\0')
      info->upstream = *upstream_version != '\0' ? std::string(project) + " " + upstream_version : project;
    const char *repo = root["source"]["repo"] | "";
    const char *ref = root["source"]["ref"] | "";
    if (*repo != '\0') {
      std::string short_repo = repo;
      const size_t at = short_repo.find("github.com/");
      if (at != std::string::npos)
        short_repo = short_repo.substr(at + 11);
      info->source = short_repo + (*ref != '\0' ? std::string(" @ ") + std::string(ref).substr(0, 7) : "");
    }
    const char *icon_base64 = icon ? (root["icon"]["base64"] | "") : "";
    const size_t icon_len = std::strlen(icon_base64);
    if (icon_len > 0) {
      info->icon_png.resize(icon_len * 3 / 4 + 4);
      size_t decoded = 0;
      if (mbedtls_base64_decode(info->icon_png.data(), info->icon_png.size(), &decoded,
                                reinterpret_cast<const unsigned char *>(icon_base64), icon_len) == 0) {
        info->icon_png.resize(decoded);
      } else {
        info->icon_png.clear();
      }
    }
    info->has_info = true;
    return true;
  });
}

static std::string size_text(uint32_t bytes) {
  char text[24];
  if (bytes >= 1024 * 1024)
    std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
  else
    std::snprintf(text, sizeof(text), "%u KB", static_cast<unsigned>((bytes + 1023) / 1024));
  return text;
}

// ── Icon decoding (runs in the info task, and for the detail page) ─────────

struct IconDecode {
  PNG *png;
  uint16_t *row;       // one decoded line
  uint32_t *acc;       // box-filter accumulators, 3 per output pixel
  uint16_t *counts;    // source pixels per output pixel
  uint16_t *out;
  uint16_t src_w, src_h, side;
};

static void icon_draw(PNGDRAW *draw) {
  auto *d = static_cast<IconDecode *>(draw->pUser);
  d->png->getLineAsRGB565(draw, d->row, PNG_RGB565_LITTLE_ENDIAN, ICON_BLEND_BGR);
  const uint32_t oy = static_cast<uint32_t>(draw->y) * d->side / d->src_h;
  for (uint32_t x = 0; x < d->src_w; x++) {
    const uint32_t ox = x * d->side / d->src_w;
    const uint16_t p = d->row[x];
    uint32_t *a = d->acc + (oy * d->side + ox) * 3;
    a[0] += p >> 11;
    a[1] += (p >> 5) & 0x3F;
    a[2] += p & 0x1F;
    d->counts[oy * d->side + ox]++;
  }
}

// Decodes a PNG and box-filters it down (or nearest-up) to side x side RGB565
// in PSRAM. Safe from any task: each call has its own decoder.
static std::shared_ptr<uint16_t> decode_png_icon(const std::vector<uint8_t> &png_bytes, uint16_t side) {
  if (png_bytes.empty())
    return nullptr;
  void *memory = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory == nullptr)
    return nullptr;
  PNG *png = new (memory) PNG();
  std::shared_ptr<uint16_t> result;
  if (png->openRAM(const_cast<uint8_t *>(png_bytes.data()), static_cast<int>(png_bytes.size()), icon_draw) ==
      PNG_SUCCESS) {
    const uint16_t w = png->getWidth(), h = png->getHeight();
    const size_t cells = static_cast<size_t>(side) * side;
    IconDecode d{};
    d.png = png;
    d.src_w = w;
    d.src_h = h;
    d.side = side;
    bool ok = w > 0 && h > 0 && w <= 512 && h <= 512;
    if (ok) {
      d.row = static_cast<uint16_t *>(heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
      d.acc = static_cast<uint32_t *>(heap_caps_calloc(cells * 3, sizeof(uint32_t), MALLOC_CAP_SPIRAM));
      d.counts = static_cast<uint16_t *>(heap_caps_calloc(cells, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
      d.out = static_cast<uint16_t *>(heap_caps_malloc(cells * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
      ok = d.row && d.acc && d.counts && d.out && png->decode(&d, 0) == PNG_SUCCESS;  // state rides in pUser
    }
    png->close();
    if (ok) {
      for (size_t i = 0; i < cells; i++) {
        const uint32_t n = d.counts[i];
        if (n == 0) {
          // Upscaling leaves gaps: take the cell above or to the left.
          d.out[i] = i >= side ? d.out[i - side] : (i > 0 ? d.out[i - 1] : 0);
          continue;
        }
        const uint32_t *a = d.acc + i * 3;
        d.out[i] = static_cast<uint16_t>(((a[0] / n) << 11) | ((a[1] / n) << 5) | (a[2] / n));
      }
      result = std::shared_ptr<uint16_t>(d.out, [](uint16_t *p) { heap_caps_free(p); });
      d.out = nullptr;
    }
    heap_caps_free(d.row);
    heap_caps_free(d.acc);
    heap_caps_free(d.counts);
    heap_caps_free(d.out);
  }
  png->~PNG();
  heap_caps_free(memory);
  return result;
}

// ── Listings ────────────────────────────────────────────────────────────────

void PaxLoader::start_info_fetch_() {
  this->catalog_generation_++;
  this->app_info_.assign(this->catalog_entries_.size(), AppInfo{});
  if (!this->store_ui_ || !this->store_info_enabled_ || this->info_loading_ || this->catalog_entries_.empty())
    return;  // a running fetch notices the new generation and starts again
  if (this->launched_) {
    // Nobody sees the store while an app runs, and nine TLS fetches then took
    // the internal DMA memory the app's SD reads and I2S needed (OpenLara's
    // intro video failed to load). loop() starts this when the app ends.
    this->info_deferred_ = true;
    return;
  }
  this->info_urls_.clear();
  for (const auto &entry : this->catalog_entries_)
    this->info_urls_.push_back(entry.second);
  this->info_generation_ = this->catalog_generation_;
  this->info_done_ = false;
  this->info_loading_ = true;
  if (xTaskCreatePinnedToCoreWithCaps(&PaxLoader::pax_info_task_entry_, "pax_info", 16384, this, 3, &this->info_task_handle_,
                                      0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->info_loading_ = false;
    this->info_task_handle_ = nullptr;
    ESP_LOGW(TAG, "Could not start the app info task");
  }
}

void PaxLoader::pax_info_task_entry_(void *arg) {
  auto *self = static_cast<PaxLoader *>(arg);
  const std::vector<std::string> urls = self->info_urls_;
  std::vector<AppInfo> infos(urls.size());
  size_t found = 0;
  self->info_interrupted_ = false;
  for (size_t i = 0; i < urls.size(); i++) {
    if (self->launched_) {
      self->info_interrupted_ = true;  // an app started: stop between fetches
      break;
    }
    if (!self->store_info_enabled_)
      break;
    const std::string sidecar = sidecar_for(urls[i]);
    if (sidecar.empty())
      continue;
    std::string text;
    const esp_err_t err = is_network_url(sidecar.c_str()) ? fetch_http_text(sidecar.c_str(), &text, INFO_MAX_BYTES)
                                                          : read_small_file(sidecar, &text, INFO_MAX_BYTES);
    if (err == ESP_OK && parse_app_info(text, &infos[i], self->store_thumbnails_enabled_)) {
      // Decoded here, not on the LVGL loop: a folder of 30+ apps took seconds there.
      if (self->store_thumbnails_enabled_)
        infos[i].tile_icon = decode_png_icon(infos[i].icon_png, TILE_ICON_STORAGE);
      else {
        infos[i].icon_png.clear();
        infos[i].icon_png.shrink_to_fit();
      }
      // The tile pixels are the persistent copy used by the grid. Keep a
      // small source PNG for the detail page when possible, but never retain
      // a large original image for every catalog entry.
      if (infos[i].icon_png.size() > DETAIL_ICON_PNG_MAX_BYTES) {
        infos[i].icon_png.clear();
        infos[i].icon_png.shrink_to_fit();
      }
      // The raw sidecar is deliberately released after parsing. The icon and
      // structured metadata above are all the catalog needs; retaining the
      // JSON for every app starves the next JsonDocument on larger catalogs.
      infos[i].sidecar.clear();
      infos[i].sidecar.shrink_to_fit();
      found++;
    }
  }
  ESP_LOGI(TAG, "App info: %u of %u app(s) have a listing", static_cast<unsigned>(found),
           static_cast<unsigned>(urls.size()));
  if (!self->store_info_enabled_)
    infos.assign(urls.size(), AppInfo{});
  self->info_installed_ = scan_installed_(self->install_dir_);
  self->info_result_ = std::move(infos);
  self->info_done_ = true;
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(100));  // the loop deletes this task
}

void PaxLoader::set_store_thumbnails_enabled(bool enabled) {
  if (this->store_thumbnails_enabled_ == enabled)
    return;
  this->store_thumbnails_enabled_ = enabled;
  if (!enabled) {
    for (auto &info : this->app_info_) {
      info.tile_icon.reset();
      info.icon_png.clear();
      info.icon_png.shrink_to_fit();
    }
  }
#ifdef PAX_LOADER_USE_LVGL
  this->free_icons_();
  this->catalog_ui_pending_ = true;
#endif
  ESP_LOGI(TAG, "Store thumbnails: %s", enabled ? "on" : "off");
}

void PaxLoader::set_store_info_enabled(bool enabled) {
  if (this->store_info_enabled_ == enabled)
    return;
  this->store_info_enabled_ = enabled;
  if (!enabled) {
    // Drop the parsed sidecars and icon buffers immediately. The worker checks
    // this flag between files and publishes an empty result when it stops.
    this->app_info_.assign(this->catalog_entries_.size(), AppInfo{});
    this->info_deferred_ = false;
#ifdef PAX_LOADER_USE_LVGL
    this->catalog_ui_pending_ = true;
#endif
  } else if (!this->info_loading_ && !this->catalog_entries_.empty()) {
    this->start_info_fetch_();
  }
  ESP_LOGI(TAG, "Store JSON metadata: %s", enabled ? "on" : "off");
}

void PaxLoader::set_store_low_memory_mode(bool enabled) {
  this->set_store_thumbnails_enabled(!enabled);
  this->set_store_info_enabled(!enabled);
  ESP_LOGI(TAG, "Store low-memory mode: %s", enabled ? "on" : "off");
}

void PaxLoader::poll_info_fetch_() {
  if (!this->info_loading_ || !this->info_done_)
    return;
  if (this->info_task_handle_ != nullptr) {
    vTaskDelete(this->info_task_handle_);
    this->info_task_handle_ = nullptr;
  }
  this->info_loading_ = false;
  if (this->info_interrupted_) {
    // Stopped early because an app started: fetch them all once it ends.
    this->info_interrupted_ = false;
    this->info_deferred_ = true;
    return;
  }
  if (this->info_generation_ != this->catalog_generation_) {
    // The catalog changed while this ran: fetch the current one.
    this->catalog_generation_--;
    this->start_info_fetch_();
    return;
  }
  this->app_info_ = std::move(this->info_result_);
  this->installed_ = std::move(this->info_installed_);
#ifdef PAX_LOADER_USE_LVGL
  this->catalog_ui_pending_ = true;
#endif
}

// name -> version of every app with a listing in install_dir. Reads the card,
// so it runs in the info and install tasks.
std::vector<std::pair<std::string, std::string>> PaxLoader::scan_installed_(const std::string &install_dir) {
  std::vector<std::pair<std::string, std::string>> installed;
  DIR *folder = opendir(runtime_path(install_dir.c_str()).c_str());
  if (folder == nullptr)
    return installed;
  while (dirent *entry = readdir(folder)) {
    const std::string file = entry->d_name;
    if (file.size() < 6 || file.compare(file.size() - 5, 5, ".json") != 0 || file == "settings.json" ||
        is_manifest_name(file))
      continue;
    std::string text;
    AppInfo info;
    if (read_small_file(install_dir + "/" + file, &text, INFO_MAX_BYTES) == ESP_OK &&
        parse_app_info(text, &info, false) && !info.name.empty())
      installed.emplace_back(info.name, info.version);
  }
  closedir(folder);
  return installed;
}

std::string PaxLoader::installed_version_(const std::string &name) const {
  for (const auto &item : this->installed_) {
    if (item.first == name)
      return item.second;
  }
  return {};
}

// ── Per-app settings ────────────────────────────────────────────────────────
// <install_dir>/settings.json maps app names to their settings:
//   {"psram_tulip": {"canvas": "1024x600"}}
// Other apps and other fields are kept when it is rewritten. Read when an app
// starts and by the store's detail page, both on the main loop.

static constexpr size_t SETTINGS_MAX_BYTES = 16 * 1024;

std::string PaxLoader::settings_path_() const { return this->install_dir_ + "/settings.json"; }

// Archive.org preferences share the installer's settings file, but the
// selected storage root may be /sd or /usb0. Prefer the root that already
// contains the setting so unplugging an SD card does not silently move the
// user's collection to another device. If that root is unavailable, use the
// first currently mounted data root and let write_text_file() create folders.
std::string PaxLoader::archive_settings_path_() const {
  auto root_is_available = [](const std::string &root) {
    struct stat info{};
    return stat(runtime_path(root.c_str()).c_str(), &info) == 0 && S_ISDIR(info.st_mode);
  };
  if (!this->archive_settings_location_.empty()) {
    const size_t slash = this->archive_settings_location_.find('/', 1);
    if (slash != std::string::npos && root_is_available(this->archive_settings_location_.substr(0, slash)))
      return this->archive_settings_location_;
  }
  for (const auto &root : this->data_roots_()) {
    if (root_is_available(root))
      return root + "/roms/pax/settings.json";
  }
  return this->settings_path_();
}

void PaxLoader::load_archive_collection_() {
  this->archive_collection_base_.clear();
  this->archive_collection_name_.clear();
  this->archive_settings_location_.clear();
  for (const auto &root : this->data_roots_()) {
    const std::string path = root + "/roms/pax/settings.json";
    std::string text;
    if (read_small_file(path, &text, SETTINGS_MAX_BYTES) != ESP_OK)
      continue;
    std::string base;
    std::string name;
    json::parse_json(text, [&base, &name](JsonObject document) -> bool {
      JsonObject archive = document["__archive"].as<JsonObject>();
      if (archive.isNull())
        return true;
      base = archive["collection_url"] | "";
      name = archive["collection_name"] | "";
      return true;
    });
    if (base.empty())
      continue;
    if (base.back() != '/')
      base.push_back('/');
    this->archive_collection_base_ = base;
    this->archive_collection_name_ = name.empty() ? base : name;
    this->archive_settings_location_ = path;
    ESP_LOGI(TAG, "Archive.org default collection: %s (settings: %s)",
             this->archive_collection_base_.c_str(), path.c_str());
    break;
  }
  if (this->archive_settings_location_.empty())
    this->archive_settings_location_ = this->archive_settings_path_();
#ifdef PAX_LOADER_USE_LVGL
  this->update_archive_settings_ui_();
#endif
}

bool PaxLoader::save_archive_collection_(const std::string &base_url, const std::string &name) {
  if (base_url.empty())
    return false;
  const std::string path = this->archive_settings_path_();
  std::string text;
  JsonDocument document;
  if (read_small_file(path, &text, SETTINGS_MAX_BYTES) == ESP_OK && !deserializeJson(document, text) &&
      document.is<JsonObject>()) {
    // Preserve app canvas/favorite settings already stored in this file.
  } else {
    document.clear();
    document.to<JsonObject>();
  }
  JsonObject root = document.as<JsonObject>();
  JsonObject archive = root["__archive"].as<JsonObject>();
  if (archive.isNull())
    archive = root["__archive"].to<JsonObject>();
  archive["collection_url"] = base_url;
  archive["collection_name"] = name;
  std::string output;
  serializeJsonPretty(document, output);
  output.push_back('\n');
  if (!write_text_file(path, output)) {
    ESP_LOGW(TAG, "Could not save Archive.org settings to %s", path.c_str());
    return false;
  }
  this->archive_collection_base_ = base_url;
  this->archive_collection_name_ = name;
  this->archive_settings_location_ = path;
  ESP_LOGI(TAG, "Saved Archive.org default collection %s to %s", name.c_str(), path.c_str());
#ifdef PAX_LOADER_USE_LVGL
  this->update_archive_settings_ui_();
#endif
  return true;
}

std::string PaxLoader::get_app_canvas(const std::string &app) {
  std::string text;
  if (app.empty() || read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK)
    return {};
  std::string value;
  json::parse_json(text, [&app, &value](JsonObject root) -> bool {
    value = root[app]["canvas"] | "";
    return true;
  });
  int w = 0, h = 0;
  return canvas::parse_size(value, &w, &h) ? canvas::format_size(w, h) : std::string();
}

bool PaxLoader::set_app_canvas(const std::string &app, const std::string &size) {
  int w = 0, h = 0;
  if (app.empty() || app.size() > 96)
    return false;
  if (!size.empty() &&
      (!canvas::parse_size(size, &w, &h) || !canvas::size_ok(w, h, this->max_canvas_w_, this->max_canvas_h_))) {
    ESP_LOGW(TAG, "Screen setting %s for %s refused: even sizes from %dx%d to %dx%d", size.c_str(), app.c_str(),
             canvas::MIN_WIDTH, canvas::MIN_HEIGHT, this->max_canvas_w_, this->max_canvas_h_);
    return false;
  }
  std::string text;
  JsonDocument doc;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK || deserializeJson(doc, text) ||
      !doc.is<JsonObject>())
    doc.to<JsonObject>();  // missing or unreadable: start a new one
  JsonObject root = doc.as<JsonObject>();
  JsonObject entry = root[app].as<JsonObject>();
  if (size.empty()) {
    if (!entry.isNull()) {
      entry.remove("canvas");
      if (entry.size() == 0)
        root.remove(app);
    }
  } else {
    if (entry.isNull())
      entry = root[app].to<JsonObject>();
    entry["canvas"] = canvas::format_size(w, h);
  }
  std::string out;
  serializeJsonPretty(doc, out);
  out.push_back('\n');

  bool ok = write_text_file(this->settings_path_(), out);
  if (ok) {
    ESP_LOGI(TAG, "Screen setting for %s: %s", app.c_str(), size.empty() ? "default" : size.c_str());
  } else {
    ESP_LOGW(TAG, "Could not write %s", this->settings_path_().c_str());
  }
  return ok;
}

bool PaxLoader::has_favorites() {
  std::string text;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK)
    return false;
  bool found = false;
  json::parse_json(text, [&found](JsonObject root) -> bool {
    for (JsonPair pair : root) {
      if (pair.value()["favorite"] | false) {
        found = true;
        break;
      }
    }
    return true;
  });
  return found;
}

#ifdef PAX_LOADER_USE_LVGL
bool PaxLoader::is_app_favorite_(const std::string &app) const {
  if (app.empty())
    return false;
  std::string text;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK)
    return false;
  bool favorite = false;
  json::parse_json(text, [&app, &favorite](JsonObject root) -> bool {
    favorite = root[app]["favorite"] | false;
    return true;
  });
  return favorite;
}

bool PaxLoader::set_app_favorite_(const std::string &app, bool favorite) {
  if (app.empty() || app.size() > 96)
    return false;
  std::string text;
  JsonDocument doc;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK || deserializeJson(doc, text) ||
      !doc.is<JsonObject>())
    doc.to<JsonObject>();
  JsonObject root = doc.as<JsonObject>();
  JsonObject entry = root[app].as<JsonObject>();
  if (favorite) {
    if (entry.isNull())
      entry = root[app].to<JsonObject>();
    entry["favorite"] = true;
  } else if (!entry.isNull()) {
    entry.remove("favorite");
    if (entry.size() == 0)
      root.remove(app);
  }
  std::string out;
  serializeJsonPretty(doc, out);
  out.push_back('\n');

  bool ok = write_text_file(this->settings_path_(), out);
  if (!ok)
    ESP_LOGW(TAG, "Could not write favorites to %s", this->settings_path_().c_str());
  return ok;
}

void PaxLoader::toggle_app_favorite_(int index) {
  const std::string app = this->detail_app_key_(index);
  if (app.empty())
    return;
  const bool favorite = !this->is_app_favorite_(app);
  if (!this->set_app_favorite_(app, favorite)) {
    this->set_progress_(false, 0, 0, "%s", "Could not save Favorites - is the card in?");
    return;
  }
  ESP_LOGI(TAG, "%s favorite: %s", favorite ? "Added" : "Removed", app.c_str());
  this->close_detail_();
  this->catalog_ui_pending_ = true;
}

// ROM favorites share settings.json with app favorites, but live under a
// reserved object so an app name can never collide with a game path.
bool PaxLoader::is_rom_favorite_(const std::string &path) const {
  if (path.empty())
    return false;
  std::string text;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK)
    return false;
  bool favorite = false;
  const std::string app = this->rom_selector_app_;
  json::parse_json(text, [&app, &path, &favorite](JsonObject root) -> bool {
    JsonArray list = root["__rom_favorites"][app].as<JsonArray>();
    for (JsonVariant item : list) {
      if ((item | "") == path) {
        favorite = true;
        break;
      }
    }
    return true;
  });
  return favorite;
}

bool PaxLoader::set_rom_favorite_(const std::string &path, bool favorite) {
  if (path.empty() || path.size() > 240 || this->rom_selector_app_.empty())
    return false;
  std::string text;
  JsonDocument doc;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK || deserializeJson(doc, text) ||
      !doc.is<JsonObject>())
    doc.to<JsonObject>();
  JsonObject root = doc.as<JsonObject>();
  JsonObject all = root["__rom_favorites"].as<JsonObject>();
  if (all.isNull())
    all = root["__rom_favorites"].to<JsonObject>();
  JsonArray list = all[this->rom_selector_app_].as<JsonArray>();
  if (favorite) {
    bool present = false;
    for (JsonVariant item : list) {
      if ((item | "") == path) {
        present = true;
        break;
      }
    }
    if (!present) {
      if (list.isNull())
        list = all[this->rom_selector_app_].to<JsonArray>();
      list.add(path);
    }
  } else if (!list.isNull()) {
    for (size_t i = list.size(); i > 0; i--) {
      if ((list[i - 1] | "") == path)
        list.remove(i - 1);
    }
    if (list.size() == 0)
      all.remove(this->rom_selector_app_);
  }
  if (all.size() == 0)
    root.remove("__rom_favorites");

  std::string out;
  serializeJsonPretty(doc, out);
  out.push_back('\n');
  bool ok = write_text_file(this->settings_path_(), out);
  if (!ok)
    ESP_LOGW(TAG, "Could not write ROM favorites to %s", this->settings_path_().c_str());
  return ok;
}

void PaxLoader::toggle_rom_favorites() {
  this->play_ui_sound(UI_SOUND_CONFIRM);
  this->rom_selector_favorites_only_ = !this->rom_selector_favorites_only_;
  this->update_rom_selector_ui_();
}

void PaxLoader::toggle_selected_rom_favorite() {
  if (this->rom_selector_selection_ >= this->rom_selector_paths_.size())
    return;
  const std::string path = this->rom_selector_paths_[this->rom_selector_selection_];
  const bool favorite = !this->is_rom_favorite_(path);
  if (!this->set_rom_favorite_(path, favorite)) {
    this->set_progress_(false, 0, 0, "%s", "Could not save game favorite - is the card in?");
    return;
  }
  this->play_ui_sound(UI_SOUND_CONFIRM);
  ESP_LOGI(TAG, "%s ROM favorite: %s", favorite ? "Added" : "Removed", path.c_str());
  this->update_rom_selector_ui_();
}
#endif

// The size an app's listing recommends, for display_get_size when it has no
// Screen setting. Runs on the main loop as the app starts.
std::string PaxLoader::listing_recommended_canvas_(const std::string &source) const {
  const std::string key = canvas::app_key(source);
  const AppInfo *found = nullptr;
  for (size_t i = 0; i < this->app_info_.size() && i < this->catalog_entries_.size() && found == nullptr; i++) {
    const AppInfo &info = this->app_info_[i];
    if (info.has_info && (this->catalog_entries_[i].second == source || (!info.name.empty() && info.name == key)))
      found = &info;
  }
  // Do not parse an arbitrary sidecar synchronously while starting an app.
  // The pasted crash was a Load access fault in ArduinoJson's string lookup
  // from this fallback path. Listings are already parsed by the background
  // info task; if this app was not in that snapshot, use the device default
  // canvas and let the next catalog refresh populate the recommendation.
  if (found == nullptr && !is_network_url(source.c_str())) {
    ESP_LOGW(TAG, "No cached listing for %s; using the default canvas", source.c_str());
    return {};
  }
  if (found == nullptr || !found->supports_canvas())
    return {};
  return canvas::recommended(this->max_canvas_w_, this->max_canvas_h_,
                             found->canvas_any ? std::vector<std::string>() : found->canvas_sizes,
                             found->canvas_recommended);
}

// ── Install manifest ────────────────────────────────────────────────────────
// <install_dir>/<app>.installed.json lists the data files the store
// downloaded for an app, so Uninstall can offer to delete exactly those:
//   {"app": "psram_doom", "files": [{"path": "/sd/roms/doom/doom1.wad",
//                                    "size": 4196020, "sha256": "1d7d..."}]}
// No "name" field: older loaders would take it for an installed app's listing.

static constexpr size_t MANIFEST_MAX_BYTES = 16 * 1024;

// Writes a small file at a /sd/... or /usb0/... path, creating its folders.
static bool write_text_file(const std::string &path, const std::string &text) {
  const size_t cut = path.find('/', 1);
  if (cut == std::string::npos)
    return false;
  const std::string storage = runtime_path(path.substr(0, cut).c_str());
  const std::string target = path.substr(cut + 1);
  const std::string local_path = storage + "/" + target;
  const std::string temp = local_path + ".tmp";
  const std::string backup = local_path + ".bak";
  if (!make_parent_dirs(storage, target))
    return false;

  struct stat local_info{}, backup_info{};
  const bool local_exists = stat(local_path.c_str(), &local_info) == 0;
  const bool backup_exists = stat(backup.c_str(), &backup_info) == 0;
  if (!local_exists && backup_exists) {
    if (std::rename(backup.c_str(), local_path.c_str()) == 0)
      ESP_LOGW(TAG, "Recovered %s from interrupted JSON replacement", path.c_str());
  } else if (local_exists && backup_exists) {
    std::remove(backup.c_str());
  }

  // Keep the previous JSON until the replacement is completely written and
  // synced. This prevents a reset from turning settings/manifest files into
  // zero-byte or truncated files.
  std::remove(temp.c_str());
  FILE *file = std::fopen(temp.c_str(), "wb");
  bool ok = file != nullptr;
  if (ok) {
    ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (ok)
      ok = sync_store_file_(file, temp.c_str());
    ok = std::fclose(file) == 0 && ok;
  }
  if (!ok) {
    std::remove(temp.c_str());
    return false;
  }

  struct stat existing{};
  const bool had_existing = stat(local_path.c_str(), &existing) == 0;
  if (had_existing) {
    std::remove(backup.c_str());
    if (std::rename(local_path.c_str(), backup.c_str()) != 0) {
      std::remove(temp.c_str());
      return false;
    }
  }
  if (std::rename(temp.c_str(), local_path.c_str()) != 0) {
    if (had_existing)
      std::rename(backup.c_str(), local_path.c_str());
    std::remove(temp.c_str());
    return false;
  }
  if (!sync_store_path_(local_path))
    ESP_LOGW(TAG, "Committed %s, but the final durability barrier failed", path.c_str());
  if (had_existing && std::remove(backup.c_str()) != 0 && errno != ENOENT)
    ESP_LOGW(TAG, "Could not remove old backup %s (errno %d)", backup.c_str(), errno);
  return true;
}

std::string PaxLoader::manifest_path_(const std::string &app) const {
  return this->install_dir_ + "/" + app + MANIFEST_SUFFIX;
}

// Called from the install and load tasks after each verified download.
void PaxLoader::record_download_(const std::string &app, const std::string &path, uint32_t size,
                                  const std::string &sha256) {
  if (!data::safe_target(app) || app.find('/') != std::string::npos)
    return;
  const std::string manifest = this->manifest_path_(app);
  std::string text;
  JsonDocument doc;
  if (read_small_file(manifest, &text, MANIFEST_MAX_BYTES) != ESP_OK || deserializeJson(doc, text) ||
      !doc.is<JsonObject>())
    doc.to<JsonObject>();
  JsonObject root = doc.as<JsonObject>();
  root["app"] = app;
  JsonArray list = root["files"].is<JsonArray>() ? root["files"].as<JsonArray>() : root["files"].to<JsonArray>();
  for (size_t i = 0; i < list.size();) {
    if (path == (list[i]["path"] | ""))
      list.remove(i);  // downloaded again: the new entry replaces it
    else
      i++;
  }
  JsonObject entry = list.add<JsonObject>();
  entry["path"] = path;
  entry["size"] = size;
  entry["sha256"] = sha256;
  std::string out;
  serializeJsonPretty(doc, out);
  out.push_back('\n');
  if (!write_text_file(manifest, out))
    ESP_LOGW(TAG, "Could not write %s; Uninstall will not know about %s", manifest.c_str(), path.c_str());
}

std::vector<std::pair<std::string, uint32_t>> PaxLoader::downloaded_files_(const std::string &app) const {
  std::vector<std::pair<std::string, uint32_t>> result;
  std::string text;
  if (!data::safe_target(app) || read_small_file(this->manifest_path_(app), &text, MANIFEST_MAX_BYTES) != ESP_OK)
    return result;
  std::vector<std::string> roots = this->data_roots_();
  roots.emplace_back("/sd");
  json::parse_json(text, [&result, &roots](JsonObject root) -> bool {
    JsonArray list = root["files"].as<JsonArray>();
    for (JsonVariant entry : list) {
      const std::string path = entry["path"] | "";
      const uint32_t size = entry["size"] | 0u;
      std::string clean, storage;
      struct stat st{};
      // Only files still exactly as the store left them: a file the user
      // replaced since (a full game over the shareware one) is theirs.
      if (!files::clean_app_path(path.c_str(), roots, &clean, &storage) || clean == storage ||
          stat(runtime_path(clean.c_str()).c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
          static_cast<uint64_t>(st.st_size) != size)
        continue;
      result.emplace_back(clean, size);
    }
    return true;
  });
  return result;
}

// ── Required files ──────────────────────────────────────────────────────────

// Where a listing's "requires" path is: the path under the first data root
// that has it (data_root, then data_search), "" when none does. A stat per
// root, or one folder listing for a pattern; the detail page calls it.
std::string PaxLoader::find_required_(const std::string &path) const {
  files::Required req;
  if (!files::parse_required(path, &req))
    return {};
  for (const auto &root : this->data_roots_()) {
    const std::string base = runtime_path(root.c_str());
    struct stat st{};
    if (!req.is_glob) {
      if (stat((base + "/" + req.relative()).c_str(), &st) == 0 &&
          (req.is_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode)))
        return root + "/" + req.relative();
      continue;
    }
    const std::string folder = req.folder.empty() ? base : base + "/" + req.folder;
    DIR *dir = opendir(folder.c_str());
    if (dir == nullptr)
      continue;
    std::string match;
    while (dirent *entry = readdir(dir)) {
      if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0 ||
          !files::glob_match(req.name.c_str(), entry->d_name))
        continue;
      bool is_dir = entry->d_type == DT_DIR;
      if (entry->d_type == DT_UNKNOWN)
        is_dir = stat((folder + "/" + entry->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      if (is_dir == req.is_dir) {
        match = entry->d_name;
        break;
      }
    }
    closedir(dir);
    if (!match.empty())
      return root + "/" + (req.folder.empty() ? std::string() : req.folder + "/") + match;
  }
  return {};
}

// ── Uninstall ───────────────────────────────────────────────────────────────

bool PaxLoader::uninstall_app(const std::string &app, bool delete_data) {
  if (!data::safe_target(app) || app.find('/') != std::string::npos) {
    ESP_LOGW(TAG, "Uninstall refused: bad app name %s", app.c_str());
    return false;
  }
  if (this->launched_ || this->pax_loading_) {
    // The store is hidden while an app runs, but a lambda could still ask.
    this->set_progress_(false, 0, 0, "%s", "Close the running app before uninstalling");
    ESP_LOGW(TAG, "Uninstall of %s refused while an app is running (%s)", app.c_str(), this->running_source_.c_str());
    return false;
  }
  if (this->install_task_handle_ != nullptr || this->web_storage_busy_) {
    ESP_LOGW(TAG, "Uninstall of %s refused while an install runs", app.c_str());
    return false;
  }
  const std::vector<std::pair<std::string, uint32_t>> data =
      delete_data ? this->downloaded_files_(app) : std::vector<std::pair<std::string, uint32_t>>();

  const std::string pax = this->install_dir_ + "/" + app + ".pax";
  struct stat st{};
  if (stat(runtime_path(pax.c_str()).c_str(), &st) == 0 && unlink(runtime_path(pax.c_str()).c_str()) != 0) {
    ESP_LOGW(TAG, "Uninstall: could not remove %s (errno %d)", pax.c_str(), errno);
    this->set_progress_(false, 0, 0, "Could not remove %.60s", pax.c_str());
    return false;
  }
  // The listing next to it is what marks the app installed.
  unlink(runtime_path((this->install_dir_ + "/" + app + ".json").c_str()).c_str());
  ESP_LOGI(TAG, "Uninstalled %s: removed %s", app.c_str(), pax.c_str());

  unsigned deleted = 0;
  uint64_t bytes = 0;
  if (delete_data) {
    std::vector<std::string> roots = this->data_roots_();
    roots.emplace_back("/sd");
    for (const auto &file : data) {
      if (unlink(runtime_path(file.first.c_str()).c_str()) != 0) {
        ESP_LOGW(TAG, "Uninstall: could not delete %s (errno %d)", file.first.c_str(), errno);
        continue;
      }
      ESP_LOGI(TAG, "Uninstall: deleted %s (%u bytes)", file.first.c_str(), static_cast<unsigned>(file.second));
      deleted++;
      bytes += file.second;
      // Folders it leaves empty go too; the first one still in use stops it.
      std::string clean, storage;
      if (files::clean_app_path(file.first.c_str(), roots, &clean, &storage) && clean.size() > storage.size()) {
        for (const auto &folder : files::parent_folders(clean.substr(storage.size() + 1))) {
          if (!remove_empty_folder(runtime_path((storage + "/" + folder).c_str())))
            break;
        }
      }
    }
    // Everything it listed is gone or no longer the store's: forget it.
    unlink(runtime_path(this->manifest_path_(app).c_str()).c_str());
  }

  this->installed_.erase(std::remove_if(this->installed_.begin(), this->installed_.end(),
                                        [&app](const std::pair<std::string, std::string> &item) {
                                          return item.first == app;
                                        }),
                         this->installed_.end());
  if (deleted > 0) {
    this->set_progress_(false, 0, 0, "Uninstalled %.40s and deleted %u data file(s), %s", app.c_str(), deleted,
                        size_text(static_cast<uint32_t>(std::min<uint64_t>(bytes, UINT32_MAX))).c_str());
  } else {
    this->set_progress_(false, 0, 0, "Uninstalled %.60s", app.c_str());
  }
  if (!this->catalog_url_.empty() && this->catalog_url_[0] == '/') {
    this->refresh_catalog();  // a folder source may have listed the removed copy
  } else {
#ifdef PAX_LOADER_USE_LVGL
    this->catalog_ui_pending_ = true;  // badges again; an open detail page reopens with Install
#endif
  }
  return true;
}

// ── Install ─────────────────────────────────────────────────────────────────

void PaxLoader::start_install_(int index) {
  if (this->install_task_handle_ != nullptr || this->web_storage_busy_ || this->launched_ || index < 0 ||
      static_cast<size_t>(index) >= this->app_info_.size())
    return;
  const AppInfo &info = this->app_info_[index];
  if (!info.has_info || info.name.empty() || info.size == 0 || info.sha256.size() != 64) {
    this->set_progress_(false, 0, 0, "%s", "This app has no install information");
    return;
  }
  this->install_index_ = index;
  this->direct_install_ = false;
  this->direct_file_upload_ = false;
  this->upload_path_.clear();
  this->install_info_ = info;
  this->install_url_ = this->catalog_entries_[index].second;
  this->install_done_ = false;
  if (xTaskCreatePinnedToCoreWithCaps(&PaxLoader::pax_install_task_entry_, "pax_install", 20480, this, 4,
                                      &this->install_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->install_task_handle_ = nullptr;
    this->set_progress_(false, 0, 0, "%s", "Could not start the install");
  }
}

int PaxLoader::start_install_from_pax_(const char *url, const char *name, const char *title,
                                         const char *version, uint32_t size, const char *sha256) {
  if (url == nullptr || name == nullptr || title == nullptr || version == nullptr || sha256 == nullptr)
    return -1;
  const size_t url_len = strnlen(url, 2049);
  const size_t name_len = strnlen(name, 65);
  const size_t title_len = strnlen(title, 97);
  const size_t version_len = strnlen(version, 33);
  if (url_len == 0 || url_len > 2048 || !is_network_url(url) ||
      name_len == 0 || name_len > 64 || !data::safe_target(std::string(name, name_len)) ||
      std::strchr(name, '/') != nullptr || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0 ||
      title_len == 0 || title_len > 96 || version_len == 0 || version_len > 32 || size == 0 ||
      size > 16U * 1024U * 1024U || !data::is_sha256_hex(sha256))
    return -1;
  if (!this->launched_ || this->pax_loading_ ||
      this->running_app_source_.find("launcher_ui.pax") == std::string::npos)
    return -1;
  if (this->install_task_handle_ != nullptr || this->web_storage_busy_)
    return -2;

  this->install_index_ = -1;
  this->direct_install_ = false;
  this->direct_file_upload_ = false;
  this->upload_path_.clear();
  this->install_info_ = AppInfo{};
  this->install_info_.name.assign(name, name_len);
  this->install_info_.title.assign(title, title_len);
  this->install_info_.version.assign(version, version_len);
  this->install_info_.size = size;
  this->install_info_.sha256 = sha256;
  this->install_url_.assign(url, url_len);
  this->install_done_ = false;
  this->install_result_ = ESP_OK;
  this->set_progress_(true, 0, size, "Preparing install: %.60s", this->install_info_.title.c_str());
  if (xTaskCreatePinnedToCoreWithCaps(&PaxLoader::pax_install_task_entry_, "launcher_install", 20480, this, 4,
                                      &this->install_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->install_task_handle_ = nullptr;
    this->set_progress_(false, 0, 0, "%s", "Could not start the install task");
    return -3;
  }
  ESP_LOGI(TAG, "launcher_ui install requested: %s %s from %s", name, version, url);
  return 0;
}

int PaxLoader::svc_store_install_start(const char *url, const char *name, const char *title,
                                        const char *version, uint32_t size, const char *sha256) {
  PaxLoader *self = active_;
  return self != nullptr ? self->start_install_from_pax_(url, name, title, version, size, sha256) : -1;
}

int PaxLoader::svc_store_install_status(pax_install_progress_t *out) {
  PaxLoader *self = active_;
  if (self == nullptr || out == nullptr)
    return -1;
  std::memset(out, 0, sizeof(*out));
  portENTER_CRITICAL(&self->progress_lock_);
  out->done = self->progress_done_;
  out->total = self->progress_total_;
  std::memcpy(out->status, self->progress_status_, sizeof(out->status));
  portEXIT_CRITICAL(&self->progress_lock_);
  out->status[sizeof(out->status) - 1] = '\0';
  out->result = static_cast<int32_t>(self->install_result_);
  if (self->install_done_)
    out->state = self->install_result_ == ESP_OK ? 2U : 3U;
  else if (self->install_task_handle_ != nullptr)
    out->state = 1U;
  return 0;
}

int PaxLoader::svc_archive_download_start(const char *url, const char *path) {
  PaxLoader *self = active_;
  if (self == nullptr || url == nullptr || path == nullptr)
    return -1;
  if (self->install_task_handle_ != nullptr)
    return -2;
  self->request_archive_download_url(url, path);
  return self->install_task_handle_ != nullptr ? 0 : -1;
}

static bool valid_archive_cookie_value_(const char *value) {
  if (value == nullptr || *value == '\0' || std::strlen(value) >= 512)
    return false;
  for (size_t i = 0; value[i] != '\0'; i++) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    // Cookie-octet: exclude controls, whitespace, quotes, comma, semicolon,
    // and backslash so a value cannot add another request header.
    if (c < 0x21 || c > 0x7e || c == '"' || c == ',' || c == ';' || c == '\\')
      return false;
  }
  return true;
}

int PaxLoader::svc_archive_auth_set(const char *logged_in_user, const char *logged_in_sig) {
  PaxLoader *self = active_;
  if (self == nullptr || logged_in_user == nullptr || logged_in_sig == nullptr)
    return -1;
  const bool clear = logged_in_user[0] == '\0' && logged_in_sig[0] == '\0';
  if (!clear && (!valid_archive_cookie_value_(logged_in_user) ||
                 !valid_archive_cookie_value_(logged_in_sig)))
    return -1;
  portENTER_CRITICAL(&self->archive_auth_lock_);
  std::memset(self->archive_auth_user_, 0, sizeof(self->archive_auth_user_));
  std::memset(self->archive_auth_sig_, 0, sizeof(self->archive_auth_sig_));
  if (!clear) {
    std::memcpy(self->archive_auth_user_, logged_in_user, std::strlen(logged_in_user));
    std::memcpy(self->archive_auth_sig_, logged_in_sig, std::strlen(logged_in_sig));
  }
  portEXIT_CRITICAL(&self->archive_auth_lock_);
  return 0;
}

int PaxLoader::svc_archive_auth_get(char *cookie_header, size_t capacity) {
  PaxLoader *self = active_;
  if (self == nullptr || cookie_header == nullptr || capacity == 0)
    return -1;
  cookie_header[0] = '\0';
  portENTER_CRITICAL(&self->archive_auth_lock_);
  if (self->archive_auth_user_[0] == '\0' || self->archive_auth_sig_[0] == '\0') {
    portEXIT_CRITICAL(&self->archive_auth_lock_);
    return 0;
  }
  const int written = std::snprintf(cookie_header, capacity, "logged-in-user=%s; logged-in-sig=%s",
                                    self->archive_auth_user_, self->archive_auth_sig_);
  portEXIT_CRITICAL(&self->archive_auth_lock_);
  if (written < 0 || static_cast<size_t>(written) >= capacity) {
    cookie_header[0] = '\0';
    return -1;
  }
  return 1;
}

void PaxLoader::request_install_url(const std::string &url, const std::string &name, uint32_t size,
                                     const std::string &sha256) {
  if (!is_network_url(url.c_str())) {
    ESP_LOGW(TAG, "Ignoring PAX install with a non-network URL: %s", url.c_str());
    return;
  }
  if (!data::safe_target(name) || name.find('/') != std::string::npos || name == "." || name == ".." || size == 0 ||
      size > 16 * 1024 * 1024 || sha256.size() != 64) {
    this->set_progress_(false, 0, 0, "%s", "Invalid PAX upload details");
    ESP_LOGW(TAG, "Ignoring invalid PAX install: name=%s size=%lu sha256=%u", name.c_str(),
             static_cast<unsigned long>(size), static_cast<unsigned>(sha256.size()));
    return;
  }
  for (const char c : sha256) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      this->set_progress_(false, 0, 0, "%s", "Invalid PAX SHA-256");
      return;
    }
  }
  if (this->install_task_handle_ != nullptr || this->web_storage_busy_ || this->launched_ || this->pax_loading_) {
    this->set_progress_(false, 0, 0, "%s", "Close the running app before saving a PAX");
    ESP_LOGW(TAG, "Ignoring PAX install while the loader is busy: %s", name.c_str());
    return;
  }

  AppInfo info;
  info.name = name;
  info.title = name;
  info.size = size;
  info.sha256 = sha256;
  // Give a command-line upload the same installed badge and basic detail
  // metadata as a catalog install. The name is restricted to safe filename
  // characters above, so it needs no additional JSON escaping here.
  info.sidecar = "{\"name\":\"" + name + "\",\"title\":\"" + name +
                 "\",\"version\":\"local\",\"size\":" + std::to_string(size) +
                 ",\"sha256\":\"" + sha256 + "\"}\n";
  info.has_info = true;
  this->install_index_ = -1;
  this->direct_install_ = true;
  this->direct_file_upload_ = false;
  this->upload_path_.clear();
  this->install_info_ = std::move(info);
  this->install_url_ = url;
  this->install_done_ = false;
  if (xTaskCreatePinnedToCoreWithCaps(&PaxLoader::pax_install_task_entry_, "pax_upload", 20480, this, 4,
                                      &this->install_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->install_task_handle_ = nullptr;
    this->direct_install_ = false;
    this->set_progress_(false, 0, 0, "%s", "Could not start the PAX upload");
    return;
  }
  ESP_LOGI(TAG, "PAX save requested: %s -> %s/%s.pax", url.c_str(), this->install_dir_.c_str(), name.c_str());
}

void PaxLoader::request_upload_url(const std::string &url, const std::string &path, uint32_t size,
                                    const std::string &sha256) {
  this->request_upload_url_(url, path, size, sha256, false);
}

void PaxLoader::request_archive_download_url(const std::string &url, const std::string &path) {
  this->request_upload_url_(url, path, 0, "", true);
}

void PaxLoader::request_upload_url_(const std::string &url, const std::string &path, uint32_t size,
                                     const std::string &sha256, bool allow_launcher) {
  const bool safe_path = path.rfind("/sd/", 0) == 0 && data::safe_target(path.substr(4));
  // Native Archive.org members do not publish a SHA-256 or a size in the
  // listing, so (size=0, sha256="") is also accepted. The downloader then
  // verifies the server's content length when it is available and always
  // stages the file through <path>.part.
  const bool unverified_size = size == 0 && sha256.empty();
  if (!is_network_url(url.c_str()) || !safe_path ||
      size > 1024U * 1024U * 1024U || (!unverified_size && !data::is_sha256_hex(sha256))) {
    this->set_progress_(false, 0, 0, "%s", "Invalid SD upload details");
    ESP_LOGW(TAG, "Ignoring invalid SD upload: path=%s size=%lu sha256=%u", path.c_str(),
             static_cast<unsigned long>(size), static_cast<unsigned>(sha256.size()));
    return;
  }
  if (path.size() >= sizeof(this->file_request_path_)) {
    this->set_progress_(false, 0, 0, "%s", "SD upload path is too long");
    ESP_LOGW(TAG, "Ignoring SD upload with a path that is too long: %s", path.c_str());
    return;
  }
  const bool data_installer_is_active = allow_launcher && this->launched_ && !this->pax_loading_ &&
      (this->running_app_source_.find("launcher_ui.pax") != std::string::npos ||
       this->running_app_source_.find("recipe.pax") != std::string::npos);
  if (this->install_task_handle_ != nullptr || this->web_storage_busy_ || (this->launched_ && !data_installer_is_active) || this->pax_loading_) {
    this->set_progress_(false, 0, 0, "%s", "Close the running app before uploading");
    ESP_LOGW(TAG, "Ignoring SD upload while the loader is busy: %s", path.c_str());
    return;
  }

  this->install_index_ = -1;
  this->direct_install_ = true;
  this->direct_file_upload_ = true;
  this->upload_path_ = path;
  this->install_info_ = AppInfo{};
  this->install_info_.name = path.substr(path.find_last_of('/') + 1);
  this->install_info_.title = this->install_info_.name;
  this->install_info_.size = size;
  this->install_info_.sha256 = sha256;
  this->install_url_ = url;
  this->install_done_ = false;
  this->set_progress_(true, 0, size, "%s", "Starting download");
  if (xTaskCreatePinnedToCoreWithCaps(&PaxLoader::pax_install_task_entry_, "pax_file_upload", 20480, this, 4,
                                      &this->install_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->install_task_handle_ = nullptr;
    this->direct_install_ = false;
    this->direct_file_upload_ = false;
    this->upload_path_.clear();
    this->set_progress_(false, 0, 0, "%s", "Could not start the SD upload");
    return;
  }
  ESP_LOGI(TAG, "SD upload requested: %s -> %s", url.c_str(), path.c_str());
}

void PaxLoader::pax_install_task_entry_(void *arg) {
  auto *self = static_cast<PaxLoader *>(arg);
  const AppInfo &info = self->install_info_;
  const std::string &url = self->install_url_;
  esp_err_t err = ESP_OK;
  std::string root;
  std::string target;
  if (self->direct_file_upload_) {
    // The request was validated as /sd/<safe relative path>; split it into
    // the mounted storage root and a relative target for the shared downloader.
    const size_t cut = self->upload_path_.find('/', 1);
    root = runtime_path(self->upload_path_.substr(0, cut).c_str());
    target = self->upload_path_.substr(cut + 1);
  } else {
    // install_dir is a storage root and a folder on it: /sd/roms/pax -> /sd + roms/pax.
    const std::string &dir = self->install_dir_;
    const size_t cut = dir.find('/', 1);
    root = runtime_path((cut == std::string::npos ? dir : dir.substr(0, cut)).c_str());
    const std::string folder = cut == std::string::npos ? std::string() : dir.substr(cut + 1) + "/";
    target = folder + info.name + ".pax";
  }
  const std::string path = root + "/" + target;
#ifdef PAX_LOADER_USE_SD_MMC
  // The store can remain visible after an eject or a card insertion after
  // boot, while /sdcard itself is no longer present. Retry the custom SD
  // component's mount before creating the install directory.
  if (self->sd_mmc_ != nullptr && root == "/sdcard") {
    struct stat mount_point{};
    if (stat(root.c_str(), &mount_point) != 0 || !S_ISDIR(mount_point.st_mode)) {
      ESP_LOGW(TAG, "SD card is not mounted before install; retrying mount");
      self->sd_mmc_->mount();
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
#endif
  if (!make_parent_dirs(root, target)) {
    ESP_LOGE(TAG, "Cannot prepare install path: root=%s target=%s path=%s errno=%d", root.c_str(), target.c_str(),
             path.c_str(), errno);
    self->set_progress_(false, 0, 0, "Cannot write to %.40s - is the card in?",
                        self->direct_file_upload_ ? self->upload_path_.c_str() : self->install_dir_.c_str());
    err = ESP_ERR_NOT_FOUND;
  }
  if (err == ESP_OK) {
    struct stat existing{};
    if (stat(path.c_str(), &existing) == 0) {
      if (!S_ISREG(existing.st_mode)) {
        self->set_progress_(false, 0, 0, "%s", "Install target is not a regular file");
        err = ESP_ERR_INVALID_ARG;
      }
    }
  }
  if (err == ESP_OK) {
    data::DataFile file;
    file.size = info.size;
    file.sha256 = info.sha256;
    file.target = target;
    file.url = url;
    err = self->download_data_file_(file, root + "/" + target, 0, info.size, 1, 1);
    if (err != ESP_OK)
      self->set_progress_(false, 0, 0, "%s failed: %s", self->direct_file_upload_ ? "Upload" : "Install",
                          esp_err_to_name(err));
  }
  if (self->direct_file_upload_) {
    if (err == ESP_OK) {
      self->set_progress_(false, 0, 0, "Uploaded %.60s", self->upload_path_.c_str());
    }
  }
  if (err == ESP_OK && is_network_url(url.c_str()) && !self->direct_install_)
    err = self->sync_app_data_(url, info.name);  // reports its own progress and errors
  if (err == ESP_OK && !self->direct_file_upload_) {
    // The listing next to the installed copy marks it installed (and at which version).
    const std::string listing = path.substr(0, path.size() - 4) + ".json";
    std::string listing_text = info.sidecar;
    // Preserve the original full listing with the installed PAX so a later
    // local-catalog refresh still has the artwork. Catalog entries normally
    // leave sidecar_text empty to keep the catalog memory-light, so for a
    // local folder source this reads the original JSON from the card; for a
    // network source it fetches the sibling JSON over the same connection.
    if (listing_text.find("\"icon\"") == std::string::npos) {
      const std::string source_sidecar = sidecar_for(url);
      if (!source_sidecar.empty()) {
        std::string full_listing;
        const esp_err_t listing_err =
            is_network_url(source_sidecar.c_str())
                ? fetch_http_text(source_sidecar.c_str(), &full_listing, INFO_MAX_BYTES)
                : read_small_file(source_sidecar, &full_listing, INFO_MAX_BYTES);
        if (listing_err == ESP_OK && !full_listing.empty())
          listing_text = std::move(full_listing);
        else
          ESP_LOGW(TAG, "Could not preserve full listing for %s (%s)", info.name.c_str(),
                   esp_err_to_name(listing_err));
      }
    }
    if (!listing_text.empty()) {
      if (!write_text_file(listing, listing_text))
        ESP_LOGW(TAG, "Could not safely write installed listing %s", listing.c_str());
    }
    self->set_progress_(false, 0, 0, "%s %.60s", self->direct_install_ ? "Saved" : "Installed",
                        info.title.empty() ? info.name.c_str() : info.title.c_str());
  }
  self->install_installed_ = scan_installed_(self->install_dir_);
  self->install_result_ = err;
  self->install_done_ = true;
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(100));  // the loop deletes this task
}

void PaxLoader::poll_install_() {
  if (this->install_task_handle_ == nullptr || !this->install_done_)
    return;
  vTaskDelete(this->install_task_handle_);
  this->install_task_handle_ = nullptr;
  this->direct_file_upload_ = false;
  this->upload_path_.clear();
  ESP_LOGI(TAG, "Install finished: %s", esp_err_to_name(this->install_result_));
  this->installed_ = std::move(this->install_installed_);
#ifdef PAX_LOADER_USE_LVGL
  // Rebuilt with the new badges; an open detail page reopens with Launch.
  this->catalog_ui_pending_ = true;
#endif
}

#ifdef PAX_LOADER_USE_LVGL

// ── Icons ───────────────────────────────────────────────────────────────────

// Shows decoded pixels through an image descriptor; the icon keeps them alive.
void PaxLoader::set_icon_(AppIcon *icon, std::shared_ptr<uint16_t> pixels, uint16_t side) {
  *icon = AppIcon{};
  icon->pixels = std::move(pixels);
  icon->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  icon->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  icon->dsc.header.w = side;
  icon->dsc.header.h = side;
  icon->dsc.header.stride = side * sizeof(uint16_t);
  icon->dsc.data_size = static_cast<uint32_t>(side) * side * sizeof(uint16_t);
  icon->dsc.data = reinterpret_cast<const uint8_t *>(icon->pixels.get());
}

void PaxLoader::release_icon_(AppIcon *icon) {
  // Raw RGB565 is drawn straight from these pixels (nothing decoded into
  // LVGL's image cache), so dropping the reference once no object shows them
  // is enough; the listing may still hold its own.
  *icon = AppIcon{};
}

void PaxLoader::free_icons_() {
  for (auto &icon : this->tile_icons_)
    release_icon_(&icon);
  this->tile_icons_.clear();
}

size_t PaxLoader::store_source_index_(int visible_index) const {
  if (visible_index < 0)
    return static_cast<size_t>(-1);
  const size_t visible = static_cast<size_t>(visible_index);
  if (visible >= this->visible_catalog_indices_.size())
    return static_cast<size_t>(-1);
  return this->visible_catalog_indices_[visible];
}

// ── Grid ────────────────────────────────────────────────────────────────────

struct TileContext {
  PaxLoader *loader;
  int index;
};

// Bigger and smaller type where the config has those Montserrat sizes;
// otherwise the default font.
static const lv_font_t *title_font() {
#if LV_FONT_MONTSERRAT_32
  return &lv_font_montserrat_32;
#elif LV_FONT_MONTSERRAT_28
  return &lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_26
  return &lv_font_montserrat_26;
#elif LV_FONT_MONTSERRAT_24
  return &lv_font_montserrat_24;
#else
  return nullptr;
#endif
}

static const lv_font_t *small_font() {
#if LV_FONT_MONTSERRAT_16
  return &lv_font_montserrat_16;
#elif LV_FONT_MONTSERRAT_14
  return &lv_font_montserrat_14;
#elif LV_FONT_MONTSERRAT_18
  return &lv_font_montserrat_18;
#else
  return nullptr;
#endif
}

static void set_font(lv_obj_t *obj, const lv_font_t *font) {
  if (font != nullptr)
    lv_obj_set_style_text_font(obj, font, 0);
}

static lv_obj_t *plain_box(lv_obj_t *parent) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  return box;
}

static lv_obj_t *text_label(lv_obj_t *parent, const std::string &text, uint32_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text.c_str());
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  return label;
}

// Keeps a label to one line ending in "...": LONG_DOT alone still wraps when
// the height follows the content.
static void one_line(lv_obj_t *label, int32_t width) {
  lv_obj_set_width(label, width);
  lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

// Store app names are often longer than a tile or detail heading. Keep the
// heading on one line and let LVGL continuously scroll it instead of cutting
// off the identifying part of the title.
static void scrolling_line(lv_obj_t *label, int32_t width) {
  lv_obj_set_width(label, width);
  lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
  lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

// A coloured square with the app's initials, for apps without an icon.
static void letter_tile(lv_obj_t *box, const std::string &title) {
  uint32_t hash = 2166136261u;
  for (char c : title)
    hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  static const uint32_t palette[] = {0x7C3AED, 0x2563EB, 0x0891B2, 0x059669, 0xD97706, 0xDC2626, 0xDB2777, 0x4F46E5};
  lv_obj_set_style_bg_color(box, lv_color_hex(palette[hash % 8]), 0);
  std::string initials;
  bool start = true;
  for (char c : title) {
    if (std::isalnum(static_cast<unsigned char>(c)) && start && initials.size() < 2) {
      initials += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      start = false;
    } else if (c == ' ' || c == '_' || c == '-') {
      start = true;
    }
  }
  lv_obj_t *label = text_label(box, initials.empty() ? "?" : initials, 0xFFFFFF);
  lv_obj_center(label);
}

static std::string display_title(const PaxLoader::AppInfo *info, const std::string &file_label) {
  if (info != nullptr && !info->title.empty())
    return info->title;
  std::string title = file_label;
  if (title.size() > 4 && title.compare(title.size() - 4, 4, ".pax") == 0)
    title = title.substr(0, title.size() - 4);
  return title;
}

void PaxLoader::store_tile_event_cb_(lv_event_t *event) {
  auto *context = static_cast<TileContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    // A tap moves the d-pad selection too, so only one tile is ever highlighted.
    context->loader->play_ui_sound(UI_SOUND_CONFIRM);
    context->loader->set_catalog_selection_(static_cast<uint16_t>(context->index));
    context->loader->open_detail_(context->index);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

void PaxLoader::build_store_grid_() {
  this->free_icons_();
  this->catalog_tiles_.clear();
  if (this->visible_catalog_indices_.empty())
    return;

  // The low-memory presentation deliberately avoids creating one image tile
  // per app. It is still part of the store UI, so the existing selection and
  // detail actions continue to work with a compact, scrollable file listing.
  if (!this->store_thumbnails_enabled_) {
    this->tile_icons_.clear();
    for (size_t i = 0; i < this->visible_catalog_indices_.size(); i++) {
      const size_t source_index = this->visible_catalog_indices_[i];
      const AppInfo *info = source_index < this->app_info_.size() && this->app_info_[source_index].has_info
                                ? &this->app_info_[source_index]
                                : nullptr;
      const std::string title = display_title(info, this->catalog_entries_[source_index].first);
      lv_obj_t *button = lv_list_add_btn(this->catalog_container_, nullptr, title.c_str());
      if (button == nullptr)
        continue;
      lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_TILE), 0);
      lv_obj_set_style_bg_color(button, lv_color_hex(0x172F4A), LV_STATE_FOCUSED);
      lv_obj_set_style_bg_color(button, lv_color_hex(0x214B69), LV_STATE_PRESSED);
      lv_obj_set_style_text_color(button, lv_color_hex(COLOR_TEXT), 0);
      lv_obj_set_style_border_width(button, 1, 0);
      lv_obj_set_style_border_color(button, lv_color_hex(COLOR_TILE_BORDER), 0);
      lv_obj_add_event_cb(button, store_tile_event_cb_, LV_EVENT_ALL,
                          new TileContext{this, static_cast<int>(i)});  // NOLINT
      add_to_default_group(button);
      this->catalog_tiles_.push_back(button);
    }
    return;
  }

  lv_obj_update_layout(this->catalog_container_);
  int32_t width = lv_obj_get_content_width(this->catalog_container_);
  if (width <= 0)
    width = 960;
  const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
  if (screen_w >= 1000 && width >= 860) {
    this->grid_columns_ = 5;
  } else {
    this->grid_columns_ = static_cast<uint16_t>(std::max<int32_t>(1, (width + TILE_GAP) / (TILE_W + TILE_GAP)));
  }
  const uint16_t cols = this->grid_columns_;
  const size_t count = this->visible_catalog_indices_.size();
  const int32_t rows = static_cast<int32_t>((count + cols - 1) / cols);
  const int32_t left = std::max<int32_t>(0, (width - (cols * TILE_W + (cols - 1) * TILE_GAP)) / 2);

  lv_obj_t *grid = plain_box(this->catalog_container_);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  // Leave a full gap below the final row. The tiles start half a gap down,
  // so without this extra space the last row extends past the grid's content
  // bounds and can be clipped while the list is being scrolled.
  lv_obj_set_size(grid, width, rows * (TILE_H + TILE_GAP) + TILE_GAP);

  this->tile_icons_.resize(count);
  for (size_t i = 0; i < count; i++) {
    const size_t source_index = this->visible_catalog_indices_[i];
    const AppInfo *info = source_index < this->app_info_.size() && this->app_info_[source_index].has_info
                              ? &this->app_info_[source_index]
                              : nullptr;
    const std::string title = display_title(info, this->catalog_entries_[source_index].first);
    lv_obj_t *tile = plain_box(grid);
    lv_obj_set_pos(tile, left + static_cast<int32_t>(i % cols) * (TILE_W + TILE_GAP),
                   static_cast<int32_t>(i / cols) * (TILE_H + TILE_GAP) + TILE_GAP / 2);
    lv_obj_set_size(tile, TILE_W, TILE_H);
    lv_obj_set_style_radius(tile, 18, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_80, 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(COLOR_TILE_BORDER), 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(COLOR_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(tile, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x172544), LV_STATE_PRESSED);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICK_FOCUSABLE);  // focus follows the selection only
    lv_obj_add_event_cb(tile, store_tile_event_cb_, LV_EVENT_ALL, new TileContext{this, static_cast<int>(i)});  // NOLINT
    add_to_default_group(tile);

    lv_obj_t *icon = plain_box(tile);
    lv_obj_set_size(icon, TILE_ICON, TILE_ICON);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(icon, 24, 0);
    lv_obj_set_style_clip_corner(icon, true, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    if (info != nullptr && info->tile_icon) {
      set_icon_(&this->tile_icons_[i], info->tile_icon, TILE_ICON_STORAGE);
      lv_obj_set_style_bg_image_src(icon, &this->tile_icons_[i].dsc, 0);
      lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    } else {
      letter_tile(icon, title);
    }

    lv_obj_t *name = text_label(tile, title, COLOR_TEXT);
    scrolling_line(name, TILE_W - 16);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, TILE_ICON + 18);

    std::string sub;
    if (info != nullptr && !info->version.empty())
      sub = "v" + info->version;
    if (info != nullptr && info->size > 0)
      sub += (sub.empty() ? "" : "  |  ") + size_text(info->size + info->data_size);
    if (!sub.empty()) {
      lv_obj_t *meta = text_label(tile, sub, COLOR_MUTED);
      set_font(meta, small_font());
      one_line(meta, TILE_W - 16);
      lv_obj_set_style_text_align(meta, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(meta, LV_ALIGN_TOP_MID, 0, TILE_ICON + 44);
    }

    // INSTALLED / UPDATE, from the listing next to an installed copy.
    const std::string have = info != nullptr ? this->installed_version_(info->name) : std::string();
    if (!have.empty()) {
      // A round mark in the tile's corner: a tick when installed, an arrow when
      // the store has a newer version.
      const bool update = !info->version.empty() && have != info->version;
      lv_obj_t *badge = plain_box(tile);
      lv_obj_set_size(badge, 30, 30);
      lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -6, 6);
      lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(badge, lv_color_hex(update ? COLOR_UPDATE : COLOR_INSTALLED), 0);
      lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
      lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_t *mark = text_label(badge, update ? LV_SYMBOL_UPLOAD : LV_SYMBOL_OK, 0x0B1020);
      set_font(mark, small_font());
      lv_obj_center(mark);
    }
    this->catalog_tiles_.push_back(tile);
  }
}

// ── Detail page ─────────────────────────────────────────────────────────────

struct DetailButtonContext {
  PaxLoader *loader;
  uint8_t action;
};

void PaxLoader::store_button_event_cb_(lv_event_t *event) {
  auto *context = static_cast<DetailButtonContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    if (context->action == ACTION_BACK) {
      context->loader->play_ui_sound(UI_SOUND_BACK);
    } else {
      context->loader->play_ui_sound(UI_SOUND_CONFIRM);
    }
    context->loader->run_detail_action_(context->action);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

void PaxLoader::open_detail_(int index) {
  const size_t source_index = this->store_ui_ ? this->store_source_index_(index) : static_cast<size_t>(index);
  if (source_index == static_cast<size_t>(-1) || source_index >= this->catalog_entries_.size())
    return;
  this->close_detail_();
  this->detail_index_ = static_cast<int>(source_index);
  this->detail_url_ = this->catalog_entries_[source_index].second;
  const AppInfo *info =
      source_index < this->app_info_.size() && this->app_info_[source_index].has_info ? &this->app_info_[source_index]
                                                                                        : nullptr;
  const std::string title = display_title(info, this->catalog_entries_[source_index].first);
  const std::string url = this->catalog_entries_[source_index].second;
  const bool remote = is_network_url(url.c_str());
  const std::string have = info != nullptr ? this->installed_version_(info->name) : std::string();

  const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
  const int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
  lv_obj_t *panel = plain_box(lv_layer_top());
  lv_obj_set_size(panel, screen_w, screen_h);
  lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_PAGE), 0);
  // Keep the library art as a subtle backdrop, not strong enough to compete
  // with the text. The old 80% fill let bright card labels bleed through the
  // detail copy and made colored headings look washed out.
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_TILE_BORDER), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);  // keeps touches off the page below
  this->detail_panel_ = panel;
  // The detail page is also on lv_layer_top(); hide the global volume control
  // so it cannot cover or intercept the detail-page action buttons.
  this->ensure_catalog_volume_control_();
  this->set_catalog_volume_control_visible_(false);

  const int32_t margin = 20;
  lv_obj_t *icon = plain_box(panel);
  lv_obj_set_size(icon, DETAIL_ICON, DETAIL_ICON);
  lv_obj_set_pos(icon, margin, margin);
  lv_obj_set_style_radius(icon, 24, 0);
  lv_obj_set_style_clip_corner(icon, true, 0);
  // One icon at the larger size is quick enough to decode here.
  std::shared_ptr<uint16_t> big = info != nullptr ? decode_png_icon(info->icon_png, DETAIL_ICON) : nullptr;
  if (big) {
    set_icon_(&this->detail_icon_, std::move(big), DETAIL_ICON);
    lv_obj_set_style_bg_image_src(icon, &this->detail_icon_.dsc, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
  } else {
    letter_tile(icon, title);
  }

  // Buttons under the icon.
  std::vector<std::pair<uint8_t, std::string>> buttons;
  if (!have.empty()) {
    buttons.emplace_back(ACTION_LAUNCH, LV_SYMBOL_PLAY "  Launch");
    if (remote && info != nullptr && !info->version.empty() && have != info->version)
      buttons.emplace_back(ACTION_INSTALL, LV_SYMBOL_REFRESH "  Update to\nv" + info->version);
  } else if (remote) {
    buttons.emplace_back(ACTION_STREAM, LV_SYMBOL_PLAY "  Stream");
    if (info != nullptr && info->size > 0 && info->sha256.size() == 64)
      buttons.emplace_back(ACTION_INSTALL, LV_SYMBOL_DOWNLOAD "  Install");
  } else {
    buttons.emplace_back(ACTION_LAUNCH, LV_SYMBOL_PLAY "  Launch");
  }
  // Cartridge-style emulators get a selector on their information page;
  // data-driven ports such as Doom and Red Alert still launch directly.
  if (this->supports_rom_selector(url))
    buttons.emplace_back(ACTION_ROM, LV_SYMBOL_LIST "  Select game");
  if (info != nullptr && info->supports_canvas())
    buttons.emplace_back(ACTION_SCREEN, this->screen_label_(static_cast<int>(source_index)));
  buttons.emplace_back(ACTION_FAVORITE,
                       this->is_app_favorite_(this->detail_app_key_(static_cast<int>(source_index)))
                           ? "*  Remove favorite"
                           : "*  Add to favorites");
  if (!have.empty())
    buttons.emplace_back(ACTION_UNINSTALL, LV_SYMBOL_TRASH "  Uninstall");
  buttons.emplace_back(ACTION_BACK, LV_SYMBOL_CLOSE "  Back");
  this->detail_buttons_.clear();
  this->detail_actions_.clear();
  int32_t y = margin + DETAIL_ICON + 16;
  for (size_t i = 0; i < buttons.size(); i++) {
    lv_obj_t *button = plain_box(panel);
    lv_obj_set_size(button, DETAIL_ICON, 46);
    lv_obj_set_pos(button, margin, y);
    // Compact rows leave the thumbnail column usable on 600px displays.
    y += 50;
    const bool primary = i == 0;
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? 0x0EA5E9 : 0x1E293B), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_90, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? 0x0284C7 : 0x334155), LV_STATE_PRESSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t *label = text_label(button, buttons[i].second, 0xFFFFFF);
    // Keep detail-button labels inside their fixed boxes. The update number
    // gets its own line; other labels scroll horizontally if a future label
    // is wider than the button.
    const int32_t label_width = DETAIL_ICON - 16;
    const int32_t line_height = lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));
    lv_obj_set_width(label, label_width);
    if (buttons[i].second.find('\n') != std::string::npos) {
      lv_obj_set_height(label, line_height * 2);
      lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    } else {
      lv_obj_set_height(label, line_height);
      lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, store_button_event_cb_, LV_EVENT_ALL, new DetailButtonContext{this, buttons[i].first});  // NOLINT
    add_to_default_group(button);
    this->detail_buttons_.push_back(button);
    this->detail_actions_.push_back(buttons[i].first);
  }

  // Text column.
  const int32_t text_x = margin + DETAIL_ICON + 32;
  const int32_t text_w = screen_w - text_x - margin;
  lv_obj_t *name = text_label(panel, title, 0xFFFFFF);
  lv_obj_set_style_text_color(name, lv_color_hex(COLOR_ACCENT), 0);
  set_font(name, title_font());
  lv_obj_set_pos(name, text_x, margin - 4);
  scrolling_line(name, text_w);

  // Keep only the app name fixed above the body. All metadata and details live
  // inside one full-height scroller; previously the metadata was stacked above
  // it, leaving a short scroll window and a large blank gap below the title.
  lv_obj_t *scroller = plain_box(panel);
  lv_obj_set_style_bg_color(scroller, lv_color_hex(0x0B1728), 0);
  lv_obj_set_style_bg_opa(scroller, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(scroller, lv_color_hex(COLOR_TILE_BORDER), 0);
  lv_obj_set_style_border_width(scroller, 1, 0);
  lv_obj_set_style_radius(scroller, 14, 0);
  const int32_t title_h = lv_font_get_line_height(lv_obj_get_style_text_font(name, LV_PART_MAIN));
  const int32_t body_y = margin + title_h + 14;
  lv_obj_set_pos(scroller, text_x, body_y);
  lv_obj_set_size(scroller, text_w, screen_h - body_y - margin);
  lv_obj_set_style_bg_color(scroller, lv_color_hex(COLOR_ACCENT), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(scroller, LV_OPA_80, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(scroller, 8, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(scroller, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
  lv_obj_set_scrollbar_mode(scroller, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_flag(scroller, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *below = nullptr;
  auto block = [&below, scroller, text_w](const std::string &text, uint32_t color, int32_t gap,
                                          bool heading = false) {
    lv_obj_t *label = text_label(scroller, text, color);
    lv_obj_set_width(label, text_w - 36);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    if (heading)
      set_font(label, small_font());
    if (below != nullptr) {
      lv_obj_align_to(label, below, LV_ALIGN_OUT_BOTTOM_LEFT, 0, gap);
    } else {
      lv_obj_set_pos(label, 14, 14);
    }
    below = label;
  };
  auto section = [&block](const char *heading, const std::string &text) {
    block(heading, COLOR_ACCENT, 18, true);
    block(text, COLOR_TEXT, 4);
  };
  if (info != nullptr) {
    std::string metadata;
    auto add_metadata = [&metadata](const std::string &part) {
      if (!part.empty())
        metadata += (metadata.empty() ? "" : "   ·   ") + part;
    };
    if (!info->version.empty())
      add_metadata("v" + info->version);
    add_metadata(info->category);
    add_metadata(info->license);
    if (!metadata.empty())
      block(metadata, COLOR_MUTED, 0);
    if (!info->author.empty())
      block("by " + info->author, COLOR_MUTED, 4);
    if (!have.empty()) {
      const bool update = !info->version.empty() && have != info->version;
      block(update ? "UPDATE AVAILABLE  ·  installed v" + have : LV_SYMBOL_OK "  INSTALLED",
            update ? COLOR_UPDATE : COLOR_INSTALLED, 4);
    }
    if (info->size > 0) {
      std::string sizes = "PAX  " + size_text(info->size);
      if (info->data_size > 0)
        sizes += "    +    data  " + size_text(info->data_size) + "    =    " +
                 size_text(info->size + info->data_size);
      block(sizes, COLOR_ACCENT, 4);
    }
  } else {
    block(remote ? "Network app" : "Local app", COLOR_MUTED, 0);
  }

  if (info != nullptr && !info->required_files.empty()) {
    block("FILES ON THE CARD", COLOR_ACCENT, 16, true);
    for (const auto &file : info->required_files) {
      const std::string found = this->find_required_(file.path);
      const uint32_t color = !found.empty() ? COLOR_INSTALLED
                             : file.optional || file.download ? COLOR_UPDATE
                                                              : 0xF87171;
      const std::string status = !found.empty() ? std::string(LV_SYMBOL_OK) + "  FOUND"
                                   : file.download ? std::string(LV_SYMBOL_DOWNLOAD) + "  NOT DOWNLOADED"
                                   : file.optional ? "OPTIONAL  ·  NOT FOUND"
                                                   : std::string(LV_SYMBOL_CLOSE) + "  MISSING";
      block(status, color, 8);
      block(file.path, COLOR_TEXT, 2);
      std::string note = file.note;
      if (file.download) {
        const std::string download_note = found.empty() ? "Downloaded by the store during install or first launch"
                                                        : "Downloaded by the store";
        note += (note.empty() ? "" : "  ·  ") + download_note;
      }
      if (file.optional)
        note += (note.empty() ? "" : "  ·  ") + std::string("optional");
      if (!found.empty() && found != file.path)
        note += (note.empty() ? "" : "  ·  ") + std::string("found at ") + found;
      if (!note.empty()) {
        block(note, COLOR_MUTED, 0);
        set_font(below, small_font());
      }
    }
  } else if (info != nullptr) {
    block("FILES ON THE CARD", COLOR_ACCENT, 16, true);
    block("No card files are required by this app.", COLOR_INSTALLED, 4);
  }
  if (info != nullptr && !info->about.empty())
    section("ABOUT", info->about);
  if (info != nullptr && !info->controls.empty()) {
    std::string lines;
    for (const auto &line : info->controls)
      lines += (lines.empty() ? "" : "\n") + line;
    section("Controls", lines);
  }
  if (info != nullptr && (!info->upstream.empty() || !info->source.empty())) {
    std::string lines = info->upstream.empty() ? "" : "Based on " + info->upstream;
    if (!info->source.empty())
      lines += (lines.empty() ? "" : "\n") + std::string("Built from ") + info->source;
    section("Source", lines);
  }
  if (info != nullptr && !info->changelog.empty())
    section("What's new", info->changelog);
  if (below == nullptr)
    block("No listing details are available yet.\n\n" + url, COLOR_MUTED, 0);

  this->focus_detail_button_(0);
  this->progress_ui_screen_ = nullptr;  // progress moves to the loader's panel, above this page
}

void PaxLoader::close_detail_() {
  this->play_ui_sound(UI_SOUND_BACK);
  this->close_dialog_();
  if (this->detail_panel_ != nullptr) {
    // Often called from a click on one of the page's own buttons, so the page
    // is hidden now (nothing draws its icon again) and deleted after the event.
    lv_obj_add_flag(this->detail_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->detail_panel_);
    this->detail_panel_ = nullptr;
  }
  release_icon_(&this->detail_icon_);
  this->detail_buttons_.clear();
  this->detail_actions_.clear();
  this->detail_index_ = -1;
  this->detail_url_.clear();
  this->progress_ui_screen_ = nullptr;
  this->set_catalog_volume_control_visible_(true);
}

void PaxLoader::focus_detail_button_(uint8_t index) {
  if (this->detail_buttons_.empty())
    return;
  index = std::min<uint8_t>(index, static_cast<uint8_t>(this->detail_buttons_.size() - 1));
  for (lv_obj_t *button : this->detail_buttons_)
    lv_obj_remove_state(button, LV_STATE_FOCUSED);
  lv_obj_add_state(this->detail_buttons_[index], LV_STATE_FOCUSED);
  lv_group_focus_obj(this->detail_buttons_[index]);
  this->detail_focus_ = index;
}

void PaxLoader::run_detail_action_(uint8_t action) {
  const int index = this->detail_index_;
  if (index < 0 || static_cast<size_t>(index) >= this->catalog_entries_.size())
    return;
  if ((this->install_task_handle_ != nullptr || this->web_storage_busy_) && action != ACTION_BACK)
    return;  // one thing at a time; the progress panel shows the install
  const AppInfo *info = static_cast<size_t>(index) < this->app_info_.size() ? &this->app_info_[index] : nullptr;
  switch (action) {
    case ACTION_STREAM: {
      const std::string url = this->catalog_entries_[index].second;
      if (this->open_rom_selector_for_app(url))
        break;
      this->close_detail_();
      this->request_launch_url(url);
      break;
    }
    case ACTION_LAUNCH: {
      std::string path = this->catalog_entries_[index].second;
      if (info != nullptr && !info->name.empty() && !this->installed_version_(info->name).empty())
        path = this->install_dir_ + "/" + info->name + ".pax";
      if (this->open_rom_selector_for_app(path))
        break;
      this->close_detail_();
      this->request_launch(path);
      break;
    }
    case ACTION_ROM: {
      std::string path = this->catalog_entries_[index].second;
      if (info != nullptr && !info->name.empty() && !this->installed_version_(info->name).empty())
        path = this->install_dir_ + "/" + info->name + ".pax";
      this->open_rom_selector_for_app(path);
      break;
    }
    case ACTION_INSTALL:
      this->start_install_(index);
      break;
    case ACTION_SCREEN:
      this->cycle_screen_setting_(index);
      break;
    case ACTION_FAVORITE:
      this->toggle_app_favorite_(index);
      break;
    case ACTION_UNINSTALL:
      this->open_uninstall_dialog_(index);
      break;
    case ACTION_BACK:
    default:
      this->close_detail_();
      break;
  }
}

// The settings key of the file Launch / Stream would run, as the loader keys
// it at launch (canvas::app_key).
std::string PaxLoader::detail_app_key_(int index) const {
  if (index < 0 || static_cast<size_t>(index) >= this->catalog_entries_.size())
    return {};
  const AppInfo *info = static_cast<size_t>(index) < this->app_info_.size() ? &this->app_info_[index] : nullptr;
  if (info != nullptr && !info->name.empty() && !this->installed_version_(info->name).empty())
    return canvas::app_key(this->install_dir_ + "/" + info->name + ".pax");
  return canvas::app_key(this->catalog_entries_[index].second);
}

std::vector<std::string> PaxLoader::screen_options_(int index, std::string *recommended) const {
  recommended->clear();
  if (index < 0 || static_cast<size_t>(index) >= this->app_info_.size() || !this->app_info_[index].supports_canvas())
    return {};
  const AppInfo &info = this->app_info_[index];
  const std::vector<std::string> listed = info.canvas_any ? std::vector<std::string>() : info.canvas_sizes;
  *recommended = canvas::recommended(this->max_canvas_w_, this->max_canvas_h_, listed, info.canvas_recommended);
  return canvas::screen_options(canvas::offered(this->max_canvas_w_, this->max_canvas_h_, listed, *recommended),
                                *recommended);
}

// "Screen 1024x600", "Screen Default", or the recommended size marked with
// an asterisk (also what an app with no setting gets).
std::string PaxLoader::screen_label_(int index) {
  std::string recommended;
  this->screen_options_(index, &recommended);
  std::string size = this->get_app_canvas(this->detail_app_key_(index));
  if (size.empty())
    size = recommended;
  return std::string(LV_SYMBOL_IMAGE "  Screen ") + (size.empty() ? std::string("Default") : size) +
         (!size.empty() && size == recommended ? "*" : "");
}

// One press of Screen: the next size the app can use on this panel. Without a
// recommended size the list starts with Default (the device's canvas); with
// one, that size stands for "no setting" (and choosing it clears the
// setting). Saved at once; the app gets it from its next start.
void PaxLoader::cycle_screen_setting_(int index) {
  std::string recommended;
  const std::vector<std::string> options = this->screen_options_(index, &recommended);
  if (options.empty())
    return;
  const std::string app = this->detail_app_key_(index);
  std::string current = this->get_app_canvas(app);
  if (!recommended.empty() && current == recommended)
    current.clear();
  if (!this->set_app_canvas(app, canvas::next_screen_option(options, current))) {
    this->set_progress_(false, 0, 0, "%s", "Could not save the Screen setting - is the card in?");
    return;
  }
  const std::string label = this->screen_label_(index);
  for (size_t i = 0; i < this->detail_buttons_.size() && i < this->detail_actions_.size(); i++) {
    if (this->detail_actions_[i] != ACTION_SCREEN)
      continue;
    lv_obj_t *text = lv_obj_get_child(this->detail_buttons_[i], 0);
    if (text != nullptr)
      lv_label_set_text(text, label.c_str());
  }
}

// ── Uninstall's confirmation ────────────────────────────────────────────────

struct DialogContext {
  PaxLoader *loader;
  uint8_t action;
};

void PaxLoader::store_dialog_event_cb_(lv_event_t *event) {
  auto *context = static_cast<DialogContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    context->loader->run_dialog_action_(context->action);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

// A touch and d-pad target in the dialog, styled like the detail page's buttons.
static lv_obj_t *dialog_item(lv_obj_t *parent, int32_t width, int32_t height, uint32_t color, uint32_t pressed) {
  lv_obj_t *item = plain_box(parent);
  lv_obj_set_size(item, width, height);
  lv_obj_set_style_radius(item, 14, 0);
  lv_obj_set_style_bg_color(item, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(item, LV_OPA_90, 0);
  lv_obj_set_style_bg_color(item, lv_color_hex(pressed), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(item, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(item, 3, LV_STATE_FOCUSED);
  lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  return item;
}

void PaxLoader::open_uninstall_dialog_(int index) {
  if (index < 0 || static_cast<size_t>(index) >= this->app_info_.size() || this->launched_)
    return;
  const AppInfo &info = this->app_info_[index];
  if (!info.has_info || info.name.empty() || this->installed_version_(info.name).empty())
    return;
  this->close_dialog_();
  this->dialog_app_ = info.name;
  this->dialog_delete_data_ = false;
  const std::vector<std::pair<std::string, uint32_t>> data = this->downloaded_files_(info.name);
  uint64_t bytes = 0;
  for (const auto &file : data)
    bytes += file.second;

  // A dimmed layer over the detail page (it takes the touches), with the box on it.
  const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
  const int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
  lv_obj_t *shade = plain_box(lv_layer_top());
  lv_obj_set_size(shade, screen_w, screen_h);
  lv_obj_set_style_bg_color(shade, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(shade, LV_OPA_60, 0);
  lv_obj_add_flag(shade, LV_OBJ_FLAG_CLICKABLE);
  this->dialog_ = shade;

  const int32_t box_w = std::min<int32_t>(640, screen_w - 80);
  const int32_t pad = 28;
  const int32_t inner = box_w - 2 * pad;
  lv_obj_t *box = plain_box(shade);
  lv_obj_set_width(box, box_w);
  lv_obj_set_style_radius(box, 20, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(COLOR_TILE), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOR_TILE_BORDER), 0);

  lv_obj_t *heading = text_label(box, "Uninstall " + (info.title.empty() ? info.name : info.title) + "?", 0xFFFFFF);
  set_font(heading, title_font());
  lv_obj_set_pos(heading, pad, pad);
  one_line(heading, inner);
  lv_obj_t *text = text_label(box,
                              "Removes " + this->install_dir_ + "/" + info.name +
                                  ".pax and its listing. Saves, settings and files you copied yourself stay.",
                              COLOR_TEXT);
  lv_obj_set_width(text, inner);
  lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(text, heading, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 14);
  lv_obj_t *last = text;

  this->dialog_items_.clear();
  this->dialog_actions_.clear();
  auto add_item = [this](lv_obj_t *item, uint8_t action) {
    lv_obj_add_event_cb(item, store_dialog_event_cb_, LV_EVENT_ALL, new DialogContext{this, action});  // NOLINT
    add_to_default_group(item);
    this->dialog_items_.push_back(item);
    this->dialog_actions_.push_back(action);
  };
  if (!data.empty()) {
    // Only files from its install manifest that are still as downloaded. A
    // check box of plain objects: no LVGL checkbox widget needed.
    lv_obj_t *row = dialog_item(box, inner, 72, COLOR_TILE, 0x172544);
    lv_obj_align_to(row, last, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    lv_obj_t *check = plain_box(row);
    lv_obj_set_size(check, 32, 32);
    lv_obj_align(check, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_radius(check, 6, 0);
    lv_obj_set_style_bg_color(check, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(check, 2, 0);
    lv_obj_set_style_border_color(check, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(text_label(check, "", COLOR_ACCENT));  // the tick, when checked
    char what[112];
    std::snprintf(what, sizeof(what), "Also delete the data the store downloaded for it: %u file(s), %s",
                  static_cast<unsigned>(data.size()),
                  size_text(static_cast<uint32_t>(std::min<uint64_t>(bytes, UINT32_MAX))).c_str());
    lv_obj_t *label = text_label(row, what, COLOR_TEXT);
    lv_obj_set_width(label, inner - 64);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 54, 0);
    add_item(row, DIALOG_TOGGLE_DATA);
    last = row;
  }
  // Uninstall (red) and Cancel side by side.
  const int32_t button_w = (inner - 16) / 2;
  lv_obj_t *confirm = dialog_item(box, button_w, 52, 0xDC2626, 0xB91C1C);
  lv_obj_align_to(confirm, last, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 24);
  lv_obj_center(text_label(confirm, LV_SYMBOL_TRASH "  Uninstall", 0xFFFFFF));
  add_item(confirm, DIALOG_CONFIRM);
  lv_obj_t *cancel = dialog_item(box, button_w, 52, 0x1E293B, 0x334155);
  lv_obj_align_to(cancel, confirm, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
  lv_obj_center(text_label(cancel, LV_SYMBOL_CLOSE "  Cancel", 0xFFFFFF));
  add_item(cancel, DIALOG_CANCEL);

  lv_obj_update_layout(box);
  lv_obj_set_height(box, lv_obj_get_y2(confirm) + pad + 4);
  lv_obj_center(box);
  // Cancel has the focus, so a second press of A changes nothing by accident.
  this->focus_dialog_item_(static_cast<uint8_t>(this->dialog_items_.size() - 1));
}

void PaxLoader::close_dialog_() {
  if (this->dialog_ != nullptr) {
    // Usually closed from a click on its own button: hidden now, deleted after the event.
    lv_obj_add_flag(this->dialog_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->dialog_);
    this->dialog_ = nullptr;
  }
  this->dialog_items_.clear();
  this->dialog_actions_.clear();
  this->dialog_focus_ = 0;
}

void PaxLoader::focus_dialog_item_(uint8_t index) {
  if (this->dialog_items_.empty())
    return;
  index = std::min<uint8_t>(index, static_cast<uint8_t>(this->dialog_items_.size() - 1));
  for (lv_obj_t *item : this->dialog_items_)
    lv_obj_remove_state(item, LV_STATE_FOCUSED);
  lv_obj_add_state(this->dialog_items_[index], LV_STATE_FOCUSED);
  lv_group_focus_obj(this->dialog_items_[index]);
  this->dialog_focus_ = index;
}

void PaxLoader::run_dialog_action_(uint8_t action) {
  if (this->dialog_ == nullptr)
    return;
  switch (action) {
    case DIALOG_TOGGLE_DATA:
      this->dialog_delete_data_ = !this->dialog_delete_data_;
      for (size_t i = 0; i < this->dialog_items_.size() && i < this->dialog_actions_.size(); i++) {
        if (this->dialog_actions_[i] != DIALOG_TOGGLE_DATA)
          continue;
        lv_obj_t *check = lv_obj_get_child(this->dialog_items_[i], 0);  // row -> check box -> tick
        lv_obj_t *mark = check != nullptr ? lv_obj_get_child(check, 0) : nullptr;
        if (mark != nullptr)
          lv_label_set_text(mark, this->dialog_delete_data_ ? LV_SYMBOL_OK : "");
        this->focus_dialog_item_(static_cast<uint8_t>(i));
      }
      break;
    case DIALOG_CONFIRM: {
      const std::string app = this->dialog_app_;
      const bool delete_data = this->dialog_delete_data_;
      this->close_dialog_();
      this->uninstall_app(app, delete_data);  // rebuilds the page, or says why not
      break;
    }
    case DIALOG_CANCEL:
    default:
      this->close_dialog_();
      break;
  }
}

static bool store_focus_candidate_visible(lv_obj_t *object) {
  if (object == nullptr || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) ||
      lv_obj_has_state(object, LV_STATE_DISABLED))
    return false;
  // The default keypad group contains controls from every YAML page. A
  // hidden flag is not enough to identify the active page: the volume page's
  // slider, for example, can still be in the group while the Storage page is
  // active. Only accept objects on the active screen, plus the fixed top layer
  // used by the compact volume/brightness controls.
  const lv_obj_t *object_screen = lv_obj_get_screen(object);
  const lv_obj_t *active_screen = lv_scr_act();
  const lv_obj_t *top_layer = lv_layer_top();
  if (object_screen != active_screen && object_screen != top_layer)
    return false;
  for (lv_obj_t *parent = lv_obj_get_parent(object); parent != nullptr; parent = lv_obj_get_parent(parent)) {
    if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN))
      return false;
  }
  return true;
}

static bool store_focus_candidate_in_drawer(lv_obj_t *object, lv_obj_t *drawer) {
  if (object == nullptr || drawer == nullptr)
    return false;
  for (lv_obj_t *parent = object; parent != nullptr; parent = lv_obj_get_parent(parent)) {
    if (parent == drawer)
      return true;
  }
  return false;
}

// The search row is intentionally a two-control route: the wide textarea,
// followed by the small clear button.  Its controls sit just above the
// catalog source bar, so generic nearest-widget scoring can otherwise choose
// the clear button when moving upward from that bar.
static bool store_is_search_row(const lv_area_t &area) {
  return area.y1 >= 45 && area.y2 <= 110;
}

static bool store_is_wide_search_control(const lv_area_t &area) {
  return store_is_search_row(area) && lv_area_get_width(&area) >= 100;
}

static const char *store_focus_direction_name(uint8_t direction) {
  switch (direction) {
    case 0:
      return "up";
    case 1:
      return "right";
    case 2:
      return "down";
    case 3:
      return "left";
    default:
      return "none";
  }
}

static uint32_t focus_first_codepoint(const char *text) {
  if (text == nullptr || *text == '\0')
    return 0;
  const auto *bytes = reinterpret_cast<const unsigned char *>(text);
  if (bytes[0] < 0x80)
    return bytes[0];
  if ((bytes[0] & 0xE0) == 0xC0 && bytes[1] != 0)
    return ((bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F);
  if ((bytes[0] & 0xF0) == 0xE0 && bytes[1] != 0 && bytes[2] != 0)
    return ((bytes[0] & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
  return 0;
}

static const char *focus_first_label(lv_obj_t *object) {
  if (object == nullptr)
    return nullptr;
  if (lv_obj_check_type(object, &lv_label_class)) {
    const char *text = lv_label_get_text(object);
    if (text != nullptr && *text != '\0')
      return text;
  }
  const uint32_t children = lv_obj_get_child_count(object);
  for (uint32_t i = 0; i < children; i++) {
    if (const char *text = focus_first_label(lv_obj_get_child(object, static_cast<int32_t>(i))))
      return text;
  }
  return nullptr;
}

static std::string focus_tooltip_text(lv_obj_t *object) {
  if (object != nullptr && lv_obj_check_type(object, &lv_keyboard_class))
    return "On-screen keyboard";
  if (object != nullptr && lv_obj_check_type(object, &lv_textarea_class))
    return "Search field";

  const char *label = focus_first_label(object);
  if (label == nullptr || *label == '\0')
    return "Control";
  const uint32_t codepoint = focus_first_codepoint(label);
  switch (codepoint) {
    case 0xF005:
      return "Favorites";
    case 0xF013:
      return "Settings";
    case 0xF015:
      return "Home";
    case 0xF021:
      return "Refresh";
    case 0xF028:
      return "Volume";
    case 0xF053:
      return "Back";
    case 0xF062:
      return "Back to parent";
    case 0xF11B:
      return "Game favorites";
    case 0xF185:
      return "Brightness";
    case 0xF187:
      return "Internet Archive";
    default:
      break;
  }
  if (std::strcmp(label, "X") == 0)
    return "Clear search";
  return std::string(label);
}

static lv_obj_t *find_keyboard_for_textarea(lv_obj_t *root, lv_obj_t *textarea) {
  if (root == nullptr || textarea == nullptr)
    return nullptr;
  if (lv_obj_check_type(root, &lv_keyboard_class) && lv_keyboard_get_textarea(root) == textarea)
    return root;
  const uint32_t children = lv_obj_get_child_count(root);
  for (uint32_t i = 0; i < children; i++) {
    if (lv_obj_t *keyboard = find_keyboard_for_textarea(lv_obj_get_child(root, static_cast<int32_t>(i)), textarea))
      return keyboard;
  }
  return nullptr;
}

void PaxLoader::set_focus_tooltips(bool enabled) {
  this->focus_tooltips_enabled_ = enabled;
  this->update_focus_tooltip_();
  ESP_LOGI(TAG, "Controller focus tooltips: %s", enabled ? "on" : "off");
}

void PaxLoader::update_focus_tooltip_() {
  if (!this->focus_tooltips_enabled_) {
    if (this->focus_tooltip_ != nullptr)
      lv_obj_add_flag(this->focus_tooltip_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_group_t *group = lv_group_get_default();
  lv_obj_t *focused = group != nullptr ? lv_group_get_focused(group) : nullptr;
  if (focused == nullptr || !store_focus_candidate_visible(focused)) {
    if (this->focus_tooltip_ != nullptr)
      lv_obj_add_flag(this->focus_tooltip_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  std::string text = focus_tooltip_text(focused);
  if (focused == this->catalog_header_) {
    text = "Library source";
    if (!this->catalogs_.empty() && this->catalog_index_ < this->catalogs_.size())
      text = this->catalogs_[this->catalog_index_].name + " selector";
  } else if (focused == this->catalog_volume_slider_)
    text = "Volume slider";
  else if (focused == this->catalog_brightness_slider_)
    text = "Brightness slider";
  if (text.empty())
    return;
  if (this->focus_tooltip_ == nullptr) {
    lv_obj_t *panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(panel, 280, 38);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B1626), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x55D6FF), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 9, 0);
    lv_obj_set_style_pad_all(panel, 7, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *label = lv_label_create(panel);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_height(label, lv_pct(100));
    lv_obj_set_style_text_color(label, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    // Size the panel for the complete tooltip text below. If a long app
    // label still reaches the screen edge, wrap it instead of replacing the
    // end with LVGL's ellipsis (for example, "Game favorites" -> "Game favor...").
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
    this->focus_tooltip_ = panel;
    this->focus_tooltip_label_ = label;
  }
  lv_label_set_text(this->focus_tooltip_label_, text.c_str());
  const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
  const int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
  // Include the panel padding and border in the estimate. The previous
  // estimate sized the outer box too tightly, so the label's dot mode could
  // ellipsize even short phrases such as "Game favorites". The screen-width
  // cap still keeps the tooltip inside the display; LVGL wraps only genuinely
  // long app names.
  constexpr int32_t TOOLTIP_HORIZONTAL_PADDING = 32;
  constexpr int32_t TOOLTIP_CHARACTER_WIDTH = 11;
  const int32_t measured_w = TOOLTIP_HORIZONTAL_PADDING +
                             static_cast<int32_t>(std::min<size_t>(128, text.size())) * TOOLTIP_CHARACTER_WIDTH;
  const int32_t tooltip_w = std::max<int32_t>(120, std::min<int32_t>(screen_w - 8, measured_w));
  const int32_t text_capacity = std::max<int32_t>(1, (tooltip_w - TOOLTIP_HORIZONTAL_PADDING) /
                                                         TOOLTIP_CHARACTER_WIDTH);
  const int32_t tooltip_h = text.size() > static_cast<size_t>(text_capacity) ? 56 : 38;
  lv_obj_set_size(this->focus_tooltip_, tooltip_w, tooltip_h);
  lv_area_t area;
  lv_obj_get_coords(focused, &area);
  const int32_t x = std::max<int32_t>(4, std::min<int32_t>(screen_w - tooltip_w - 4,
                                                            area.x1 + (lv_area_get_width(&area) - tooltip_w) / 2));
  int32_t y = area.y1 - tooltip_h - 6;
  if (y < 42)
    y = area.y2 + 6;
  y = std::max<int32_t>(4, std::min<int32_t>(screen_h - tooltip_h - 4, y));
  lv_obj_set_pos(this->focus_tooltip_, x, y);
  lv_obj_clear_flag(this->focus_tooltip_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(this->focus_tooltip_);
}

// Keep controller-focus diagnostics in the serial log. This is intentionally
// edge-triggered so it identifies the highlighted object without flooding the
// log once per ESPHome loop iteration. The same focus transition drives the
// optional tooltip shown above the active control.
void PaxLoader::log_focus_(const char *reason) {
  lv_group_t *group = lv_group_get_default();
  static lv_obj_t *last_object = nullptr;
  lv_obj_t *object = group != nullptr ? lv_group_get_focused(group) : nullptr;
  if (object == last_object) {
    this->update_focus_tooltip_();
    return;
  }
  last_object = object;

  if (object == nullptr) {
    ESP_LOGI(TAG, "UI focus: none (%s)", reason != nullptr ? reason : "change");
    return;
  }

  lv_area_t area;
  lv_obj_get_coords(object, &area);
  char text[80] = {};
  if (lv_obj_check_type(object, &lv_label_class)) {
    std::snprintf(text, sizeof(text), "%.64s", lv_label_get_text(object));
  } else {
    const uint32_t children = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < children; i++) {
      lv_obj_t *child = lv_obj_get_child(object, static_cast<int32_t>(i));
      if (child != nullptr && lv_obj_check_type(child, &lv_label_class)) {
        std::snprintf(text, sizeof(text), "%.64s", lv_label_get_text(child));
        break;
      }
    }
  }

  uint32_t group_index = UINT32_MAX;
  const uint32_t count = group != nullptr ? lv_group_get_obj_count(group) : 0;
  for (uint32_t i = 0; i < count; i++) {
    if (lv_group_get_obj_by_index(group, i) == object) {
      group_index = i;
      break;
    }
  }
  ESP_LOGI(TAG, "UI focus: obj=%p group=%u/%u area=(%ld,%ld)-(%ld,%ld) text=\"%s\" (%s)", object,
           static_cast<unsigned>(group_index), static_cast<unsigned>(count), static_cast<long>(area.x1),
           static_cast<long>(area.y1), static_cast<long>(area.x2), static_cast<long>(area.y2),
           text[0] != '\0' ? text : "<no label>", reason != nullptr ? reason : "change");
  this->update_focus_tooltip_();
}

static bool store_focus_candidate_in_lane(const lv_area_t &current, const lv_area_t &candidate,
                                          uint8_t direction) {
  const int32_t current_w = lv_area_get_width(&current);
  const int32_t current_h = lv_area_get_height(&current);
  const int32_t candidate_w = lv_area_get_width(&candidate);
  const int32_t candidate_h = lv_area_get_height(&candidate);
  const int32_t current_cx = current.x1 + current_w / 2;
  const int32_t current_cy = current.y1 + current_h / 2;
  const int32_t candidate_cx = candidate.x1 + candidate_w / 2;
  const int32_t candidate_cy = candidate.y1 + candidate_h / 2;

  if (direction == 1 || direction == 3) {
    // Left/right should stay on the same visual row. Allow a small gap so a
    // toolbar button and a nearby search control still feel like one row.
    const bool overlap = candidate.y1 <= current.y2 && candidate.y2 >= current.y1;
    const int32_t row_gap = std::max<int32_t>(16, (current_h + candidate_h) / 2 + 8);
    return overlap || std::abs(candidate_cy - current_cy) <= row_gap;
  }

  // Up/down should stay in the same visual column. This is what makes the
  // first tile row reach the toolbar above it instead of jumping to a button
  // that happens to be closer in the unrelated horizontal direction.
  const bool overlap = candidate.x1 <= current.x2 && candidate.x2 >= current.x1;
  const int32_t column_gap = std::max<int32_t>(16, (current_w + candidate_w) / 2 + 8);
  return overlap || std::abs(candidate_cx - current_cx) <= column_gap;
}

static int store_tile_index(const std::vector<lv_obj_t *> &tiles, lv_obj_t *object) {
  for (size_t i = 0; i < tiles.size(); i++) {
    if (tiles[i] == object)
      return static_cast<int>(i);
  }
  return -1;
}

// Find the closest visible focus target in the requested screen direction.
// LVGL's keypad input sends LV_KEY_UP/DOWN/LEFT/RIGHT to the focused object;
// it does not perform spatial focus navigation for ordinary buttons. The
// launcher therefore does that part explicitly for USB/gamepad input.
bool PaxLoader::focus_store_spatial_(uint8_t direction) {
  lv_group_t *group = lv_group_get_default();
  if (group == nullptr)
    return false;

  lv_obj_t *current = lv_group_get_focused(group);
  const uint32_t count = lv_group_get_obj_count(group);
  if (count == 0)
    return false;

  if (!store_focus_candidate_visible(current)) {
    for (uint32_t i = 0; i < count; i++) {
      lv_obj_t *candidate = lv_group_get_obj_by_index(group, i);
      if (store_focus_candidate_visible(candidate)) {
        lv_group_focus_obj(candidate);
        this->log_focus_("recovered hidden focus");
        return true;
      }
    }
    return false;
  }

  lv_area_t current_area;
  lv_obj_get_coords(current, &current_area);

  // A search field is a wide control above the content area.  Give Down an
  // explicit content target instead of letting the generic nearest-object
  // scorer compare it with the adjacent Clear button or another toolbar
  // control.  This is especially important on Archive, where the list rows
  // are rebuilt dynamically and are not part of the YAML creation order.
  if (direction == 2 && store_is_wide_search_control(current_area)) {
    const lv_obj_t *active_screen = lv_scr_act();
    const bool archive_active = this->archive_container_ != nullptr &&
                                active_screen == lv_obj_get_screen(this->archive_container_);
    const bool rom_active = this->rom_selector_container_ != nullptr &&
                            active_screen == lv_obj_get_screen(this->rom_selector_container_);
    const bool catalog_active = this->catalog_container_ != nullptr &&
                                active_screen == lv_obj_get_screen(this->catalog_container_);

    if (catalog_active && this->store_ui_ && !this->catalog_tiles_.empty()) {
      this->set_catalog_selection_(0);
      this->log_focus_("search down to first app");
      return true;
    }
    if (rom_active && !this->rom_selector_buttons_.empty()) {
      this->set_rom_selector_selection_(0);
      this->log_focus_("search down to first game");
      return true;
    }

    lv_obj_t *content = archive_active ? this->archive_container_
                                       : catalog_active ? this->catalog_container_ : nullptr;
    if (content != nullptr) {
      lv_obj_t *best_below = nullptr;
      int32_t best_y = INT32_MAX;
      int32_t best_x = INT32_MAX;
      for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *candidate = lv_group_get_obj_by_index(group, i);
        if (!store_focus_candidate_visible(candidate) || candidate == current ||
            (!this->side_menu_open_ && store_focus_candidate_in_drawer(candidate, this->drawer_)))
          continue;
        bool belongs_to_content = false;
        for (lv_obj_t *parent = candidate; parent != nullptr; parent = lv_obj_get_parent(parent)) {
          if (parent == content) {
            belongs_to_content = true;
            break;
          }
        }
        if (!belongs_to_content)
          continue;
        lv_area_t area;
        lv_obj_get_coords(candidate, &area);
        if (store_is_search_row(area) || area.y1 <= current_area.y2)
          continue;
        const int32_t x = area.x1 + lv_area_get_width(&area) / 2;
        if (area.y1 < best_y || (area.y1 == best_y && x < best_x)) {
          best_below = candidate;
          best_y = area.y1;
          best_x = x;
        }
      }
      if (best_below != nullptr) {
        lv_group_focus_obj(best_below);
        lv_obj_scroll_to_view_recursive(best_below, LV_ANIM_ON);
        this->log_focus_("search down to content");
        return true;
      }
    }
  }

  const int32_t current_x = current_area.x1 + lv_area_get_width(&current_area) / 2;
  const int32_t current_y = current_area.y1 + lv_area_get_height(&current_area) / 2;
  const bool current_top_row = current_area.y1 <= 45 && current_area.y2 <= 45;
  lv_obj_t *best = nullptr;
  int64_t best_score = INT64_MAX;

  // First search the same visual lane. A second pass is only a fallback for
  // intentional transitions such as the search box above a list or the last
  // tile row below the toolbar. This prevents focus from drifting by widget
  // creation order when a page has several unrelated controls.
  for (int pass = 0; pass < 2 && best == nullptr; pass++) {
    for (uint32_t i = 0; i < count; i++) {
      lv_obj_t *candidate = lv_group_get_obj_by_index(group, i);
      // The parked drawer tab is a touch target, but it is not part of the
      // main toolbar's directional route. Select opens the drawer explicitly;
      // allowing spatial focus to land on its hidden panel/tab makes Right
      // from the search controls appear to get lost in a blank object.
      if (!store_focus_candidate_visible(candidate) || candidate == current ||
          (!this->side_menu_open_ && store_focus_candidate_in_drawer(candidate, this->drawer_)))
        continue;

      lv_area_t area;
      lv_obj_get_coords(candidate, &area);
      const bool current_search_row = store_is_search_row(current_area);
      const bool candidate_search_row = store_is_search_row(area);
      const bool current_wide_search = store_is_wide_search_control(current_area);
      const bool candidate_wide_search = store_is_wide_search_control(area);

      // Keep the search field and its clear button as one horizontal route.
      // In particular, Up from the source/count bar must select the search
      // field, never the clear button; Clear is reached only with Right.
      if (direction == 0 && !current_search_row && candidate_search_row &&
          !candidate_wide_search)
        continue;
      // The search field and Clear share the same y band. Their bounds can
      // differ by a few pixels after LVGL applies fonts/padding, which makes
      // the Clear button look very slightly "above" the textarea to the
      // generic spatial scorer. Never allow Up from the wide search field to
      // remain in that row; the only horizontal route into Clear is Right.
      if (current_wide_search && direction == 0 && candidate_search_row)
        continue;
      if ((direction == 1 || direction == 3) && current_search_row && !candidate_search_row)
        continue;
      // The fixed toolbar is its own horizontal route. Do not let a search
      // control one row below it win the fallback pass when Right reaches the
      // toolbar edge; that made Refresh jump down to Clear instead of wrapping
      // to the first toolbar button (Settings).
      if (current_top_row && (direction == 1 || direction == 3) && (area.y1 > 45 || area.y2 > 45))
        continue;
      if (pass == 0 && !store_focus_candidate_in_lane(current_area, area, direction))
        continue;
      const int32_t x = area.x1 + lv_area_get_width(&area) / 2;
      const int32_t y = area.y1 + lv_area_get_height(&area) / 2;
      const int32_t dx = x - current_x;
      const int32_t dy = y - current_y;

      bool ahead = false;
      if (direction == 0)
        ahead = dy < 0;
      else if (direction == 1)
        ahead = dx > 0;
      else if (direction == 2)
        ahead = dy > 0;
      else
        ahead = dx < 0;
      if (!ahead)
        continue;

      // Prefer the nearest target in the requested direction, with a small
      // penalty for being off the current row or column.
      const int32_t primary = (direction == 1 || direction == 3) ? std::abs(dx) : std::abs(dy);
      const int32_t cross = (direction == 1 || direction == 3) ? std::abs(dy) : std::abs(dx);
      const int64_t score = static_cast<int64_t>(primary) * 1024 + static_cast<int64_t>(cross);
      if (score < best_score) {
        best_score = score;
        best = candidate;
      }
    }
  }

  // The fixed toolbar is a deliberate left-to-right strip. Once the user has
  // reached its right edge, Right is the natural gesture for the parked side
  // menu: open it immediately and focus its first control. The drawer tab is
  // intentionally excluded from ordinary spatial scoring because it is only
  // a narrow touch target while parked. Keep the old wrap behavior for Left
  // at the toolbar's left edge and for any page that has no drawer.
  if (best == nullptr && direction == 1 && !this->side_menu_open_ && this->drawer_ != nullptr) {
    this->set_side_menu(true);
    this->log_focus_("right edge opened side menu");
    return true;
  }

  // Wrap the fixed toolbar at its left edge so repeated Left from Settings
  // returns to the last top-row control instead of stopping at the edge.
  if (best == nullptr && (direction == 1 || direction == 3) && current_area.y1 <= 45 && current_area.y2 <= 45) {
    int32_t edge = direction == 1 ? INT32_MAX : INT32_MIN;
    for (uint32_t i = 0; i < count; i++) {
      lv_obj_t *candidate = lv_group_get_obj_by_index(group, i);
      if (!store_focus_candidate_visible(candidate) || candidate == current ||
          (!this->side_menu_open_ && store_focus_candidate_in_drawer(candidate, this->drawer_)))
        continue;
      lv_area_t area;
      lv_obj_get_coords(candidate, &area);
      if (area.y1 > 45 || area.y2 > 45)
        continue;
      const int32_t x = area.x1 + lv_area_get_width(&area) / 2;
      if ((direction == 1 && x < edge) || (direction == 3 && x > edge)) {
        edge = x;
        best = candidate;
      }
    }
    if (best != nullptr) {
      lv_group_focus_obj(best);
      lv_obj_scroll_to_view_recursive(best, LV_ANIM_ON);
      this->log_focus_(direction == 1 ? "top-row wrap right" : "top-row wrap left");
      return true;
    }
  }

  if (best == nullptr)
  {
    ESP_LOGW(TAG, "UI navigation: no %s target from current focus", store_focus_direction_name(direction));
    return false;
  }

  const int tile = store_tile_index(this->catalog_tiles_, best);
  if (tile >= 0)
    this->set_catalog_selection_(static_cast<uint16_t>(tile));
  else {
    lv_group_focus_obj(best);
    lv_obj_scroll_to_view_recursive(best, LV_ANIM_ON);
  }
  this->log_focus_(store_focus_direction_name(direction));
  return true;
}

bool PaxLoader::activate_store_focus_() {
  lv_group_t *group = lv_group_get_default();
  lv_obj_t *focused = group != nullptr ? lv_group_get_focused(group) : nullptr;
  if (focused == nullptr || !store_focus_candidate_visible(focused))
    return false;

  const int tile = store_tile_index(this->catalog_tiles_, focused);
  if (tile >= 0) {
    this->play_ui_sound(UI_SOUND_CONFIRM);
    this->open_detail_(tile);
  } else {
    // Static toolbar controls are normal LVGL buttons; dispatching their click
    // event reuses the same action used by touch input.
    lv_obj_send_event(focused, LV_EVENT_CLICKED, nullptr);
    // A controller can activate a textarea without generating the same touch
    // focus transition that LVGL normally uses. Find the keyboard attached to
    // this search field and make it the actual group focus target so D-pad/A
    // moves through keys instead of continuing to operate the page controls.
    if (lv_obj_check_type(focused, &lv_textarea_class)) {
      lv_obj_t *screen = lv_obj_get_screen(focused);
      lv_obj_t *keyboard = find_keyboard_for_textarea(screen, focused);
      if (keyboard != nullptr) {
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(keyboard, focused);
        lv_obj_move_foreground(keyboard);
        lv_group_focus_obj(keyboard);
        ESP_LOGI(TAG, "Search keyboard focus: textarea=%p keyboard=%p", focused, keyboard);
      }
    }
  }
  return true;
}

// Returns true when the store view handled the input.
bool PaxLoader::handle_store_controls_(uint8_t newly_pressed, bool a_pressed, bool b_pressed, bool l_pressed,
                                        bool r_pressed, bool select_pressed) {
  const bool up = newly_pressed & (1U << 0), right = newly_pressed & (1U << 1), down = newly_pressed & (1U << 2),
             left = newly_pressed & (1U << 3);
  lv_group_t *focus_group = lv_group_get_default();
  this->log_focus_("input");
  if (newly_pressed != 0) {
    const uint8_t direction = up ? 0 : right ? 1 : down ? 2 : 3;
    ESP_LOGI(TAG, "UI navigation: %s", store_focus_direction_name(direction));
  }
  if (this->dialog_ != nullptr) {
    // Uninstall's confirmation: any direction moves, A presses, B cancels.
    if ((up || left) && this->dialog_focus_ > 0)
      this->focus_dialog_item_(this->dialog_focus_ - 1);
    else if (down || right)
      this->focus_dialog_item_(this->dialog_focus_ + 1);
    if (a_pressed && this->dialog_focus_ < this->dialog_actions_.size())
      this->run_dialog_action_(this->dialog_actions_[this->dialog_focus_]);
    else if (b_pressed)
      this->close_dialog_();
    this->log_focus_("dialog input");
    return true;
  }

  // LVGL's keyboard is a button matrix. While it owns focus, pass controller
  // arrows and A through the group so LVGL moves/highlights an actual key and
  // Enter activates it. Treating the keyboard as one ordinary toolbar object
  // was why the popup appeared but its letters could not be navigated.
  lv_group_t *keyboard_group = lv_group_get_default();
  lv_obj_t *keyboard_focused = keyboard_group != nullptr ? lv_group_get_focused(keyboard_group) : nullptr;
  if (keyboard_focused != nullptr && lv_obj_check_type(keyboard_focused, &lv_keyboard_class) &&
      store_focus_candidate_visible(keyboard_focused)) {
    uint32_t key = 0;
    if (up)
      key = LV_KEY_UP;
    else if (right)
      key = LV_KEY_RIGHT;
    else if (down)
      key = LV_KEY_DOWN;
    else if (left)
      key = LV_KEY_LEFT;
    if (key != 0) {
      lv_group_send_data(keyboard_group, key);
      this->play_ui_sound(UI_SOUND_CLICK);
    }
    if (a_pressed)
      lv_group_send_data(keyboard_group, LV_KEY_ENTER);
    if (b_pressed) {
      lv_obj_t *textarea = lv_keyboard_get_textarea(keyboard_focused);
      lv_obj_send_event(keyboard_focused, LV_EVENT_CANCEL, nullptr);
      if (textarea != nullptr)
        lv_group_focus_obj(textarea);
    }
    this->log_focus_("keyboard input");
    return true;
  }

  // Compact toolbar sliders are edited in place. Do not let Left/Right move
  // focus to another toolbar button while the user is changing a value; B
  // closes the slider and returns focus to the icon that opened it.
  lv_obj_t *compact_focused = focus_group != nullptr ? lv_group_get_focused(focus_group) : nullptr;
  const bool compact_volume = compact_focused == this->catalog_volume_slider_;
  const bool compact_brightness = compact_focused == this->catalog_brightness_slider_;
  if (compact_volume || compact_brightness) {
    if (b_pressed) {
      if (compact_volume)
        this->toggle_catalog_volume_slider();
      else
        this->toggle_catalog_brightness_slider();
      this->play_ui_sound(UI_SOUND_BACK);
      this->log_focus_("compact slider closed");
      return true;
    }
    if (left || right) {
      lv_obj_t *slider = compact_volume ? this->catalog_volume_slider_ : this->catalog_brightness_slider_;
      const int32_t min_value = compact_volume ? 0 : 5;
      const int32_t max_value = 100;
      const int32_t current_value = lv_slider_get_value(slider);
      const int32_t next_value = std::max(min_value, std::min(max_value, current_value + (right ? 1 : -1)));
      lv_slider_set_value(slider, next_value, LV_ANIM_OFF);
      lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
      this->log_focus_("compact slider adjust");
      return true;
    }
    this->log_focus_("compact slider focus");
    return true;
  }
  if (this->detail_panel_ != nullptr) {
    if (up && this->detail_focus_ > 0) {
      this->play_ui_sound(UI_SOUND_CLICK);
      this->focus_detail_button_(this->detail_focus_ - 1);
    } else if (up && this->detail_focus_ == 0) {
      if (this->focus_store_spatial_(0))
        this->play_ui_sound(UI_SOUND_CLICK);
    } else if (down && this->detail_focus_ + 1 < this->detail_buttons_.size()) {
      this->play_ui_sound(UI_SOUND_CLICK);
      this->focus_detail_button_(this->detail_focus_ + 1);
    } else if (down) {
      if (this->focus_store_spatial_(2))
        this->play_ui_sound(UI_SOUND_CLICK);
    }
    lv_group_t *group = lv_group_get_default();
    lv_obj_t *focused = group != nullptr ? lv_group_get_focused(group) : nullptr;
    const bool detail_focused = this->detail_focus_ < this->detail_buttons_.size() &&
                                focused == this->detail_buttons_[this->detail_focus_];
    if (a_pressed && detail_focused && this->detail_focus_ < this->detail_actions_.size()) {
      this->play_ui_sound(UI_SOUND_CONFIRM);
      this->run_detail_action_(this->detail_actions_[this->detail_focus_]);
    } else if (a_pressed) {
      this->activate_store_focus_();
    } else if (b_pressed) {
      this->close_detail_();
    }
    this->log_focus_("detail input");
    return true;
  }
  if (!this->store_ui_)
    return false;
  if (select_pressed) {
    this->play_ui_sound(UI_SOUND_CLICK);
    this->toggle_side_menu();
    return true;
  }
  if (this->side_menu_open_ && this->drawer_ != nullptr && !l_pressed && !r_pressed) {
    lv_obj_t *drawer_focused = focus_group != nullptr ? lv_group_get_focused(focus_group) : nullptr;
    const bool drawer_volume = drawer_focused == this->drawer_volume_slider_;
    const bool drawer_brightness = drawer_focused == this->drawer_brightness_slider_;
    if ((left || right) && (drawer_volume || drawer_brightness)) {
      lv_obj_t *slider = drawer_volume ? this->drawer_volume_slider_ : this->drawer_brightness_slider_;
      const int32_t minimum = drawer_volume ? 0 : 5;
      const int32_t current = lv_slider_get_value(slider);
      const int32_t next = std::max(minimum, std::min<int32_t>(100, current + (right ? 1 : -1)));
      lv_slider_set_value(slider, next, LV_ANIM_OFF);
      lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
      this->log_focus_("drawer slider adjust");
      return true;
    }
    if (up && this->drawer_focus_ > 0) {
      this->play_ui_sound(UI_SOUND_CLICK);
      this->focus_drawer_button_(this->drawer_focus_ - 1);
    } else if (down) {
      this->play_ui_sound(UI_SOUND_CLICK);
      this->focus_drawer_button_(this->drawer_focus_ + 1);
    }
    if (a_pressed && this->drawer_focus_ < this->drawer_actions_.size()) {
      this->play_ui_sound(UI_SOUND_CONFIRM);
      this->run_drawer_action_(this->drawer_actions_[this->drawer_focus_]);
    } else if (b_pressed || right) {
      this->play_ui_sound(UI_SOUND_BACK);
      this->set_side_menu(false);
    }
    this->log_focus_("drawer input");
    return true;
  }
  if (l_pressed || r_pressed) {
    this->play_ui_sound(UI_SOUND_CHIME);
    this->next_catalog(r_pressed ? 1 : -1);
    this->log_focus_("catalog switch");
    return true;
  }
  const size_t count = this->catalog_tiles_.size();
  if (count == 0) {
    if (newly_pressed != 0) {
      const uint8_t direction = up ? 0 : right ? 1 : down ? 2 : 3;
      if (this->focus_store_spatial_(direction))
        this->play_ui_sound(UI_SOUND_CLICK);
    }
    if (a_pressed)
      this->activate_store_focus_();
    this->log_focus_("toolbar input");
    return true;
  }

  lv_group_t *group = lv_group_get_default();
  lv_obj_t *focused = group != nullptr ? lv_group_get_focused(group) : nullptr;
  const int focused_tile = store_tile_index(this->catalog_tiles_, focused);
  bool moved = false;
  if (newly_pressed != 0) {
    // Preserve the grid's natural row/column behavior while a tile has focus.
    // At an edge, continue into the visible toolbar instead of stopping.
    if (focused_tile >= 0) {
      int next = focused_tile;
      const int cols = std::max<int>(1, this->grid_columns_);
      if (left)
        next -= 1;
      else if (right)
        next += 1;
      else if (up)
        next -= cols;
      else if (down)
        next += cols;
      if (next >= 0 && next < static_cast<int>(count)) {
        this->play_ui_sound(UI_SOUND_CLICK);
        this->set_catalog_selection_(static_cast<uint16_t>(next));
        moved = true;
      }
    }
    if (!moved) {
      const uint8_t direction = up ? 0 : right ? 1 : down ? 2 : 3;
      moved = this->focus_store_spatial_(direction);
      if (moved)
        this->play_ui_sound(UI_SOUND_CLICK);
    }
  }
  if (a_pressed)
    this->activate_store_focus_();
  this->log_focus_("grid input");
  return true;
}

// ── Side menu ───────────────────────────────────────────────────────────────

static constexpr int32_t DRAWER_W = 300;
static constexpr int32_t DRAWER_TAB = 34;
static constexpr int32_t DRAWER_PARKED_VISIBLE = DRAWER_TAB / 2;
static constexpr int DRAWER_TOGGLE = -2;
static constexpr int DRAWER_REFRESH = -1;
static constexpr int DRAWER_VOLUME = -3;
static constexpr int DRAWER_BRIGHTNESS = -4;
static constexpr int DRAWER_AUTO_BRIGHTNESS = -5;

struct DrawerContext {
  PaxLoader *loader;
  int action;
};

// The drawer's x on its screen: parked with only the tab showing, or open.
// It sits over the screen's padding so it reaches the edge.
static int32_t drawer_x(lv_obj_t *drawer, bool open) {
  lv_obj_t *screen = lv_obj_get_parent(drawer);
  return lv_obj_get_content_width(screen) + lv_obj_get_style_pad_right(screen, LV_PART_MAIN) -
         (open ? DRAWER_W : DRAWER_PARKED_VISIBLE);
}

static void slide_drawer(lv_obj_t *drawer, int32_t to) {
  lv_anim_delete(drawer, nullptr);
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, drawer);
  lv_anim_set_values(&anim, lv_obj_get_x(drawer), to);
  lv_anim_set_duration(&anim, 200);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&anim, [](void *obj, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t *>(obj), x); });
  lv_anim_start(&anim);
}

void PaxLoader::store_drawer_event_cb_(lv_event_t *event) {
  auto *context = static_cast<DrawerContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    if (context->action == DRAWER_TOGGLE) {
      context->loader->play_ui_sound(UI_SOUND_CLICK);
      context->loader->toggle_side_menu();
    } else {
      // Dynamic drawer buttons do not have YAML on_click actions, so provide
      // the same confirmation sound as the static launcher buttons.
      context->loader->play_ui_sound(UI_SOUND_CONFIRM);
      context->loader->run_drawer_action_(context->action);
    }
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

void PaxLoader::build_drawer_() {
  if (this->drawer_ != nullptr) {
    lv_obj_add_flag(this->drawer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->drawer_);
    this->drawer_ = nullptr;
  }
  this->drawer_volume_slider_ = nullptr;
  this->drawer_brightness_slider_ = nullptr;
  this->drawer_auto_brightness_button_ = nullptr;
  this->drawer_auto_brightness_label_ = nullptr;
  this->drawer_buttons_.clear();
  this->drawer_actions_.clear();
  if (!this->store_ui_ || this->catalog_container_ == nullptr)
    return;
  // The drawer is a launcher-wide control, not part of the scrolling Library
  // page.  Attach it to LVGL's top layer so the parked tab and open panel stay
  // available when the active screen changes to Archive, Settings, Favorites,
  // or a ROM selector.  Keeping it on catalog_container_'s screen made it
  // disappear as soon as another YAML page was loaded.
  lv_obj_t *screen = lv_layer_top();
  if (screen == nullptr)
    return;
  lv_obj_t *drawer = plain_box(screen);
  lv_obj_add_flag(drawer, LV_OBJ_FLAG_FLOATING);  // not moved by scrolling or layouts
  // Leave the fixed top toolbar/header uncovered. The bottom margin also
  // keeps the drawer from touching the display edge on shorter panels.
  constexpr int32_t drawer_top = 48;
  constexpr int32_t drawer_bottom = 12;
  const int32_t drawer_height = std::max<int32_t>(0, lv_obj_get_height(screen) - drawer_top - drawer_bottom);
  lv_obj_set_size(drawer, DRAWER_W, drawer_height);
  lv_obj_set_y(drawer, drawer_top);
  lv_obj_set_style_bg_opa(drawer, LV_OPA_TRANSP, 0);
  this->drawer_ = drawer;
  lv_obj_set_x(drawer, drawer_x(drawer, this->side_menu_open_));

  // The panel: the source's name and where it reads from, then its buttons.
  const int32_t panel_w = DRAWER_W - DRAWER_TAB + 8;
  lv_obj_t *panel = plain_box(drawer);
  lv_obj_set_size(panel, panel_w, lv_pct(100));
  lv_obj_set_x(panel, DRAWER_TAB - 8);
  lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_TILE), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_TILE_BORDER), 0);
  lv_obj_set_style_pad_all(panel, 20, 0);

  const CatalogSource *source = this->catalogs_.empty() ? nullptr : &this->catalogs_[this->catalog_index_];
  const int32_t inner = panel_w - 40;
  lv_obj_t *title = text_label(panel, source != nullptr ? source->name : std::string("Library"), COLOR_ACCENT);
  set_font(title, title_font());
  one_line(title, inner);
  char count[32];
  std::snprintf(count, sizeof(count), "%u app(s)", static_cast<unsigned>(this->catalog_entries_.size()));
  std::string about_source = (source != nullptr ? source->url : this->catalog_url_) + "\n" + count;
  // A folder source reads the storage roots: say what each holds (or that it
  // is missing), e.g. "Root files: sd 29 | usb0 -". Rebuilt on every refresh.
  const std::string &url = source != nullptr ? source->url : this->catalog_url_;
  if (!url.empty() && url[0] == '/') {
    std::string roots;
    for (const auto &root : this->data_search_) {
      int entries = -1;
      if (DIR *dir = opendir(runtime_path(root.c_str()).c_str())) {
        entries = 0;
        while (dirent *entry = readdir(dir)) {
          if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
            entries++;
        }
        closedir(dir);
      }
      roots += (roots.empty() ? "" : "  |  ") + root.substr(1) + " " +
               (entries < 0 ? std::string("-") : std::to_string(entries));
    }
    if (!roots.empty())
      about_source += "\nRoot files: " + roots;
  }
  lv_obj_t *where = text_label(panel, about_source, COLOR_MUTED);
  set_font(where, small_font());
  lv_obj_set_width(where, inner);
  lv_label_set_long_mode(where, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(where, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
  lv_obj_update_layout(panel);
  int32_t y = lv_obj_get_y2(where) + 14;

  auto add_drawer_caption = [&](const char *text) {
    lv_obj_t *caption = text_label(panel, text, COLOR_TEXT);
    set_font(caption, small_font());
    lv_obj_set_width(caption, inner);
    lv_obj_set_height(caption, 22);
    lv_obj_set_pos(caption, 0, y);
    y += 24;
    return caption;
  };
  auto style_drawer_slider = [&](lv_obj_t *slider) {
    lv_obj_set_size(slider, inner, 24);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(COLOR_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(slider, 2, LV_STATE_FOCUSED);
    lv_obj_set_pos(slider, 0, y);
    y += 34;
  };

  add_drawer_caption("BRIGHTNESS");
  this->drawer_brightness_slider_ = lv_slider_create(panel);
  lv_slider_set_range(this->drawer_brightness_slider_, 5, 100);
  const int32_t brightness = this->catalog_brightness_number_ != nullptr &&
                                     std::isfinite(this->catalog_brightness_number_->state)
                                 ? std::max<int32_t>(5, std::min<int32_t>(100, static_cast<int32_t>(
                                                                                 this->catalog_brightness_number_->state)))
                                 : 100;
  lv_slider_set_value(this->drawer_brightness_slider_, brightness, LV_ANIM_OFF);
  style_drawer_slider(this->drawer_brightness_slider_);
  lv_obj_add_event_cb(
      this->drawer_brightness_slider_,
      [](lv_event_t *event) {
        if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED)
          return;
        auto *loader = static_cast<PaxLoader *>(lv_event_get_user_data(event));
        if (loader == nullptr)
          return;
        loader->set_brightness_level(
            lv_slider_get_value(static_cast<const lv_obj_t *>(lv_event_get_target(event))));
      },
      LV_EVENT_VALUE_CHANGED, this);
  add_to_default_group(this->drawer_brightness_slider_);
  this->drawer_buttons_.push_back(this->drawer_brightness_slider_);
  this->drawer_actions_.push_back(DRAWER_BRIGHTNESS);

  add_drawer_caption("VOLUME");
  this->drawer_volume_slider_ = lv_slider_create(panel);
  lv_slider_set_range(this->drawer_volume_slider_, 0, 100);
  lv_slider_set_value(this->drawer_volume_slider_, this->master_volume_, LV_ANIM_OFF);
  style_drawer_slider(this->drawer_volume_slider_);
  lv_obj_add_event_cb(
      this->drawer_volume_slider_,
      [](lv_event_t *event) {
        if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED)
          return;
        auto *loader = static_cast<PaxLoader *>(lv_event_get_user_data(event));
        if (loader != nullptr)
          loader->set_master_volume(lv_slider_get_value(static_cast<const lv_obj_t *>(lv_event_get_target(event))));
      },
      LV_EVENT_VALUE_CHANGED, this);
  add_to_default_group(this->drawer_volume_slider_);
  this->drawer_buttons_.push_back(this->drawer_volume_slider_);
  this->drawer_actions_.push_back(DRAWER_VOLUME);

  this->drawer_auto_brightness_button_ = plain_box(panel);
  lv_obj_set_size(this->drawer_auto_brightness_button_, inner, 40);
  lv_obj_set_pos(this->drawer_auto_brightness_button_, 0, y);
  y += 50;
  lv_obj_set_style_radius(this->drawer_auto_brightness_button_, 10, 0);
  lv_obj_set_style_bg_color(this->drawer_auto_brightness_button_, lv_color_hex(0x0D2137), 0);
  lv_obj_set_style_bg_opa(this->drawer_auto_brightness_button_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(this->drawer_auto_brightness_button_, lv_color_hex(COLOR_ACCENT), 0);
  lv_obj_set_style_border_width(this->drawer_auto_brightness_button_, 1, 0);
  lv_obj_set_style_border_color(this->drawer_auto_brightness_button_, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(this->drawer_auto_brightness_button_, 3, LV_STATE_FOCUSED);
  lv_obj_add_flag(this->drawer_auto_brightness_button_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(this->drawer_auto_brightness_button_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  this->drawer_auto_brightness_label_ = text_label(
      this->drawer_auto_brightness_button_,
      this->auto_brightness_enabled_ ? "AUTO-BRIGHTNESS: ON" : "AUTO-BRIGHTNESS: OFF", COLOR_TEXT);
  set_font(this->drawer_auto_brightness_label_, small_font());
  lv_obj_center(this->drawer_auto_brightness_label_);
  lv_obj_add_event_cb(this->drawer_auto_brightness_button_, store_drawer_event_cb_, LV_EVENT_ALL,
                      new DrawerContext{this, DRAWER_AUTO_BRIGHTNESS});  // NOLINT
  add_to_default_group(this->drawer_auto_brightness_button_);
  this->drawer_buttons_.push_back(this->drawer_auto_brightness_button_);
  this->drawer_actions_.push_back(DRAWER_AUTO_BRIGHTNESS);

  std::vector<std::pair<int, std::string>> buttons;
  if (source != nullptr) {
    for (size_t i = 0; i < source->actions.size(); i++)
      buttons.emplace_back(static_cast<int>(i), source->actions[i].first);
  }
  for (const auto &item : buttons) {
    lv_obj_t *button = plain_box(panel);
    lv_obj_set_size(button, inner, 52);
    lv_obj_set_pos(button, 0, y);
    y += 62;
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0EA5E9), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0284C7), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 3, LV_STATE_FOCUSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t *label = text_label(button, item.second, 0xFFFFFF);
    one_line(label, inner - 16);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, store_drawer_event_cb_, LV_EVENT_ALL, new DrawerContext{this, item.first});  // NOLINT
    add_to_default_group(button);
    this->drawer_buttons_.push_back(button);
    this->drawer_actions_.push_back(item.first);
  }
  lv_obj_t *hint = text_label(panel, "Select: open / close", COLOR_MUTED);
  set_font(hint, small_font());
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  // The tab: a strip down the left edge, all that shows while parked. Created
  // last so it is on top of the panel's edge.
  lv_obj_t *strip = plain_box(drawer);
  lv_obj_set_size(strip, DRAWER_TAB, lv_pct(100));
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_obj_add_event_cb(strip, store_drawer_event_cb_, LV_EVENT_ALL, new DrawerContext{this, DRAWER_TOGGLE});  // NOLINT
  add_to_default_group(strip);
  lv_obj_t *pill = plain_box(strip);
  lv_obj_set_size(pill, DRAWER_TAB, 96);
  lv_obj_align(pill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_radius(pill, 12, 0);
  lv_obj_set_style_bg_color(pill, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(pill, 1, 0);
  lv_obj_set_style_border_color(pill, lv_color_hex(COLOR_ACCENT), 0);
  lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *arrow = text_label(pill, this->side_menu_open_ ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT, COLOR_ACCENT);
  lv_obj_center(arrow);

  if (this->side_menu_open_)
    this->focus_drawer_button_(this->drawer_focus_);
}

void PaxLoader::ensure_catalog_drawer() {
  if (!this->store_ui_ || this->catalog_container_ == nullptr)
    return;
  if (this->drawer_ == nullptr)
    this->build_drawer_();
  if (this->drawer_ != nullptr) {
    lv_obj_clear_flag(this->drawer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(this->drawer_);
    lv_obj_set_x(this->drawer_, drawer_x(this->drawer_, this->side_menu_open_));
  }
}

void PaxLoader::focus_drawer_button_(uint8_t index) {
  if (this->drawer_buttons_.empty())
    return;
  index = std::min<uint8_t>(index, static_cast<uint8_t>(this->drawer_buttons_.size() - 1));
  for (lv_obj_t *button : this->drawer_buttons_)
    lv_obj_remove_state(button, LV_STATE_FOCUSED);
  lv_obj_add_state(this->drawer_buttons_[index], LV_STATE_FOCUSED);
  lv_group_focus_obj(this->drawer_buttons_[index]);
  this->drawer_focus_ = index;
}

void PaxLoader::run_drawer_action_(int action) {
  if (action == DRAWER_VOLUME || action == DRAWER_BRIGHTNESS) {
    lv_obj_t *slider = action == DRAWER_VOLUME ? this->drawer_volume_slider_ : this->drawer_brightness_slider_;
    if (slider != nullptr)
      lv_group_focus_obj(slider);
    return;
  }
  if (action == DRAWER_AUTO_BRIGHTNESS) {
    this->set_auto_brightness_enabled(!this->auto_brightness_enabled_);
    return;
  }
  if (action == DRAWER_REFRESH) {
    this->refresh_catalog();
    return;
  }
  if (this->catalogs_.empty())
    return;
  const auto &actions = this->catalogs_[this->catalog_index_].actions;
  if (action >= 0 && static_cast<size_t>(action) < actions.size() && actions[action].second != nullptr)
    actions[action].second->trigger();
}

void PaxLoader::release_launcher_runtime_() {
  if (this->lvgl_ == nullptr || !this->lvgl_->is_loop_started())
    return;

  const size_t catalog_count = this->catalog_entries_.size();
  const size_t icon_count = this->tile_icons_.size();
  const size_t rom_count = this->rom_selector_buttons_.size();

  // These are all launcher-created objects. The page roots and YAML-owned
  // controls stay alive; their dynamic children are recreated by the normal
  // update_*_ui_ paths after the game exits.
  if (this->detail_panel_ != nullptr) {
    lv_obj_add_flag(this->detail_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->detail_panel_);
    this->detail_panel_ = nullptr;
  }
  release_icon_(&this->detail_icon_);
  this->detail_buttons_.clear();
  this->detail_actions_.clear();
  this->detail_index_ = -1;
  this->detail_url_.clear();

  if (this->catalog_header_ != nullptr) {
    lv_obj_add_flag(this->catalog_header_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->catalog_header_);
    this->catalog_header_ = nullptr;
  }
  this->catalog_header_focus_pending_ = false;
  if (this->drawer_ != nullptr) {
    lv_obj_add_flag(this->drawer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->drawer_);
    this->drawer_ = nullptr;
  }
  this->drawer_volume_slider_ = nullptr;
  this->drawer_brightness_slider_ = nullptr;
  this->drawer_auto_brightness_button_ = nullptr;
  this->drawer_auto_brightness_label_ = nullptr;
  this->drawer_buttons_.clear();
  this->drawer_actions_.clear();

  if (this->catalog_container_ != nullptr)
    lv_obj_clean(this->catalog_container_);
  if (this->archive_container_ != nullptr)
    lv_obj_clean(this->archive_container_);
  if (this->rom_selector_container_ != nullptr)
    lv_obj_clean(this->rom_selector_container_);

  this->catalog_tiles_.clear();
  this->visible_catalog_indices_.clear();
  this->free_icons_();
  this->rom_selector_buttons_.clear();
  this->rom_selector_paths_.clear();
  this->rom_selector_selection_ = 0;

  // Background artwork is another launcher-only PSRAM allocation. Hide it
  // before dropping the shared pixels so LVGL cannot dereference the old
  // descriptor while the game is starting.
  if (this->background_image_ != nullptr)
    lv_obj_add_flag(this->background_image_, LV_OBJ_FLAG_HIDDEN);
  this->background_pixels_.reset();
  this->background_dsc_ = {};
  this->background_initialized_ = false;

  // The task itself owns its temporary HTTP buffers. If it has finished, it
  // is safe to delete it now; otherwise the running task will observe the
  // launched_ flag, finish its current request, and the normal loop will
  // reclaim it after the PAX exits.
  if (this->pax_catalog_task_handle_ != nullptr && this->catalog_done_) {
    vTaskDelete(this->pax_catalog_task_handle_);
    this->pax_catalog_task_handle_ = nullptr;
    this->catalog_loading_ = false;
    this->catalog_done_ = false;
  }
  if (this->info_task_handle_ != nullptr && this->info_done_) {
    vTaskDelete(this->info_task_handle_);
    this->info_task_handle_ = nullptr;
    this->info_loading_ = false;
    this->info_done_ = false;
  }
  if (this->archive_task_handle_ != nullptr && this->archive_done_) {
    vTaskDelete(this->archive_task_handle_);
    this->archive_task_handle_ = nullptr;
    this->archive_loading_ = false;
    this->archive_done_ = false;
  }

  this->catalog_entries_.clear();
  this->catalog_entries_.shrink_to_fit();
  this->app_info_.clear();
  this->app_info_.shrink_to_fit();
  this->info_result_.clear();
  this->info_result_.shrink_to_fit();
  this->info_urls_.clear();
  this->info_urls_.shrink_to_fit();
  this->installed_.clear();
  this->installed_.shrink_to_fit();
  this->archive_search_results_.clear();
  this->archive_search_results_.shrink_to_fit();
  this->archive_entries_.clear();
  this->archive_entries_.shrink_to_fit();
  if (!this->catalog_loading_)
    this->catalog_html_.clear();
  if (!this->archive_loading_)
    this->archive_html_.clear();

  // Keep the selected source/search settings, but make the next launcher
  // pass fetch a clean listing instead of trying to reuse freed indices.
  this->catalog_ui_pending_ = true;
  this->archive_ui_pending_ = true;
  this->info_deferred_ = false;
  this->catalog_deferred_ = !this->catalog_url_.empty();

  ESP_LOGI(TAG, "Released launcher runtime: catalog=%u icons=%u rom_rows=%u; game gets a clean LVGL store heap",
           static_cast<unsigned>(catalog_count), static_cast<unsigned>(icon_count), static_cast<unsigned>(rom_count));
}

#endif  // PAX_LOADER_USE_LVGL

void PaxLoader::set_side_menu(bool open) {
  this->side_menu_open_ = open;
#ifdef PAX_LOADER_USE_LVGL
  if (this->drawer_ == nullptr)
    return;
  slide_drawer(this->drawer_, drawer_x(this->drawer_, open));
  // The tab strip is the drawer's last child: strip -> pill -> arrow.
  lv_obj_t *strip = lv_obj_get_child(this->drawer_, -1);
  lv_obj_t *pill = strip != nullptr ? lv_obj_get_child(strip, 0) : nullptr;
  lv_obj_t *arrow = pill != nullptr ? lv_obj_get_child(pill, 0) : nullptr;
  if (arrow != nullptr)
    lv_label_set_text(arrow, open ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
  if (open) {
    this->focus_drawer_button_(0);
  } else {
    for (lv_obj_t *button : this->drawer_buttons_)
      lv_obj_remove_state(button, LV_STATE_FOCUSED);
  }
#endif
}

}  // namespace pax_loader
}  // namespace esphome
