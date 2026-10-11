#pragma once
#include "esp_http_server.h"
#include "esphome/components/pax_loader/pax_loader.h"
#include "esphome/core/component.h"
#include "freertos/semphr.h"
#include <atomic>
#include <string>
namespace esphome::pax_web {
class PaxWeb : public Component {
public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override {
    return setup_priority::AFTER_WIFI;
  }
  void set_loader(pax_loader::PaxLoader *loader) { loader_ = loader; }
  void set_port(uint16_t port) { port_ = port; }

protected:
  static esp_err_t handle_(httpd_req_t *req);
  esp_err_t serve_(httpd_req_t *req);
  static int read_(void *context, uint8_t *buffer, size_t size);
  static void starter_task_(void *context);
  bool reserve_();
  bool allowed_host_(httpd_req_t *req);
  bool authorized_(httpd_req_t *req);
  void report_(const char *message);
  esp_err_t reply_(httpd_req_t *req, const char *status, const char *message);
  pax_loader::PaxLoader *loader_{nullptr};
  uint16_t port_{8080};
  httpd_handle_t server_{nullptr};
  SemaphoreHandle_t granted_{nullptr};
  SemaphoreHandle_t status_mutex_{nullptr};
  TaskHandle_t starter_task_handle_{nullptr};
  // 0 idle, 1 main-loop reservation requested, 2 reserved, 3 rejected, 4
  // release requested.
  std::atomic<int> transfer_{0};
  std::atomic<bool> close_requested_{false};
  char token_[33]{};
  char status_[192]{"Ready"};
};
} // namespace esphome::pax_web
