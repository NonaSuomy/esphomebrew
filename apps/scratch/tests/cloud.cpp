#include "mist/mist.hpp"
#include "pax_port.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <nlohmann/json.hpp>
#include <vector>
extern std::unique_ptr<MistConnection> cloudConnection;
extern std::string pax_cloud_test_accept(const std::string &);
namespace {
std::string incoming, outgoing;
int64_t clockValue;
int connects, closes, status;
bool tlsUsed;
int connectTls(const char *host, uint16_t port) {
  assert(std::string(host) == "clouddata.turbowarp.org" && port == 443);
  tlsUsed = true;
  return ++connects;
}
int sendBytes(int, const void *data, int n) {
  n = std::min(n, 7);
  outgoing.append(static_cast<const char *>(data), n);
  return n;
}
int recvBytes(int, void *data, int n) {
  n = std::min<int>(incoming.size(), n);
  if (n) {
    memcpy(data, incoming.data(), n);
    incoming.erase(0, n);
  }
  return n;
}
std::string serverFrame(int op, const std::string &text, bool fin = true) {
  std::string s;
  s += char((fin ? 128 : 0) | op);
  if (text.size() < 126)
    s += char(text.size());
  else if (text.size() <= 65535) {
    s += char(126);
    s += char(text.size() >> 8);
    s += char(text.size());
  } else {
    s += char(127);
    for (int i = 7; i >= 0; i--)
      s += char(uint64_t(text.size()) >> (i * 8));
  }
  return s + text;
}
std::vector<std::pair<int, std::string>> clientFrames() {
  std::vector<std::pair<int, std::string>> out;
  while (outgoing.size() >= 2) {
    unsigned op = uint8_t(outgoing[0]) & 15, n = uint8_t(outgoing[1]) & 127;
    assert(uint8_t(outgoing[1]) & 128);
    size_t at = 2;
    if (n == 126) {
      if (outgoing.size() < 4)
        break;
      n = uint8_t(outgoing[2]) * 256 + uint8_t(outgoing[3]);
      at = 4;
    } else if (n == 127) {
      if (outgoing.size() < 10)
        break;
      uint64_t big = 0;
      for (int i = 2; i < 10; i++)
        big = big << 8 | uint8_t(outgoing[i]);
      assert(big <= 102400);
      n = big;
      at = 10;
    }
    if (outgoing.size() < at + 4 + n)
      break;
    std::string v = outgoing.substr(at + 4, n);
    for (size_t i = 0; i < v.size(); i++)
      v[i] = uint8_t(v[i]) ^ uint8_t(outgoing[at + i % 4]);
    out.emplace_back(op, std::move(v));
    outgoing.erase(0, at + 4 + n);
  }
  return out;
}
void accept(MistConnection &c, bool valid = true) {
  c.tick();
  auto begin = outgoing.find("Sec-WebSocket-Key: ");
  assert(begin != std::string::npos);
  begin += 19;
  auto end = outgoing.find("\r\n", begin);
  std::string key = outgoing.substr(begin, end - begin);
  assert(outgoing.find("User-Agent: PAX-Scratch/") != std::string::npos);
  outgoing.clear();
  incoming = "HTTP/1.1 101 Any Reason\r\nUpgrade: websocket\r\nConnection: "
             "Upgrade\r\nSec-WebSocket-Accept: " +
             (valid ? pax_cloud_test_accept(key) : "wrong") + "\r\n\r\n";
  c.tick();
  if (valid) {
    assert(c.isConnected());
    auto frames = clientFrames();
    assert(frames.size() == 1);
    auto j = nlohmann::json::parse(frames[0].second);
    assert(j["method"] == "handshake" && j["project_id"] == "test-room");
  } else
    assert(!c.isConnected());
}
} // namespace
void testCloud() {
  assert(pax_cloud_test_accept("dGhlIHNhbXBsZSBub25jZQ==") ==
         "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  auto old = pax_svc;
  app_services_t svc{};
  svc.get_time_us = []() -> int64_t { return clockValue; };
  svc.net_tls_connect = connectTls;
  svc.net_tls_status = [](int) { return status; };
  svc.net_tls_send = sendBytes;
  svc.net_tls_recv = recvBytes;
  svc.net_tls_close = [](int) { closes++; };
  pax_svc = &svc;
  clockValue = 1000000;
  status = 1;
  connects = closes = 0;
  tlsUsed = false;
  {
    MistConnection c("test-room", "player1234567",
                     "https://example.org/issues");
    int updates = 0;
    std::string value;
    c.onVariableUpdate([&](const std::string &n, const std::string &v) {
      assert(n == "☁ test");
      updates++;
      value = v;
    });
    assert(c.connect(false) && tlsUsed);
    accept(c);
    std::string message = "{\"method\":\"set\",\"name\":\"☁ "
                          "test\",\"value\":\"12345678901234567890\"}";
    auto frame = serverFrame(1, message);
    for (char byte : frame) {
      incoming += byte;
      c.tick();
    }
    assert(updates == 1 && value == "12345678901234567890" &&
           c.get("☁ test") == value);
    incoming = serverFrame(1, "", false) + serverFrame(9, "ping") +
               serverFrame(0, message);
    c.tick();
    assert(updates == 2);
    auto frames = clientFrames();
    assert(frames.size() == 1 && frames[0].first == 10 &&
           frames[0].second == "ping");
    incoming = serverFrame(1, "{\"method\":1}\n" + message + "\n" + message);
    c.tick();
    assert(updates == 4);
    assert(!c.set("☁ test", "NaN"));
    assert(c.set("☁ test", "42"));
    assert(c.set("☁ test", "43"));
    c.tick();
    frames = clientFrames();
    assert(frames.size() == 1 &&
           nlohmann::json::parse(frames[0].second)["value"] == "42");
    clockValue += 100001;
    c.tick();
    frames = clientFrames();
    assert(frames.size() == 1 &&
           nlohmann::json::parse(frames[0].second)["value"] == "43");
    std::string large(70000, '7');
    incoming = serverFrame(
        1,
        nlohmann::json{{"method", "set"}, {"name", "☁ test"}, {"value", large}}
            .dump());
    for (int i = 0; i < 10; i++)
      c.tick();
    assert(value == large);
    incoming = serverFrame(1, "not JSON");
    c.tick();
    assert(!c.isConnected());
    int count = connects;
    clockValue += 3000000;
    c.tick();
    assert(connects == count + 1);
    accept(c, false);
  }
  incoming.clear();
  outgoing.clear();
  {
    MistConnection c("test-room", "player1234567",
                     "https://example.org/issues");
    assert(c.connect());
    accept(c);
    incoming = serverFrame(8, std::string("\x0f\xa4", 2) + "Unavailable");
    c.tick();
    int count = connects;
    clockValue += 120000000;
    c.tick();
    assert(connects == count && !c.isConnected());
  }
  incoming.clear();
  outgoing.clear();
  {
    MistConnection c("test-room", "player1234567",
                     "https://example.org/issues");
    assert(c.connect());
    accept(c);
    incoming = std::string("\x81\x7f\0\0\0\0\0\x10\0\0", 10);
    c.tick();
    assert(!c.isConnected());
  }
  incoming.clear();
  outgoing.clear();
  {
    MistConnection c("test-room", "player1234567",
                     "https://example.org/issues");
    assert(c.connect());
    accept(c);
    std::string large(100000, '2');
    assert(c.set("☁ test", large));
    assert(c.set("☁ test", large));
    assert(!c.set("☁ test", large));
    assert(!c.isConnected());
  }
  incoming.clear();
  outgoing.clear();
  assert(closes >= 5);
  incoming.clear();
  outgoing.clear();
  status = 0;
  cloudConnection = std::make_unique<MistConnection>(
      "test-room", "player1234567", "https://example.org/issues");
  assert(cloudConnection->connect());
  assert(pax_cloud_begin_startup());
  clockValue += 3000000;
  pax_cloud_tick();
  assert(pax_cloud_startup_waiting());
  status = 1;
  accept(*cloudConnection);
  assert(!pax_cloud_startup_waiting());
  cloudConnection.reset();
  incoming.clear();
  outgoing.clear();
  status = -1;
  cloudConnection = std::make_unique<MistConnection>(
      "test-room", "player1234567", "https://example.org/issues");
  assert(cloudConnection->connect());
  assert(pax_cloud_begin_startup());
  pax_cloud_tick();
  assert(!pax_cloud_startup_waiting());
  cloudConnection.reset();
  status = 1;
  puts("PASS cloud startup gate waits through slow connection and releases on "
       "success or failure");
  pax_svc = old;
  puts("PASS cloud: verified WebSocket handshake, masked/partial sends, "
       "fragmented/newline JSON, exact long values, ping, rate limit, bounded "
       "queues/frames, malformed JSON, retry and clean shutdown");
}
