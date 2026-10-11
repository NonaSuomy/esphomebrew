#include "pax_web.h"
#include "esp_random.h"
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "freertos/idf_additions.h"
#include "pax_web_assets.h"
#include "pax_web_starter.h"
#include <algorithm>
#include <cstdio>
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
  config.max_uri_handlers = 8;
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
                          {"/pax/status", HTTP_GET},
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
  ESP_LOGI(TAG, "SD setup and file upload available at http://<device-ip>:8080/pax");
}
void PaxWeb::dump_config() {
  ESP_LOGCONFIG(TAG, "PAX SD setup: port %u, path /pax", port_);
}
void PaxWeb::loop() {
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
  const std::string suffix = port_ == 80 ? "" : ":" + std::to_string(port_);
  const std::string name = App.get_name() + ".local";
  if (host == name + suffix || (port_ == 80 && host == name + ":80"))
    return true;
  for (const auto &ip : network::get_ip_addresses()) {
    if (!ip.is_set())
      continue;
    char text[network::IP_ADDRESS_BUFFER_SIZE];
    ip.str_to(text);
    const std::string value =
        ip.is_ip6() ? "[" + std::string(text) + "]" : std::string(text);
    if (host == value + suffix || (port_ == 80 && host == value + ":80"))
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
} // namespace esphome::pax_web
