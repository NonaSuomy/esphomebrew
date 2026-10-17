#include "../components/pax_web/pax_web_host.h"
#include <cassert>
#include <iostream>
using esphome::pax_web::host_matches;
int main() {
  assert(host_matches("10.13.37.159:8080", "10.13.37.159", 8080));
  assert(host_matches("esp32-p4-elecrow-pax-quick.local:8080", "esp32-p4-elecrow-pax-quick.local", 8080));
  assert(host_matches("[fd00::42]:8080", "[fd00::42]", 8080));
  assert(host_matches("10.13.37.159", "10.13.37.159", 80));
  assert(host_matches("10.13.37.159:80", "10.13.37.159", 80));
  for (const auto &host : {"evil.example:8080", "10.13.37.159.evil.example:8080", "10.13.37.159", "10.13.37.159:80", "10.13.37.159:8080@evil.example", "10.13.37.159:8080/", "10.13.37.159:8080,evil.example"})
    assert(!host_matches(host, "10.13.37.159", 8080));
  // Reaching Wi-Fi does not require its IP to be the preferred Ethernet IP.
  assert(!host_matches("10.13.37.159:8080", "10.20.30.158", 8080));
  assert(host_matches("10.13.37.159:8080", "10.13.37.159", 8080));
  std::cout << "Device Host checks: secondary interface, mDNS, IPv6, ports and hostile names passed\n";
}
