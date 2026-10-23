#include "pax_web.h"
#include "esphome/components/pax_loader/pax_firmware_update.h"
#include "esphome/components/pax_loader/pax_c6_update.h"
#include "esphome/components/json/json_util.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esphome/components/pax_loader/pax_data.h"
#ifdef USE_IMPROV_SERIAL
#include "esphome/components/improv_serial/improv_serial_component.h"
#endif
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "freertos/idf_additions.h"
#include "pax_web_assets.h"
#include "pax_web_starter.h"
#include "pax_web_host.h"
#include <lwip/sockets.h>
#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace esphome::pax_web {
static const char *const TAG = "pax_web";
static std::string header(httpd_req_t *req, const char *name, size_t max) {
  const size_t size = httpd_req_get_hdr_value_len(req, name);
  if (size == 0 || size > max)
    return {};
  std::string value(size + 1, '\0');
  if (httpd_req_get_hdr_value_str(req, name, value.data(), value.size()) !=
      ESP_OK)
    return {};
  value.resize(size);
  return value;
}
void PaxWeb::setup() {
  granted_ = xSemaphoreCreateBinary();
  status_mutex_ = xSemaphoreCreateMutex();
  if (granted_ == nullptr || status_mutex_ == nullptr) {
    mark_failed();
    return;
  }
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  for (size_t i = 0; i < sizeof(random); i++)
    std::snprintf(token_ + i * 2, 3, "%02x", random[i]);
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port_;
  config.ctrl_port = 32769;
  config.stack_size = 12288;
  config.max_uri_handlers = 14;
  config.max_open_sockets = 3;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 20;
  config.send_wait_timeout = 20;
  if (httpd_start(&server_, &config) != ESP_OK) {
    mark_failed();
    return;
  }
  struct Route {
    const char *path;
    httpd_method_t method;
  };
  const Route routes[] = {{"/", HTTP_GET},
                          {"/pax", HTTP_GET},
                          {"/pax.js", HTTP_GET},
                          {"/pax-store.js", HTTP_GET},
                          {"/pax/status", HTTP_GET},
                          {"/pax/firmware", HTTP_GET},
                          {"/pax/c6", HTTP_GET},
                          {"/pax/c6/check", HTTP_POST},
                          {"/pax/c6/install", HTTP_POST},
                          {"/pax/firmware/check", HTTP_POST},
                          {"/pax/firmware/install", HTTP_POST},
                          {"/pax/starter", HTTP_POST},
                          {"/pax/upload", HTTP_POST},
                          {"/pax/close", HTTP_POST}};
  for (const auto &route : routes) {
    httpd_uri_t uri{};
    uri.uri = route.path;
    uri.method = route.method;
    uri.handler = handle_;
    uri.user_ctx = this;
    if (httpd_register_uri_handler(server_, &uri) != ESP_OK) {
      mark_failed();
      httpd_stop(server_);
      server_ = nullptr;
      return;
    }
  }
#ifdef USE_IMPROV_SERIAL
  serial_queue_ = xQueueCreate(4, sizeof(SerialChunk));
#endif
  ESP_LOGI(TAG, "SD setup and file upload available at http://<device-ip>:8080/pax");
}
void PaxWeb::dump_config() {
  ESP_LOGCONFIG(TAG, "PAX SD setup: port %u, path /pax", port_);
}
void PaxWeb::loop() {
  if (snapshot_state_.load() >= 2 && uint32_t(millis() - snapshot_access_) > 120000) {
    heap_caps_free(snapshot_pixels_); snapshot_pixels_ = nullptr; snapshot_state_ = 0;
  }
#ifdef USE_IMPROV_SERIAL
  if (!serial_bound_ && serial_queue_ != nullptr && improv_serial::global_improv_serial_component != nullptr) {
    improv_serial::global_improv_serial_component->set_vendor_handler(
        [this](uint8_t cmd, std::span<const uint8_t> payload) { return serial_command_(cmd, payload); });
    serial_bound_ = true;
  }
#endif
  if (close_requested_.exchange(false))
    loader_->request_close();
  if (transfer_.load() == 1) {
    const bool accepted = loader_->begin_web_transfer();
    int expected = 1;
    if (!transfer_.compare_exchange_strong(expected, accepted ? 2 : 3) &&
        accepted)
      loader_->end_web_transfer();
    xSemaphoreGive(granted_);
  }
  if (transfer_.load() == 4) {
    loader_->end_web_transfer();
    transfer_ = 0;
  }
}
bool PaxWeb::reserve_() {
  // Only the HTTP server task requests reservations.
  while (xSemaphoreTake(granted_, 0) == pdTRUE) {
  }
  // A successful HTTP reply can reach the next request before the main loop
  // releases storage ownership. Wait for that completed transfer, not an
  // active upload, so consecutive files do not spuriously fail as busy.
  const uint32_t started = millis();
  while (transfer_.load() == 4 && uint32_t(millis() - started) < 1000)
    vTaskDelay(pdMS_TO_TICKS(1));
  int expected = 0;
  if (!transfer_.compare_exchange_strong(expected, 1))
    return false;
  if (xSemaphoreTake(granted_, pdMS_TO_TICKS(5000)) != pdTRUE) {
    expected = 1;
    if (!transfer_.compare_exchange_strong(expected, 0) && expected == 2)
      transfer_ = 4;
    else if (expected == 3)
      transfer_ = 0;
    return false;
  }
  if (transfer_.load() == 2)
    return true;
  transfer_ = 0;
  return false;
}
void PaxWeb::report_(const char *message) {
  xSemaphoreTake(status_mutex_, portMAX_DELAY);
  std::snprintf(status_, sizeof(status_), "%s", message);
  xSemaphoreGive(status_mutex_);
}
bool PaxWeb::allowed_host_(httpd_req_t *req) {
  const std::string host = header(req, "Host", 128);
  const std::string name = App.get_name() + ".local";
  if (host_matches(host, name, port_))
    return true;
  // network::get_ip_addresses() describes the preferred interface only.
  // On dual-network boards the browser can reach us through another one.
  // Trust the local endpoint of this accepted socket, never a client header.
  sockaddr_storage local{};
  socklen_t length = sizeof local;
  if (::getsockname(httpd_req_to_sockfd(req),
                    reinterpret_cast<sockaddr *>(&local), &length) == 0) {
    char address[network::IP_ADDRESS_BUFFER_SIZE];
    bool ipv6 = false;
    const void *raw = nullptr;
    if (local.ss_family == AF_INET)
      raw = &reinterpret_cast<const sockaddr_in *>(&local)->sin_addr;
#if LWIP_IPV6
    else if (local.ss_family == AF_INET6) {
      raw = &reinterpret_cast<const sockaddr_in6 *>(&local)->sin6_addr;
      ipv6 = true;
    }
#endif
    if (raw && ::inet_ntop(local.ss_family, raw, address, sizeof address)) {
      const std::string value = ipv6
                                    ? "[" + std::string(address) + "]"
                                    : std::string(address);
      if (host_matches(host, value, port_))
        return true;
    }
  }
  for (const auto &ip : network::get_ip_addresses()) {
    if (!ip.is_set())
      continue;
    char text[network::IP_ADDRESS_BUFFER_SIZE];
    ip.str_to(text);
    const std::string value =
        ip.is_ip6() ? "[" + std::string(text) + "]" : std::string(text);
    if (host_matches(host, value, port_))
      return true;
  }
  return false;
}
bool PaxWeb::authorized_(httpd_req_t *req) {
  if (!allowed_host_(req) || header(req, "X-PAX-Token", 32) != token_)
    return false;
  const std::string origin = header(req, "Origin", 160);
  return origin.empty() || origin == "http://" + header(req, "Host", 128);
}
esp_err_t PaxWeb::reply_(httpd_req_t *req, const char *status,
                         const char *message) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const bool close = req->method == HTTP_POST && req->content_len > 0 &&
                     std::strcmp(status, "200 OK") != 0;
  if (close)
    httpd_resp_set_hdr(req, "Connection", "close");
  const esp_err_t result = httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
  return close ? ESP_FAIL : result;
}
esp_err_t PaxWeb::handle_(httpd_req_t *req) {
  return static_cast<PaxWeb *>(req->user_ctx)->serve_(req);
}
int PaxWeb::read_(void *context, uint8_t *buffer, size_t size) {
  return httpd_req_recv(static_cast<httpd_req_t *>(context),
                        reinterpret_cast<char *>(buffer), size);
}
esp_err_t PaxWeb::serve_(httpd_req_t *req) {
  if (!allowed_host_(req))
    return reply_(req, "403 Forbidden",
                  "Open this page using the device IP address or .local name");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
  httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
  if (req->method == HTTP_GET) {
    if (std::strcmp(req->uri, "/pax-store.js") == 0) {
      httpd_resp_set_type(req, "text/javascript; charset=utf-8");
      return httpd_resp_send(req, STORE_JS, sizeof(STORE_JS) - 1);
    }
    if (std::strcmp(req->uri, "/pax.js") == 0) {
      httpd_resp_set_type(req, "text/javascript; charset=utf-8");
      return httpd_resp_send(req, WEB_JS, sizeof(WEB_JS) - 1);
    }
    if (std::strcmp(req->uri, "/pax/status") == 0) {
      char text[192];
      xSemaphoreTake(status_mutex_, portMAX_DELAY);
      std::memcpy(text, status_, sizeof(text));
      xSemaphoreGive(status_mutex_);
      std::string message(text);
      if (message.rfind("Downloading", 0) == 0 && transfer_.load() == 2) {
        message += ": " + loader_->get_load_status();
        const float progress = loader_->get_load_progress();
        if (progress >= 0) {
          char value[16];
          std::snprintf(value, sizeof(value), "%.1f", progress * 100);
          httpd_resp_set_hdr(req, "X-PAX-Progress", value);
        }
      }
      return reply_(req, "200 OK", message.c_str());
    }
    if(!std::strcmp(req->uri,"/pax/c6")){
      auto info=pax_c6::snapshot();JsonDocument document;
      document["state"]=info.status.state;document["done"]=info.status.done;document["total"]=info.status.total;
      document["message"]=info.status.message;document["profile"]=info.profile;
      document["installed_version"]=info.installed_version;document["recommended_version"]=info.recommended_version;
      std::string body;serializeJson(document,body);httpd_resp_set_type(req,"application/json");
      return httpd_resp_send(req,body.data(),body.size());
    }
    if (std::strcmp(req->uri,"/pax/firmware")==0) {
      auto status=pax_firmware::snapshot();
      JsonDocument document;document["state"]=status.state;document["done"]=status.done;document["total"]=status.total;
      // Decimal strings keep 64-bit build IDs intact in browsers.
      document["installed_build"]=std::to_string(status.installed_build);document["latest_build"]=std::to_string(status.latest_build);
      document["profile"]=status.profile;document["message"]=status.message;
      document["volume_percent"]=loader_->get_master_volume();document["auto_brightness"]=loader_->auto_brightness_enabled();
      std::string body;serializeJson(document,body);httpd_resp_set_type(req,"application/json");
      return httpd_resp_send(req,body.data(),body.size());
    }
    std::string page(WEB_HTML);
    const auto pos = page.find("PAX_SESSION_TOKEN");
    if (pos != std::string::npos)
      page.replace(pos, 17, token_);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page.data(), page.size());
  }
  if (!authorized_(req))
    return reply_(req, "403 Forbidden",
                  "Reload the device page before transferring files");
  if(!std::strcmp(req->uri,"/pax/c6/check") || !std::strcmp(req->uri,"/pax/c6/install")){
    if(req->content_len)return reply_(req,"400 Bad Request","C6 request must have no body");
    bool install=!std::strcmp(req->uri,"/pax/c6/install");auto info=pax_c6::snapshot();
    if(install && (info.status.state!=PAX_C6_READY || header(req,"X-PAX-C6",48)!=info.profile))
      return reply_(req,"409 Conflict","Check matching C6 firmware and confirm its profile first");
    if(transfer_.load() || !loader_->request_c6_update(install ? PAX_C6_INSTALL : PAX_C6_PREPARE))
      return reply_(req,"409 Conflict","C6 update unavailable or device busy");
    return reply_(req,"202 Accepted",install ? "Installing matching C6 firmware" : "Checking matching C6 firmware");
  }
  if (!std::strcmp(req->uri,"/pax/firmware/check") || !std::strcmp(req->uri,"/pax/firmware/install")) {
    if(req->content_len!=0)return reply_(req,"400 Bad Request","Firmware request must have no body");
    bool install=!std::strcmp(req->uri,"/pax/firmware/install");
    uint64_t expected=0;
    if(install){
      std::string value=header(req,"X-PAX-Build",24);
      if(value.empty() || value.size()>20 || value.find_first_not_of("0123456789")!=std::string::npos)
        return reply_(req,"400 Bad Request","Confirm the checked firmware build first");
      char *end=nullptr;errno=0;expected=strtoull(value.c_str(),&end,10);
      if(errno || !end || *end)return reply_(req,"400 Bad Request","Invalid build ID");
    }
    if(transfer_.load()!=0 || !loader_->request_firmware_update(install ? PAX_FIRMWARE_INSTALL : PAX_FIRMWARE_CHECK,expected))
      return reply_(req,"409 Conflict","Device busy or firmware confirmation expired; check again");
    return reply_(req,"202 Accepted",install ? "Firmware update accepted; settings and SD files will be kept" : "Checking GitHub for firmware");
  }
  if (std::strcmp(req->uri, "/pax/close") == 0) {
    close_requested_ = true;
    return reply_(
        req, "202 Accepted",
        "Closing app. Wait for the three-button screen before transferring.");
  }
  if (std::strcmp(req->uri, "/pax/starter") == 0) {
    if (req->content_len != 0)
      return reply_(req, "400 Bad Request",
                    "Starter request must have no body");
    if (!reserve_())
      return reply_(
          req, "409 Conflict",
          "Close the running app and wait for other transfers to finish");
    report_("Downloading verified SD starter files");
    if (xTaskCreatePinnedToCoreWithCaps(
            starter_task_, "pax_starter", 20480, this, 4, &starter_task_handle_,
            0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
      transfer_ = 4;
      report_("Could not start download");
      return reply_(req, "503 Service Unavailable", "Could not start download");
    }
    return reply_(req, "202 Accepted", "Downloading verified SD starter files");
  }
  const std::string path = header(req, "X-PAX-Path", 204);
  const std::string digest = header(req, "X-PAX-SHA256", 64);
  if (path.rfind("/sd/", 0) != 0 ||
      !pax_loader::data::safe_target(path.substr(4)) ||
      !pax_loader::data::is_sha256_hex(digest) ||
      req->content_len > pax_loader::data::MAX_FILE_SIZE)
    return reply_(req, "400 Bad Request",
                  "Invalid SD path, file size or SHA-256");
  if (!reserve_())
    return reply_(
        req, "409 Conflict",
        "Close the running app and wait for other transfers to finish");
  report_("Receiving file");
  const esp_err_t result =
      loader_->receive_web_file(path, req->content_len, digest, read_, req);
  transfer_ = 4;
  char message[192];
  std::snprintf(message, sizeof(message), "%s: %.100s (%s)",
                result == ESP_OK ? "Saved and verified" : "Upload failed",
                path.c_str(), esp_err_to_name(result));
  report_(message);
  const esp_err_t sent = reply_(
      req, result == ESP_OK ? "200 OK" : "500 Internal Server Error", message);
  // An incomplete request body must never become the next request on this
  // connection.
  return result == ESP_OK ? sent : ESP_FAIL;
}
void PaxWeb::starter_task_(void *context) {
  auto *self = static_cast<PaxWeb *>(context);
  esp_err_t result = ESP_OK;
  for (const auto &file : STARTER_FILES) {
    char message[192];
    std::snprintf(message, sizeof(message), "Downloading %s", file.target);
    self->report_(message);
    pax_loader::data::DataFile entry;
    entry.target = file.target;
    entry.size = file.size;
    entry.sha256 = file.sha256;
    entry.url = file.url;
    result = self->loader_->download_web_file(entry);
    if (result != ESP_OK)
      break;
  }
  char message[192];
  std::snprintf(
      message, sizeof(message), "%s (%s)",
      result == ESP_OK
          ? "SD starter installed and verified. Choose STORAGE on the device."
          : "Starter download failed. Check the SD card and network, then "
            "retry.",
      esp_err_to_name(result));
  self->report_(message);
  self->transfer_ = 4;
  self->starter_task_handle_ = nullptr;
  vTaskDeleteWithCaps(nullptr);
}
// Improv vendor 0x74 extensions: op + session ID, plus offset for reads.
// Polling keeps logs out of the image and bounds main-loop UART work.
void PaxWeb::snapshot_task_(void *context) {
  auto *self = static_cast<PaxWeb *>(context);
  bool ok = self->loader_->capture_screenshot(self->snapshot_pixels_, self->snapshot_width_, self->snapshot_height_);
  if (ok) {
    pax_loader::data::Sha256 hash;
    size_t size = size_t(self->snapshot_width_) * self->snapshot_height_ * 2;
    const auto *pixels = reinterpret_cast<const uint8_t *>(self->snapshot_pixels_);
    for (size_t offset=0;offset<size;offset+=65536) {
      hash.update(pixels+offset,std::min<size_t>(65536,size-offset));vTaskDelay(1);
    }
    const auto hex = hash.hex();
    auto digit = [](char c) { return c <= '9' ? c-'0' : c-'a'+10; };
    for (unsigned i=0;i<32;i++)self->snapshot_sha_[i] = (digit(hex[i*2])<<4) | digit(hex[i*2+1]);
  }
  self->snapshot_state_ = ok ? 2 : 3;
  vTaskDeleteWithCaps(nullptr);
}
std::vector<uint8_t> PaxWeb::snapshot_command_(std::span<const uint8_t> payload) {
  auto error = [](const char *message) {
    std::vector<uint8_t> out{0x74, 1};out.insert(out.end(), message, message + std::strlen(message));return out;
  };
  if (payload.size() != 5 && payload.size() != 9) return error("Invalid screenshot request");
  const uint8_t op = payload[0];
  uint32_t id = 0;for (unsigned i=0;i<4;i++) id |= uint32_t(payload[i+1]) << (i*8);
  if (op < 1 || op > 4 || payload.size() != (op == 3 ? 9U : 5U)) return error("Invalid screenshot operation");
  int state = snapshot_state_.load();
  if (op == 1 && (state == 0 || (state != 1 && id != snapshot_id_))) {
    heap_caps_free(snapshot_pixels_);snapshot_pixels_ = nullptr;
    snapshot_id_ = id;snapshot_state_ = 1;snapshot_access_ = millis();
    if (xTaskCreatePinnedToCoreWithCaps(snapshot_task_, "pax_snapshot", 8192, this, 4, nullptr,
        0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) snapshot_state_ = 3;
    state = snapshot_state_.load();
  }
  if (id != snapshot_id_ || state == 0) return error("Screenshot session unavailable");
  snapshot_access_ = millis();
  if (op == 4 && state != 1) {
    heap_caps_free(snapshot_pixels_);snapshot_pixels_=nullptr;snapshot_state_=0;
    std::vector<uint8_t> out{0x74,0,op};for(unsigned i=0;i<4;i++)out.push_back(id>>(8*i));return out;
  }
  if (state == 3) return error("Screenshot unavailable: no canvas, display busy, or out of memory");
  std::vector<uint8_t> out{0x74, uint8_t(state == 1 && op != 1 ? 2 : 0), op};
  auto append = [&out](uint32_t n, unsigned bytes) {for(unsigned i=0;i<bytes;i++)out.push_back(n>>(8*i));};
  append(id,4);
  if (op == 1 || state == 1) return out;
  const uint32_t size = uint32_t(snapshot_width_) * snapshot_height_ * 2;
  if (op == 2) {
    append(snapshot_width_,2);append(snapshot_height_,2);append(size,4);
    out.insert(out.end(),snapshot_sha_,snapshot_sha_+32);
  } else if (op == 3) {
    uint32_t offset=0;for(unsigned i=0;i<4;i++)offset |= uint32_t(payload[i+5])<<(8*i);
    if(offset>=size)return error("Screenshot offset out of range");
    append(offset,4);const auto *pixels=reinterpret_cast<const uint8_t *>(snapshot_pixels_);
    out.insert(out.end(),pixels+offset,pixels+offset+std::min<uint32_t>(240,size-offset));
  }
  return out;
}
// Executed on the main loop via Improv, so storage ownership changes here.
std::vector<uint8_t> PaxWeb::serial_command_(uint8_t command, std::span<const uint8_t> payload) {
  auto reply = [command](uint8_t status, const char *text) {
    std::vector<uint8_t> out{command, status};
    out.insert(out.end(), text, text + std::min<size_t>(std::strlen(text), 180));
    return out;
  };
  if (command == 0x74) {
    if (!payload.empty()) return snapshot_command_(payload);
    return reply(0, "PAXSD1");
  }
  if (command == 0x75) { close_requested_ = true; return reply(0, "Closing app"); }
  if (command == 0x73) { serial_abort_ = true; return reply(0, "Cancelling transfer"); }
  if (command == 0x72) {
    const int result = serial_result_.load();
    if (result == 1 || transfer_.load() == 4) return reply(2, "Receiving or saving file");
    return reply(result == 2 ? 0 : 1, result == 2 ? "Saved and verified" : "Transfer failed; previous file kept");
  }
  if (command == 0x70) {
    if (payload.size() < 37 || payload.size() > 240) return reply(1, "Invalid file header");
    uint32_t size = 0;
    for (size_t i = 0; i < 4; i++) size |= uint32_t(payload[i]) << (8 * i);
    std::string path(reinterpret_cast<const char *>(payload.data() + 36), payload.size() - 36);
    if (size > pax_loader::data::MAX_FILE_SIZE || path.rfind("/sd/", 0) != 0 ||
        !pax_loader::data::safe_target(path.substr(4))) return reply(1, "Invalid SD path or file size");
    int expected = 0;
    if (!transfer_.compare_exchange_strong(expected, 2)) return reply(1, "Another transfer is busy");
    if (!loader_->begin_web_transfer()) { transfer_ = 0; return reply(1, "Close the running app before installing"); }
    serial_path_ = path;
    serial_size_ = size;
    serial_received_ = serial_sequence_ = serial_offset_ = 0;
    serial_chunk_.length = 0;
    serial_sha_.clear();
    for (size_t i = 4; i < 36; i++) { char hex[3]; std::snprintf(hex, 3, "%02x", payload[i]); serial_sha_ += hex; }
    xQueueReset(serial_queue_);
    serial_abort_ = false;
    serial_result_ = 1;
    if (xTaskCreatePinnedToCoreWithCaps(serial_task_, "pax_serial", 12288, this, 4, nullptr,
                                      0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
      serial_result_ = 3; transfer_ = 4; return reply(1, "Could not start transfer");
    }
    return reply(0, "Ready");
  }
  if (command == 0x71) {
    if (payload.size() <= 4 || payload.size() > 244)
      return reply(1, "Invalid chunk");
    uint32_t sequence = 0;
    for (size_t i = 0; i < 4; i++) sequence |= uint32_t(payload[i]) << (8 * i);
    const std::string accepted = "Accepted " + std::to_string(sequence);
    if (!serial_abort_.load() && serial_sequence_ > 0 && sequence == serial_sequence_ - 1 &&
        (serial_result_.load() == 1 || serial_result_.load() == 2)) return reply(0, accepted.c_str());
    if (serial_result_.load() != 1) return reply(1, "No active transfer");
    if (sequence != serial_sequence_ || payload.size() - 4 > serial_size_ - serial_received_)
      return reply(1, "Out of sequence or oversized chunk");
    SerialChunk chunk{};
    chunk.length = payload.size() - 4;
    std::memcpy(chunk.data, payload.data() + 4, chunk.length);
    if (xQueueSend(serial_queue_, &chunk, 0) != pdTRUE) {
      const std::string retry = "Retry " + std::to_string(sequence);
      return reply(2, retry.c_str());
    }
    serial_received_ += chunk.length;
    serial_sequence_++;
    return reply(0, accepted.c_str());
  }
  return reply(1, "Unknown command");
}
int PaxWeb::serial_read_(void *context, uint8_t *buffer, size_t size) {
  auto *self = static_cast<PaxWeb *>(context);
  if (self->serial_abort_.load()) return -1;
  if (self->serial_offset_ == self->serial_chunk_.length) {
    unsigned waits = 0;
    while (xQueueReceive(self->serial_queue_, &self->serial_chunk_, pdMS_TO_TICKS(250)) != pdTRUE) {
      if (self->serial_abort_.load() || ++waits >= 120) return -1;
    }
    self->serial_offset_ = 0;
  }
  const size_t count = std::min<size_t>(size, self->serial_chunk_.length - self->serial_offset_);
  std::memcpy(buffer, self->serial_chunk_.data + self->serial_offset_, count);
  self->serial_offset_ += count;
  return count;
}
void PaxWeb::serial_task_(void *context) {
  auto *self = static_cast<PaxWeb *>(context);
  const esp_err_t result = self->loader_->receive_web_file(self->serial_path_, self->serial_size_,
                                                          self->serial_sha_, serial_read_, self);
  self->report_(result == ESP_OK ? "Serial file saved and verified" : "Serial transfer failed; previous file kept");
  self->serial_result_ = result == ESP_OK ? 2 : 3;
  self->transfer_ = 4;
  vTaskDeleteWithCaps(nullptr);
}
} // namespace esphome::pax_web
