#pragma once
#include "esp_http_server.h"
#include "esphome/components/pax_loader/pax_loader.h"
#include "esphome/core/component.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <span>
#include <vector>
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
  static void snapshot_task_(void *context);
  std::vector<uint8_t> snapshot_command_(std::span<const uint8_t> payload);
  std::atomic<int> snapshot_state_{0}; // idle, capturing, ready, failed
  uint16_t *snapshot_pixels_{nullptr}, snapshot_width_{0}, snapshot_height_{0};
  uint32_t snapshot_id_{0}, snapshot_access_{0};
  uint8_t snapshot_sha_[32]{};
  static void serial_task_(void *context);
  static int serial_read_(void *context, uint8_t *buffer, size_t size);
  std::vector<uint8_t> serial_command_(uint8_t command, std::span<const uint8_t> payload);
  struct SerialChunk { uint16_t length; uint8_t data[240]; };
  bool serial_bound_{false};
  QueueHandle_t serial_queue_{nullptr};
  SerialChunk serial_chunk_{};
  size_t serial_offset_{0};
  uint32_t serial_sequence_{0}, serial_received_{0}, serial_size_{0};
  std::string serial_path_, serial_sha_;
  std::atomic<int> serial_result_{0};  // 0 idle, 1 receiving, 2 saved, 3 failed
  std::atomic<bool> serial_abort_{false};
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
