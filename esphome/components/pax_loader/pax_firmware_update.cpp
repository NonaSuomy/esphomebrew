#include "pax_firmware_update.h"
#include "esphome/core/defines.h"
#include "pax_firmware_transfer.h"
#ifdef USE_ESP32_VARIANT_ESP32P4
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <psa/crypto.h>
#ifndef PAX_FIRMWARE_PROFILE
#define PAX_FIRMWARE_PROFILE ""
#endif
#ifndef PAX_FIRMWARE_BUILD_ID
#define PAX_FIRMWARE_BUILD_ID 0ULL
#endif
namespace pax_firmware {
static const char *const TAG = "pax_firmware";
static constexpr char BASE[] =
    "https://nonasuomy.github.io/esphomebrew/install/";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static pax_firmware_status_t status{PAX_FIRMWARE_MAGIC,
                                    PAX_FIRMWARE_IDLE,
                                    0,
                                    0,
                                    PAX_FIRMWARE_BUILD_ID,
                                    0,
                                    PAX_FIRMWARE_PROFILE,
                                    "Check GitHub for this device's firmware"};
struct Candidate {
  uint64_t build;
  uint32_t bytes;
  char url[192], hash[65];
};
static Candidate candidate{};
static int64_t reboot_at;
static void report(uint32_t state, const char *message, uint32_t done = 0,
                   uint32_t total = 0) {
  ESP_LOGD(TAG, "%s (%lu/%lu)", message, (unsigned long)done,
           (unsigned long)total);
  portENTER_CRITICAL(&lock);
  status.state = state;
  status.done = done;
  status.total = total;
  snprintf(status.message, sizeof(status.message), "%s", message);
  portEXIT_CRITICAL(&lock);
}
pax_firmware_status_t snapshot() {
  portENTER_CRITICAL(&lock);
  auto copy = status;
  portEXIT_CRITICAL(&lock);
  return copy;
}
struct Connection {
  esp_http_client_handle_t handle = nullptr;
  ~Connection() {
    if (handle) {
      esp_http_client_close(handle);
      esp_http_client_cleanup(handle);
    }
  }
  bool open(const char *url, uint32_t limit, int64_t &length) {
    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.timeout_ms = 20000;
    cfg.disable_auto_redirect = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.user_agent = "PAX-Firmware-Update/1";
    handle = esp_http_client_init(&cfg);
    if (handle)
      esp_http_client_set_header(handle, "Cache-Control", "no-cache");
    if (!handle) { ESP_LOGW(TAG,"HTTP client allocation failed"); return false; }
    esp_err_t opened=esp_http_client_open(handle,0);
    if (opened!=ESP_OK) { ESP_LOGW(TAG,"HTTP connection failed: %s",esp_err_to_name(opened)); return false; }
    length = esp_http_client_fetch_headers(handle);
    ESP_LOGI(TAG,"HTTP response %d, length %lld",esp_http_client_get_status_code(handle),static_cast<long long>(length));
    return esp_http_client_get_status_code(handle) == 200 && length > 0 &&
           length <= limit;
  }
};
// Match every partition before writing: OTA must never require moving NVS.
static bool partitions_match(JsonArray array) {
  if (array.isNull() || array.size() == 0)
    return false;
  size_t count = 0;
  auto iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                     ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (iterator) {
    count++;
    iterator = esp_partition_next(iterator);
  }
  if (count != array.size()) {ESP_LOGW(TAG,"Partition count %u != %u",unsigned(count),unsigned(array.size()));return false;}
  for (JsonObject entry : array) {
    const char *label = entry["label"] | "";
    unsigned matches = 0;
    for (JsonObject other : array)
      if (!strcmp(label, other["label"] | ""))
        matches++;
    if (!label[0] || matches != 1)
      return false;
    auto p = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                      ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p || p->address != entry["address"].as<uint32_t>() ||
        p->size != entry["size"].as<uint32_t>() ||
        p->type != entry["type"].as<unsigned>() ||
        p->subtype != entry["subtype"].as<unsigned>())
      return false;
  }
  return true;
}
static bool check(Candidate &out) {
  char url[192];
  snprintf(url, sizeof(url), "%supdates/%s.json?check=%lld", BASE,
           PAX_FIRMWARE_PROFILE, static_cast<long long>(esp_timer_get_time()));
  Connection connection;
  int64_t length;
  if (!connection.open(url, 8192, length))
    return false;
  std::string body;
  body.resize(length);
  size_t offset = 0;
  while (offset < body.size()) {
    int n = esp_http_client_read(connection.handle, &body[offset],
                                 body.size() - offset);
    if (n <= 0)
      return false;
    offset += n;
    vTaskDelay(1);
  }
  char expected_url[192];
  snprintf(expected_url, sizeof(expected_url), "%sota/pax-%s.ota.bin", BASE,
           PAX_FIRMWARE_PROFILE);
  bool parsed = esphome::json::parse_json(body, [&](JsonObject json) {
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    const char *profile = json["profile"] | "", *chip_name = json["chip"] | "";
    const char *download = json["url"] | "", *hash = json["sha256"] | "";
    uint64_t build = json["build_id"].as<uint64_t>();
    auto partition = esp_ota_get_next_update_partition(nullptr);
    uint32_t bytes = json["bytes"].as<uint32_t>();
    if (json["format"].as<int>() != 1 ||
        strcmp(profile, PAX_FIRMWARE_PROFILE) ||
        strcmp(chip_name, "ESP32-P4") || !build ||
        strcmp(download, expected_url) || strlen(hash) != 64 || !partition ||
        !bytes || bytes > partition->size ||
        !json["min_revision"].is<unsigned>() ||
        !json["max_revision"].is<unsigned>() ||
        chip.revision < json["min_revision"].as<unsigned>() ||
        chip.revision > json["max_revision"].as<unsigned>() ||
        !partitions_match(json["partitions"].as<JsonArray>())) {
      ESP_LOGW(TAG,"Manifest rejected: profile=%s chip=%s build=%llu rev=%u min=%u max=%u bytes=%u slot=%u",profile,chip_name,(unsigned long long)build,chip.revision,json["min_revision"].as<unsigned>(),json["max_revision"].as<unsigned>(),unsigned(bytes),unsigned(partition ? partition->size : 0));
      return false;
    }
    for (size_t i = 0; i < 64; i++)
      if (!((hash[i] >= '0' && hash[i] <= '9') ||
            (hash[i] >= 'a' && hash[i] <= 'f')))
        return false;
    out.build = build;
    out.bytes = bytes;
    snprintf(out.url, sizeof(out.url), "%s", download);
    snprintf(out.hash, sizeof(out.hash), "%s", hash);
    return true;
  });
  if(!parsed)ESP_LOGW(TAG,"Manifest parse or compatibility check failed (%u bytes)",unsigned(body.size()));
  return parsed;
}
static bool install(const Candidate &image) {
  Connection connection;
  int64_t length;
  char download[224];
  snprintf(download, sizeof(download), "%s?build=%llu", image.url,
           static_cast<unsigned long long>(image.build));
  if (!connection.open(download, image.bytes, length) || length != image.bytes)
    return false;
  auto partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition || image.bytes > partition->size)
    return false;
  struct Source {
    esp_http_client_handle_t client;
    int read(uint8_t *buffer, size_t size) {
      return esp_http_client_read(client, reinterpret_cast<char *>(buffer),
                                  size);
    }
  } source{connection.handle};
  struct Target {
    const esp_partition_t *partition;
    esp_ota_handle_t handle = 0;
    int64_t last_report = 0;
    bool begin(uint32_t size) {
      return esp_ota_begin(partition, size, &handle) == ESP_OK;
    }
    bool write(uint8_t *buffer, size_t size) {
      return esp_ota_write(handle, buffer, size) == ESP_OK;
    }
    void abort() { esp_ota_abort(handle); }
    bool end() { return esp_ota_end(handle) == ESP_OK; }
    bool activate() { return esp_ota_set_boot_partition(partition) == ESP_OK; }
    void progress(uint32_t done, uint32_t total) {
      int64_t now = esp_timer_get_time();
      if (now - last_report > 250000 || done == total) {
        report(PAX_FIRMWARE_INSTALLING,
               "Installing firmware; keep power connected", done, total);
        last_report = now;
      }
      vTaskDelay(1);
    }
  } target{partition};
  struct Digest {
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    ~Digest() { psa_hash_abort(&hash); }
    bool begin() {
      return psa_crypto_init() == PSA_SUCCESS &&
             psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS;
    }
    bool update(uint8_t *buffer, size_t size) {
      return psa_hash_update(&hash, buffer, size) == PSA_SUCCESS;
    }
    bool matches(const char *expected) {
      uint8_t bytes[32];
      size_t size = 0;
      char hex[65]{};
      if (psa_hash_finish(&hash, bytes, sizeof(bytes), &size) != PSA_SUCCESS ||
          size != 32)
        return false;
      for (size_t i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", bytes[i]);
      return !strcmp(hex, expected);
    }
  } digest;
  return transfer(source, target, digest, image.bytes, image.hash);
}

