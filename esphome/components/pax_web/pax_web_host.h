#pragma once
#include <cstdint>
#include <string>

namespace esphome::pax_web {
// address must come from the device, not Host/Origin or forwarded headers.
inline bool host_matches(const std::string &host, const std::string &address,
                         uint16_t port) {
  return host == address + ":" + std::to_string(port) ||
         (port == 80 && host == address);
}
} // namespace esphome::pax_web
