#pragma once
#include "esphome/components/api/api_server.h"
#include "esphome/components/api/homeassistant_service.h"
#include "esphome/core/component.h"
#include "protocol.h"
#include <array>
#include <cstdio>
#include <utility>
#include <vector>

namespace esphome::pax_media_bridge {
class PaxMediaBridge : public Component {
public:
  void set_relay_entity(const std::string &value) { this->entity_ = value; }
  void set_home_assistant_address(const std::string &value) {
    this->ha_address_ = value;
  }
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override {
    return setup_priority::AFTER_WIFI;
  }

protected:
  struct Connection {
    int fd{-1};
    bool is_loopback{false}, waiting{false};
    uint32_t generation{0}, deadline{0};
    std::string input, output;
    size_t sent{0};
    FILE *file{nullptr};
    uint64_t left{0};
  };
  std::array<Connection, 3> clients_{};
  int listener_{-1};
  uint32_t ha_ip_{0}, call_id_{0}, next_call_{0x50410000};
  std::string entity_, ha_address_, sd_path_, sd_token_;
  uint32_t token_started_{0};
  void close_(Connection &c);
  void reply_(Connection &c, int status, const std::string &body,
              const std::string &extra = "");
  void fail_(Connection &c, int status, const char *message);
  void dispatch_(Connection &c, const Header &header);
  void action_(Connection &c, const std::string &service,
               const std::vector<std::pair<std::string, std::string>> &data,
               const std::string &media_template,
               const std::string &response_template);
  void stream_(Connection &c, const Header &header);
};
} // namespace esphome::pax_media_bridge