static void check_worker(void *) {
  Candidate next{};
  if (!check(next))
    report(PAX_FIRMWARE_ERROR, "Update unavailable: check network, clock, "
                               "profile and partition compatibility");
  else {
    portENTER_CRITICAL(&lock);
    candidate = next;
    status.latest_build = next.build;
    portEXIT_CRITICAL(&lock);
    char message[192];
    bool newer = next.build > PAX_FIRMWARE_BUILD_ID;
    snprintf(message, sizeof(message),
             newer ? "Build %llu available. Settings and SD files will be kept."
                   : "Already up to date (build %llu)",
             (unsigned long long)next.build);
    report(newer ? PAX_FIRMWARE_AVAILABLE : PAX_FIRMWARE_CURRENT, message);
  }
  vTaskDelete(nullptr);
}
static void install_worker(void *) {
  Candidate image;
  portENTER_CRITICAL(&lock);
  image = candidate;
  portEXIT_CRITICAL(&lock);
  if (!install(image))
    report(PAX_FIRMWARE_ERROR, "Update failed; previous firmware and settings "
                               "retained. Check again to retry.");
  else {
    report(PAX_FIRMWARE_REBOOTING, "Update verified; restarting", image.bytes,
           image.bytes);
    portENTER_CRITICAL(&lock);
    reboot_at = esp_timer_get_time() + 1500000;
    portEXIT_CRITICAL(&lock);
  }
  vTaskDelete(nullptr);
}
struct Stream {
  bool writing;
  size_t offset;
  pax_firmware_request_t request;
  pax_firmware_status_t response;
};
static ssize_t read(void *cookie, void *out, size_t size) {
  auto s = static_cast<Stream *>(cookie);
  size = std::min(size, sizeof(s->response) - s->offset);
  memcpy(out, reinterpret_cast<char *>(&s->response) + s->offset, size);
  s->offset += size;
  return size;
}
static ssize_t write(void *cookie, const void *in, size_t size) {
  auto s = static_cast<Stream *>(cookie);
  if (size > sizeof(s->request) - s->offset)
    return -1;
  memcpy(reinterpret_cast<char *>(&s->request) + s->offset, in, size);
  s->offset += size;
  return size;
}
static int close(void *cookie) {
  auto s = static_cast<Stream *>(cookie);
  int result = 0;
  if (s->writing) {
    auto &r = s->request;
    bool valid =
        s->offset == sizeof(r) && r.magic == PAX_FIRMWARE_MAGIC &&
        PAX_FIRMWARE_PROFILE[0] &&
        (r.command == PAX_FIRMWARE_CHECK || r.command == PAX_FIRMWARE_INSTALL);
    portENTER_CRITICAL(&lock);
    bool idle = status.state != PAX_FIRMWARE_CHECKING &&
                status.state != PAX_FIRMWARE_WAITING &&
                status.state != PAX_FIRMWARE_INSTALLING &&
                status.state != PAX_FIRMWARE_REBOOTING;
    bool allowed = valid && idle &&
                   (r.command != PAX_FIRMWARE_INSTALL ||
                    (status.state == PAX_FIRMWARE_AVAILABLE &&
                     r.expected_build == candidate.build));
    if (allowed)
      status.state = r.command == PAX_FIRMWARE_CHECK ? PAX_FIRMWARE_CHECKING
                                                     : PAX_FIRMWARE_WAITING;
    portEXIT_CRITICAL(&lock);
    if (!allowed)
      result = -1;
    else if (r.command == PAX_FIRMWARE_CHECK) {
      report(PAX_FIRMWARE_CHECKING, "Checking GitHub for this device...");
      if (xTaskCreate(check_worker, "pax_fw_check", 12288, nullptr, 3,
                      nullptr) != pdPASS) {
        report(PAX_FIRMWARE_ERROR, "Not enough memory to check for updates");
        result = -1;
      }
    } else
      report(PAX_FIRMWARE_WAITING, "Closing launcher and saving settings...");
  }
  free(s);
  return result;
}
FILE *open(const char *mode) {
  if (!mode || (strcmp(mode, "rb") && strcmp(mode, "wb")))
    return nullptr;
  auto s = static_cast<Stream *>(calloc(1, sizeof(Stream)));
  if (!s)
    return nullptr;
  s->writing = mode[0] == 'w';
  s->response = snapshot();
  if (!PAX_FIRMWARE_PROFILE[0])
    snprintf(s->response.message, sizeof(s->response.message),
             "Custom firmware: no matching public update profile");
  FILE *f = funopen(s, s->writing ? nullptr : read,
                    s->writing ? write : nullptr, nullptr, close);
  if (!f) {
    free(s);
    return nullptr;
  }
  setvbuf(f, nullptr, _IONBF, 0);
  return f;
}
bool pending() { return snapshot().state == PAX_FIRMWARE_WAITING; }
bool busy() {
  auto state = snapshot().state;
  return state == PAX_FIRMWARE_CHECKING ||
         (state >= PAX_FIRMWARE_WAITING && state <= PAX_FIRMWARE_REBOOTING);
}
void loop(bool app_closed) {
  if (pending() && app_closed) {
    if (!esphome::global_preferences || !esphome::global_preferences->sync()) {
      report(PAX_FIRMWARE_ERROR, "Could not save pending settings; update cancelled");
      return;
    }
    report(PAX_FIRMWARE_INSTALLING, "Starting firmware download...");
    if (xTaskCreate(install_worker, "pax_fw_install", 16384, nullptr, 3,
                    nullptr) != pdPASS)
      report(PAX_FIRMWARE_ERROR, "Not enough memory to install update");
  }
  portENTER_CRITICAL(&lock);
  int64_t deadline = reboot_at;
  portEXIT_CRITICAL(&lock);
  if (deadline && esp_timer_get_time() >= deadline)
    esphome::App.safe_reboot();
}
} // namespace pax_firmware
#else
namespace pax_firmware {
FILE *open(const char *) { return nullptr; }
bool pending() { return false; }
bool busy() { return false; }
void loop(bool) {}
pax_firmware_status_t snapshot() { return {}; }
} // namespace pax_firmware
#endif
