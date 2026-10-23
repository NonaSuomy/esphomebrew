#include "pax_media_bridge.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/log.h"
#include <cerrno>
#include <esp_random.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <sys/stat.h>
#include <unistd.h>

namespace esphome::pax_media_bridge {
static const char *const TAG = "pax_media_bridge";
static constexpr uint16_t PORT = 5001;
static constexpr uint32_t REQUEST_MS = 30000, FILE_MS = 30000,
                          TOKEN_MS = 86400000;
static void nonblocking(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}
void PaxMediaBridge::setup() {
  in_addr address{};
  if (inet_pton(AF_INET, this->ha_address_.c_str(), &address) != 1) {
    this->mark_failed();
    return;
  }
  this->ha_ip_ = address.s_addr;
  this->listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (this->listener_ < 0) {
    this->mark_failed();
    return;
  }
  int one = 1;
  setsockopt(this->listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(PORT);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(this->listener_, reinterpret_cast<sockaddr *>(&local),
             sizeof local) ||
      ::listen(this->listener_, 3)) {
    ::close(this->listener_);
    this->listener_ = -1;
    this->mark_failed();
    return;
  }
  nonblocking(this->listener_);
}
void PaxMediaBridge::dump_config() {
  ESP_LOGCONFIG(TAG,
                "PAX media browser: local commands on 127.0.0.1:%u; selected "
                "SD media only for %s",
                PORT, this->ha_address_.c_str());
}
void PaxMediaBridge::close_(Connection &c) {
  if (c.fd >= 0)
    ::close(c.fd);
  if (c.file)
    std::fclose(c.file);
  c.fd = -1;
  c.file = nullptr;
  c.input.clear();
  c.output.clear();
  c.sent = 0;
  c.left = 0;
  c.waiting = false;
  ++c.generation;
}
void PaxMediaBridge::on_shutdown() {
  for (auto &c : this->clients_)
    this->close_(c);
  if (this->listener_ >= 0)
    ::close(this->listener_);
  this->listener_ = -1;
}
void PaxMediaBridge::reply_(Connection &c, int status, const std::string &body,
                            const std::string &extra) {
  c.output = "HTTP/1.1 " + std::to_string(status) + " " +
             (status == 200 ? "OK" : "Error") +
             "\r\nConnection: close\r\nContent-Type: "
             "application/json\r\nContent-Length: " +
             std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
  c.sent = 0;
  c.waiting = false;
  c.deadline = millis() + REQUEST_MS;
}
void PaxMediaBridge::fail_(Connection &c, int status, const char *message) {
  std::string body = json::build_json([&](JsonObject root) {
    root["ok"] = false;
    root["message"] = message;
  });
  this->reply_(c, status, body);
}
void PaxMediaBridge::action_(
    Connection &c, const std::string &service,
    const std::vector<std::pair<std::string, std::string>> &data,
    const std::string &media_template, const std::string &response_template) {
  if (this->call_id_) {
    this->fail_(c, 409, "Another media request is pending");
    return;
  }
  auto *api = api::global_api_server;
  if (!api || !api->is_connected()) {
    this->fail_(c, 503, "Home Assistant is not connected");
    return;
  }
  api::HomeassistantActionRequest request;
  request.service = StringRef(service);
  request.call_id = ++this->next_call_;
  request.wants_response = !response_template.empty();
  request.response_template = StringRef(response_template);
  request.data.init(data.size() + 1);
  auto &entity = request.data.emplace_back();
  entity.key = StringRef("entity_id");
  entity.value = StringRef(this->entity_);
  for (const auto &p : data) {
    auto &v = request.data.emplace_back();
    v.key = StringRef(p.first);
    v.value = StringRef(p.second);
  }
  if (!media_template.empty()) {
    request.data_template.init(1);
    auto &v = request.data_template.emplace_back();
    v.key = StringRef("media");
    v.value = StringRef(media_template);
  }
  this->call_id_ = request.call_id;
  c.waiting = true;
  c.deadline = millis() + REQUEST_MS;
  const uint32_t generation = c.generation;
  api->register_action_response_callback(
      request.call_id,
      [this, &c, generation](const api::ActionResponse &response) {
        this->call_id_ = 0;
        if (c.fd < 0 || c.generation != generation)
          return;
        if (!response.is_success()) {
          const auto message = response.get_error_message().str();
          this->fail_(c, 502,
                      message.empty()
                          ? "Home Assistant rejected the media action"
                          : message.c_str());
          return;
        }
        auto object = response.get_json();
        auto send_listing = [&](JsonObjectConst listing) {
          std::string body = json::build_json([&](JsonObject root) {
            root.set(listing);
            root["ok"] = true;
          });
          if (body.size() > 60000) {
            this->fail_(c, 502, "Media listing is too large");
            return;
          }
          this->reply_(c, 200, body);
        };
        if (!object.isNull() && object["response"].is<JsonObjectConst>())
          send_listing(object["response"].as<JsonObjectConst>());
        else if (!object.isNull() && object["response"].is<const char *>()) {
          const char *encoded = object["response"].as<const char *>();
          bool valid = json::parse_json(encoded, [&](JsonObject listing) {
            send_listing(listing);
            return true;
          });
          if (!valid)
            this->fail_(c, 502, "Invalid Home Assistant media listing");
        } else
          this->reply_(c, 200, "{\"ok\":true}");
      });
  api->send_homeassistant_action(request);
}
void PaxMediaBridge::dispatch_(Connection &c, const Header &header) {
  if ((header.method == "GET" || header.method == "HEAD") &&
      header.path.compare(0, 8, "/stream/") == 0) {
    this->stream_(c, header);
    return;
  }
  if (!c.is_loopback || header.method != "POST") {
    this->fail_(c, 403, "Media commands are local to the panel");
    return;
  }
  const auto body = c.input.substr(header.end, header.length);
  bool valid = json::parse_json(body, [&](JsonObject root) {
    if (header.path == "/browse") {
      const std::string id = root["id"] | "";
      const int offset = root["offset"] | 0;
      if (id.size() > 1024 || offset < 0 || offset > 100000)
        return false;
      std::vector<std::pair<std::string, std::string>> data;
      if (!id.empty())
        data.emplace_back("media_content_id", id);
      // Limit both the response size and each string before it crosses the
      // native API.
      const std::string key = json_quote(this->entity_);
      const std::string templ =
          "{% set b = response[" + key +
          "] %}{% set ns = namespace(items=[]) %}"
          "{% for c in (b.children or [])[ " +
          std::to_string(offset) + ":" + std::to_string(offset + 32) +
          "] %}"
          "{% set ns.items=ns.items+[{'title':(c.title or '')[:128], "
          "'id':(c.media_content_id or '') if "
          "(c.media_content_id or '')|length <= 1024 else '',"
          "'type':(c.media_content_type or '')[:128], "
          "'class':(c.media_class or ''),"
          "'dir':c.can_expand and (c.media_content_id or '')|length <= 1024, 'play':c.can_play and (c.media_content_id or '')|length <= 1024}] "
          "%}{% endfor %}"
          "{{ {'title':(b.title or '')[:128], 'items':ns.items, "
          "'total':(b.children or [])|length, 'offset':" +
          std::to_string(offset) + "} | to_json }}";
      this->action_(c, "media_player.browse_media", data, "", templ);
      return true;
    }
    if (header.path == "/play") {
      std::string id = root["id"] | "", type = root["type"] | "";
      if (id.empty() || id.size() > 1024 || type.size() > 128)
        return false;
      if (root["sd"] | false) {
        if (!safe_media_path(id))
          return false;
        const std::string actual = "/sdcard/" + id.substr(4);
        struct stat st{};
        if (::stat(actual.c_str(), &st) || !S_ISREG(st.st_mode) ||
            st.st_size <= 0 || st.st_size > 0x7fffffff) {
          this->fail_(c, 404, "SD media file is missing or larger than 2 GiB");
          return true;
        }
        std::string address;
        for (const auto &ip : network::get_ip_addresses())
          if (ip.is_ip4() && ip.is_set()) {
            char buffer[network::IP_ADDRESS_BUFFER_SIZE];
            address = ip.str_to(buffer);
            break;
          }
        if (address.empty()) {
          this->fail_(c, 503, "The panel has no IPv4 address");
          return true;
        }
        for (auto &old : this->clients_)
          if (old.file)
            this->close_(old);
        char token[33];
        std::snprintf(token, sizeof token, "%08lx%08lx%08lx%08lx",
                      (unsigned long)esp_random(), (unsigned long)esp_random(),
                      (unsigned long)esp_random(), (unsigned long)esp_random());
        this->sd_token_ = token;
        this->sd_path_ = actual;
        this->token_started_ = millis();
        type = media_type(id);
        id = "http://" + address + ":" + std::to_string(PORT) + "/stream/" +
             this->sd_token_;
      }
      std::string media = json::build_json([&](JsonObject o) {
        o["media_content_id"] = id;
        o["media_content_type"] = type;
      });
      this->action_(c, "media_player.play_media", {}, "{{ " + media + " }}",
                    "");
      return true;
    }
    if (header.path == "/control") {
      const std::string command = root["command"] | "";
      std::string action;
      std::vector<std::pair<std::string, std::string>> data;
      if (command == "pause")
        action = "media_pause";
      else if (command == "resume")
        action = "media_play";
      else if (command == "stop")
        action = "media_stop";
      else if (command == "mute" || command == "unmute") {
        action = "volume_mute";
        data.emplace_back("is_volume_muted",
                          command == "mute" ? "true" : "false");
      } else
        return false;
      this->action_(c, "media_player." + action, data, "", "");
      return true;
    }
    return false;
  });
  if (!valid && c.output.empty() && !c.waiting)
    this->fail_(c, 400, "Invalid media browser request");
}
void PaxMediaBridge::stream_(Connection &c, const Header &h) {
  if (this->sd_token_.empty() || h.path != "/stream/" + this->sd_token_ ||
      millis() - this->token_started_ > TOKEN_MS) {
    this->fail_(c, 404, "No selected SD media");
    return;
  }
  FILE *file = std::fopen(this->sd_path_.c_str(), "rb");
  struct stat st{};
  if (!file || ::fstat(fileno(file), &st) || !S_ISREG(st.st_mode) ||
      st.st_size <= 0 || st.st_size > 0x7fffffff) {
    if (file)
      std::fclose(file);
    this->fail_(c, 404, "SD media is unavailable");
    return;
  }
  uint64_t first, last;
  if (!byte_range(h.range, st.st_size, first, last)) {
    std::fclose(file);
    this->reply_(c, 416, "",
                 "Content-Range: bytes */" + std::to_string(st.st_size) +
                     "\r\n");
    return;
  }
  if (std::fseek(file, static_cast<long>(first), SEEK_SET)) {
    std::fclose(file);
    this->fail_(c, 500, "Could not seek in SD media");
    return;
  }
  const char *type = media_type(this->sd_path_);
  c.output = std::string("HTTP/1.1 ") +
             (h.range.empty() ? "200 OK" : "206 Partial Content") +
             "\r\nConnection: close\r\nAccept-Ranges: bytes\r\nContent-Type: " +
             (type ? type : "application/octet-stream") +
             "\r\nContent-Length: " + std::to_string(last - first + 1) + "\r\n";
  if (!h.range.empty())
    c.output += "Content-Range: bytes " + std::to_string(first) + "-" +
                std::to_string(last) + "/" + std::to_string(st.st_size) +
                "\r\n";
  c.output += "\r\n";
  c.sent = 0;
  c.deadline = millis() + FILE_MS;
  if (h.method == "HEAD")
    std::fclose(file);
  else {
    c.file = file;
    c.left = last - first + 1;
  }
}
void PaxMediaBridge::loop() {
  if (this->listener_ < 0)
    return;
  sockaddr_in peer{};
  socklen_t length = sizeof peer;
  int fd =
      ::accept(this->listener_, reinterpret_cast<sockaddr *>(&peer), &length);
  if (fd >= 0) {
    const bool local = ntohl(peer.sin_addr.s_addr) == 0x7f000001;
    Connection *free = nullptr;
    for (auto &c : this->clients_)
      if (c.fd < 0) {
        free = &c;
        break;
      }
    if ((!local && peer.sin_addr.s_addr != this->ha_ip_) || !free)
      ::close(fd);
    else {
      nonblocking(fd);
      free->fd = fd;
      free->is_loopback = local;
      free->deadline = millis() + REQUEST_MS;
    }
  }
  for (auto &c : this->clients_) {
    if (c.fd < 0)
      continue;
    if (static_cast<int32_t>(millis() - c.deadline) >= 0) {
      if (c.waiting && this->call_id_)
        api::global_api_server->handle_action_response(
            this->call_id_, false,
            StringRef("Media action timed out; check the device's HA action "
                      "permission"));
      else
        this->close_(c);
      continue;
    }
    if (!c.output.empty()) {
      int n =
          ::send(c.fd, c.output.data() + c.sent, c.output.size() - c.sent, 0);
      if (n > 0) {
        c.sent += n;
        c.deadline = millis() + FILE_MS;
      } else if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        this->close_(c);
        continue;
      }
      if (c.sent == c.output.size()) {
        c.output.clear();
        c.sent = 0;
        if (!c.file || !c.left) {
          this->close_(c);
          continue;
        }
      }
      continue;
    }
    if (c.file) {
      char buffer[4096];
      size_t n = std::fread(
          buffer, 1,
          static_cast<size_t>(std::min<uint64_t>(sizeof buffer, c.left)),
          c.file);
      if (!n) {
        this->close_(c);
        continue;
      }
      c.left -= n;
      c.output.assign(buffer, n);
      continue;
    }
    if (c.waiting)
      continue;
    char buffer[1024];
    int n = ::recv(c.fd, buffer, sizeof buffer, 0);
    if (!n || (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) {
      this->close_(c);
      continue;
    }
    if (n < 0)
      continue;
    c.input.append(buffer, n);
    Header header;
    int state = parse_header(c.input, header);
    if (state < 0 || c.input.size() > 8192) {
      this->fail_(c, 400, "Invalid HTTP request");
      continue;
    }
    if (state == 1 && c.input.size() >= header.end + header.length) {
      this->dispatch_(c, header);
      c.input.clear();
    }
  }
}
} // namespace esphome::pax_media_bridge
