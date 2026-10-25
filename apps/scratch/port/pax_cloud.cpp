#include "mist/mist.hpp"
#include "pax_port.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <log.hpp>
#include <map>
#include <nlohmann/json.hpp>
#include <render.hpp>
#include <runtime.hpp>
#include <settings.hpp>
#include <unzip.hpp>
#include <vector>
extern std::unique_ptr<MistConnection> cloudConnection;
namespace {
constexpr size_t MAX_MESSAGE = 102400, MAX_QUEUE = 262144;
static int64_t now() {
  return pax_svc && pax_svc->get_time_us ? pax_svc->get_time_us() : 0;
}
static uint32_t randomWord() {
  static uint32_t s = 0x8591fe03;
  s ^= uint32_t(now()) + uint32_t(rand());
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
static std::string b64(const uint8_t *p, size_t n) {
  const char *a =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(p[i]) << 16;
    if (i + 1 < n)
      v |= uint32_t(p[i + 1]) << 8;
    if (i + 2 < n)
      v |= p[i + 2];
    out += a[v >> 18];
    out += a[(v >> 12) & 63];
    out += i + 1 < n ? a[(v >> 6) & 63] : '=';
    out += i + 2 < n ? a[v & 63] : '=';
  }
  return out;
}
static uint32_t rol(uint32_t x, unsigned n) { return x << n | x >> (32 - n); }
static std::string acceptKey(const std::string &key) {
  std::string v = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint64_t bits = v.size() * 8;
  v.push_back(char(0x80));
  while (v.size() % 64 != 56)
    v.push_back(0);
  for (int i = 7; i >= 0; --i)
    v.push_back(char(bits >> (i * 8)));
  uint32_t h[] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  for (size_t at = 0; at < v.size(); at += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      w[i] = 0;
      for (int j = 0; j < 4; j++)
        w[i] = w[i] << 8 | uint8_t(v[at + i * 4 + j]);
    }
    for (int i = 16; i < 80; i++)
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f = i < 20   ? ((b & c) | (~b & d))
                   : i < 40 ? (b ^ c ^ d)
                   : i < 60 ? ((b & c) | (b & d) | (c & d))
                            : (b ^ c ^ d);
      uint32_t k = i < 20   ? 0x5a827999
                   : i < 40 ? 0x6ed9eba1
                   : i < 60 ? 0x8f1bbcdc
                            : 0xca62c1d6;
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  uint8_t digest[20];
  for (int i = 0; i < 20; i++)
    digest[i] = h[i / 4] >> (24 - (i % 4) * 8);
  return b64(digest, 20);
}
static std::string lower(std::string s) {
  for (char &c : s)
    c = std::tolower(uint8_t(c));
  return s;
}
static std::string trim(std::string s) {
  auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static bool numeric(const std::string &s) {
  if (s.empty() || s.size() > 100000)
    return false;
  bool digit = false, dot = false;
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  for (; i < s.size(); i++) {
    if (s[i] >= '0' && s[i] <= '9')
      digit = true;
    else if (s[i] == '.' && !dot)
      dot = true;
    else
      return false;
  }
  return digit;
}
} // namespace
struct MistConnection::Impl {
  std::string url, project, user, contact, host, path, key, expected, rx,
      fragment;
  uint16_t port = 443;
  bool tls = true, enabled = false, ready = false, fragmented = false;
  int handle = -1, state = 0;
  int64_t deadline = 0, retryAt = 0, lastSend = 0;
  unsigned retries = 0;
  struct Packet {
    std::string bytes;
    size_t sent = 0;
    bool update = false;
  };
  std::deque<Packet> tx;
  size_t queued = 0;
  std::map<std::string, std::string> values;
  VariableUpdateCallback onValue;
  ConnectionStatusCallback onStatus;
  Impl(std::string u, std::string p, std::string n, std::string c)
      : url(std::move(u)), project(std::move(p)), user(std::move(n)),
        contact(std::move(c)) {}
  void close() {
    if (handle >= 0 && pax_svc) {
      if (tls && pax_svc->net_tls_close)
        pax_svc->net_tls_close(handle);
      else if (!tls && pax_svc->net_udp_close)
        pax_svc->net_udp_close(handle);
    }
    handle = -1;
    ready = false;
    state = 0;
    rx.clear();
    fragment.clear();
    fragmented = false;
    tx.clear();
    queued = 0;
  }
  void fail(const std::string &why) {
    close();
    if (onStatus)
      onStatus(false, why);
    if (enabled) {
      retryAt = now() +
                int64_t(std::min(60u, 2u << std::min(retries++, 5u))) * 1000000;
      state = 4;
    }
  }
  bool parseUrl() {
    tls = url.compare(0, 6, "wss://") == 0;
    size_t start = tls ? 6 : url.compare(0, 5, "ws://") == 0 ? 5 : 0;
    if (!start || url.find_first_of("\r\n\t @#") != std::string::npos)
      return false;
    auto slash = url.find('/', start);
    auto authority = url.substr(start, slash - start);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    auto colon = authority.find(':');
    host = authority.substr(0, colon);
    port = tls ? 443 : 80;
    if (colon != std::string::npos) {
      auto p = authority.substr(colon + 1);
      unsigned n = 0;
      if (p.empty() || p.size() > 5)
        return false;
      for (char c : p) {
        if (c < '0' || c > '9')
          return false;
        n = n * 10 + c - '0';
      }
      if (!n || n > 65535)
        return false;
      port = n;
    }
    return !host.empty() && host.size() <= 253 && path.size() <= 512;
  }
  bool start() {
    close();
    if (!pax_svc || !parseUrl() || contact.empty() ||
        contact.find_first_of("\r\n") != std::string::npos) {
      enabled = false;
      fail("Invalid cloud endpoint or User-Agent");
      return false;
    }
    if (tls) {
      if (!pax_svc->net_tls_connect || !pax_svc->net_tls_status ||
          !pax_svc->net_tls_send || !pax_svc->net_tls_recv ||
          !pax_svc->net_tls_close) {
        enabled = false;
        fail("Firmware lacks verified TLS cloud services");
        return false;
      }
      handle = pax_svc->net_tls_connect(host.c_str(), port);
    } else {
      uint32_t ip = 0;
      if (!pax_svc->net_resolve || !pax_svc->net_tcp_connect ||
          !pax_svc->net_tcp_send || !pax_svc->net_tcp_recv ||
          !pax_svc->net_udp_close || !pax_svc->net_poll) {
        enabled = false;
        fail("Firmware lacks TCP cloud services");
        return false;
      }
      if (pax_svc->net_resolve(host.c_str(), &ip))
        handle = pax_svc->net_tcp_connect(ip, port);
    }
    if (handle < 0) {
      fail("Cloud connection could not start");
      return false;
    }
    state = 1;
    deadline = now() + 45000000;
    return true;
  }
  bool enqueue(std::string bytes, bool update = false) {
    if (bytes.size() > MAX_QUEUE - queued) {
      fail("Cloud output queue limit exceeded");
      return false;
    }
    queued += bytes.size();
    tx.push_back({std::move(bytes), 0, update});
    return true;
  }
  bool frame(unsigned opcode, const std::string &payload, bool update = false) {
    if (payload.size() > MAX_MESSAGE)
      return false;
    std::string f;
    f += char(0x80 | opcode);
    if (payload.size() < 126)
      f += char(0x80 | payload.size());
    else if (payload.size() <= 65535) {
      f += char(0xfe);
      f += char(payload.size() >> 8);
      f += char(payload.size());
    } else {
      f += char(0xff);
      for (int i = 7; i >= 0; i--)
        f += char(uint64_t(payload.size()) >> (i * 8));
    }
    uint32_t mask = randomWord();
    uint8_t m[4];
    for (int i = 0; i < 4; i++) {
      m[i] = mask >> (i * 8);
      f += char(m[i]);
    }
    for (size_t i = 0; i < payload.size(); i++)
      f += char(uint8_t(payload[i]) ^ m[i % 4]);
    return enqueue(std::move(f), update);
  }
  int send(const char *p, int n) {
    return tls ? pax_svc->net_tls_send(handle, p, n)
               : pax_svc->net_tcp_send(handle, p, n);
  }
  int recv(char *p, int n) {
    int got = tls ? pax_svc->net_tls_recv(handle, p, n)
                  : pax_svc->net_tcp_recv(handle, p, n);
    if (!tls) {
      if (got == 0)
        return -1;
      if (got == -2)
        return 0;
    }
    return got;
  }
  bool flush() {
    size_t budget = 4096;
    while (!tx.empty() && budget) {
      auto &p = tx.front();
      if (p.update && p.sent == 0 && lastSend && now() - lastSend < 100000)
        return true;
      int n = send(p.bytes.data() + p.sent,
                   std::min<size_t>(budget, p.bytes.size() - p.sent));
      if (n < 0) {
        fail("Cloud send failed");
        return false;
      }
      if (!n)
        return true;
      p.sent += n;
      budget -= n;
      if (p.sent == p.bytes.size()) {
        if (p.update)
          lastSend = now();
        queued -= p.bytes.size();
        tx.pop_front();
      }
    }
    return true;
  }
  bool headers() {
    auto end = rx.find("\r\n\r\n");
    if (end == std::string::npos) {
      if (rx.size() > 8192)
        fail("Cloud HTTP headers too large");
      return false;
    }
    std::string h = rx.substr(0, end);
    rx.erase(0, end + 4);
    auto first = h.find("\r\n");
    if (h.substr(0, 13) != "HTTP/1.1 101 ") {
      fail("Cloud WebSocket upgrade rejected");
      return false;
    }
    std::map<std::string, std::string> fields;
    size_t at = first + 2;
    while (at < h.size()) {
      auto e = h.find("\r\n", at);
      if (e == std::string::npos)
        e = h.size();
      auto c = h.find(':', at);
      if (c < e)
        fields[lower(h.substr(at, c - at))] = trim(h.substr(c + 1, e - c - 1));
      at = e + 2;
    }
    if (lower(fields["upgrade"]) != "websocket" ||
        lower(fields["connection"]).find("upgrade") == std::string::npos ||
        fields["sec-websocket-accept"] != expected) {
      fail("Invalid cloud WebSocket handshake");
      return false;
    }
    state = 3;
    retries = 0;
    frame(1, nlohmann::json{{"method", "handshake"},
                            {"project_id", project},
                            {"user", user}}
                 .dump());
    ready = true;
    if (onStatus)
      onStatus(true, "Verified WebSocket connected");
    return true;
  }
  bool message(const std::string &text) {
    size_t at = 0;
    while (at < text.size()) {
      auto end = text.find('\n', at);
      if (end == std::string::npos)
        end = text.size();
      auto line = text.substr(at, end - at);
      at = end + 1;
      if (line.empty())
        continue;
      auto j = nlohmann::json::parse(line, nullptr, false);
      if (j.is_discarded() || !j.is_object()) {
        fail("Invalid cloud JSON");
        return false;
      }
      if (!j.contains("method") || !j["method"].is_string() ||
          j["method"].get_ref<const std::string &>() != "set")
        continue;
      if (!j.contains("name") || !j["name"].is_string() || !j.contains("value"))
        continue;
      auto name = j["name"].get<std::string>();
      auto &v = j["value"];
      std::string value = v.is_string()   ? v.get<std::string>()
                          : v.is_number() ? v.dump()
                                          : "";
      if (name.size() > 256 || !numeric(value))
        continue;
      if (values.size() >= 128 && !values.count(name)) {
        fail("Cloud variable limit exceeded");
        return false;
      }
      values[name] = value;
      if (onValue)
        onValue(name, value);
    }
    return true;
  }
  bool frames() {
    while (rx.size() >= 2) {
      uint8_t a = rx[0], b = rx[1];
      unsigned op = a & 15;
      bool fin = a & 128;
      if ((a & 0x70) || (b & 128)) {
        fail("Invalid cloud WebSocket frame");
        return false;
      }
      uint64_t n = b & 127;
      size_t h = 2;
      if (n == 126) {
        if (rx.size() < 4)
          return true;
        n = uint8_t(rx[2]) * 256 + uint8_t(rx[3]);
        h = 4;
      } else if (n == 127) {
        if (rx.size() < 10)
          return true;
        n = 0;
        for (int i = 2; i < 10; i++)
          n = n << 8 | uint8_t(rx[i]);
        h = 10;
      }
      if (n > MAX_MESSAGE || (op >= 8 && (!fin || n > 125))) {
        fail("Cloud frame limit exceeded");
        return false;
      }
      if (n > rx.size() - h)
        return true;
      std::string payload = rx.substr(h, n);
      rx.erase(0, h + n);
      if (op == 8) {
        unsigned code = payload.size() >= 2
                            ? uint8_t(payload[0]) * 256 + uint8_t(payload[1])
                            : 1000;
        if (code == 4002 || code == 4004 || code == 4005 || code == 4006)
          enabled = false;
        fail("Cloud closed (" + std::to_string(code) +
             "): " + (payload.size() > 2 ? payload.substr(2) : ""));
        return false;
      }
      if (op == 9) {
        frame(10, payload);
        continue;
      }
      if (op == 10)
        continue;
      if (op == 1) {
        if (fragmented) {
          fail("Interleaved cloud messages");
          return false;
        }
        if (fin) {
          if (!message(payload))
            return false;
        } else {
          fragment = std::move(payload);
          fragmented = true;
        }
      } else if (op == 0 && fragmented) {
        if (fragment.size() + payload.size() > MAX_MESSAGE) {
          fail("Cloud message limit exceeded");
          return false;
        }
        fragment += payload;
        if (fin) {
          auto text = std::move(fragment);
          fragment.clear();
          fragmented = false;
          if (!message(text))
            return false;
        }
      } else {
        fail("Unsupported cloud frame");
        return false;
      }
    }
    return true;
  }
  void tick() {
    if (state == 4) {
      if (now() >= retryAt)
        start();
      return;
    }
    if (state == 0)
      return;
    if (state != 3 && now() > deadline) {
      fail("Cloud connection timed out");
      return;
    }
    if (state == 1) {
      int status =
          tls ? pax_svc->net_tls_status(handle) : pax_svc->net_poll(handle);
      if (status < 0 || (!tls && (status & 4))) {
        fail("Cloud TLS/TCP connection failed");
        return;
      }
      if (tls ? status == 0 : !(status & 2))
        return;
      uint8_t nonce[16];
      for (auto &n : nonce)
        n = randomWord();
      key = b64(nonce, 16);
      expected = acceptKey(key);
      std::string authority =
          host + ((tls && port == 443) || (!tls && port == 80)
                      ? ""
                      : ":" + std::to_string(port));
      enqueue(
          "GET " + path + " HTTP/1.1\r\nHost: " + authority +
          "\r\nUpgrade: websocket\r\nConnection: "
          "Upgrade\r\nSec-WebSocket-Key: " +
          key +
          "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: PAX-Scratch/0.4.16 " +
          contact + "\r\n\r\n");
      state = 2;
      deadline = now() + 15000000;
    }
    if (!flush())
      return;
    char buf[2048];
    size_t budget = 16384;
    while (budget) {
      int n = recv(buf, std::min<size_t>(budget, sizeof(buf)));
      if (n < 0) {
        fail("Cloud peer disconnected");
        return;
      }
      if (!n)
        break;
      if (rx.size() + n > MAX_MESSAGE + 8192) {
        fail("Cloud receive limit exceeded");
        return;
      }
      rx.append(buf, n);
      budget -= n;
      if (state == 2 && !headers()) {
        if (state != 2)
          return;
        continue;
      }
      if (state == 3 && !frames())
        return;
    }
    if (state == 3)
      flush();
  }
};
MistConnection::MistConnection(const std::string &project,
                               const std::string &user,
                               const std::string &contact)
    : MistConnection("wss://clouddata.turbowarp.org/", project, user, contact) {
}
MistConnection::MistConnection(const std::string &url,
                               const std::string &project,
                               const std::string &user,
                               const std::string &contact)
    : p(new Impl(url, project, user, contact)) {}
MistConnection::~MistConnection() { disconnect(); }
bool MistConnection::connect(bool) {
  p->enabled = true;
  p->retries = 0;
  return p->start();
}
void MistConnection::disconnect() {
  p->enabled = false;
  p->close();
}
void MistConnection::tick() { p->tick(); }
bool MistConnection::isConnected() const { return p->ready; }
bool MistConnection::isConnecting() const {
  return p->state == 1 || p->state == 2;
}
void MistConnection::onVariableUpdate(VariableUpdateCallback cb) {
  p->onValue = std::move(cb);
}
void MistConnection::onConnectionStatus(ConnectionStatusCallback cb) {
  p->onStatus = std::move(cb);
}
bool MistConnection::set(const std::string &name, const std::string &value) {
  if (!p->ready || name.size() > 256 || !numeric(value))
    return false;
  return p->frame(
      1,
      nlohmann::json{{"method", "set"}, {"name", name}, {"value", value}}
          .dump(),
      true);
}
std::string MistConnection::get(const std::string &name) const {
  auto it = p->values.find(name);
  return it == p->values.end() ? "" : it->second;
}
void pax_cloud_tick() {
  if (cloudConnection)
    cloudConnection->tick();
}
std::string pax_cloud_username() {
  auto f = OS::getScratchFolderLocation() + "cloud-username.txt";
  FILE *in = fopen(f.c_str(), "rb");
  char name[32]{};
  if (in) {
    size_t n = fread(name, 1, sizeof(name) - 1, in);
    fclose(in);
    std::string s(name, n);
    s = trim(s);
    bool valid =
        s.size() >= 8 && s.size() <= 13 && s.compare(0, 6, "player") == 0;
    for (size_t i = 6; i < s.size(); i++)
      valid = valid && std::isdigit(uint8_t(s[i]));
    if (valid)
      return s;
  }
  std::string s = "player" + std::to_string(1000000 + randomWord() % 9000000);
  FILE *out = fopen(f.c_str(), "wb");
  if (out) {
    fwrite(s.data(), 1, s.size(), out);
    fclose(out);
  }
  return s;
}
#ifdef PAX_HOST_TEST
std::string pax_cloud_test_accept(const std::string &key) {
  return acceptKey(key);
}
#endif

namespace {
int64_t cloudStartupDeadline = 0, cloudStartupFrame = 0;
}
bool pax_cloud_begin_startup() {
  cloudStartupDeadline = now() + 60000000;
  cloudStartupFrame = 0;
  return cloudConnection && cloudConnection->isConnecting();
}
bool pax_cloud_startup_waiting() {
  if (!cloudConnection || !cloudConnection->isConnecting())
    return false;
  if (now() < cloudStartupDeadline)
    return true;
  Log::logWarning("Cloud startup timed out; starting project offline");
  return false;
}
void pax_cloud_draw_startup() {
  if (cloudStartupFrame && now() - cloudStartupFrame < 100000)
    return;
  cloudStartupFrame = now();
  Render::beginFrame(0, 20, 25, 40);
  pax_draw_text("Connecting to cloud...", PAX_W / 2, PAX_H / 2 - 12, 0xffffffff,
                1, true);
  Render::endFrame();
}
