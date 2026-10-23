#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace esphome::pax_media_bridge {
inline std::string json_quote(const std::string &s) {
  std::string out = "\"";
  const char *hex = "0123456789abcdef";
  for (unsigned char c : s) {
    if (c == '\\' || c == '"') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += static_cast<char>(c);
  }
  return out + "\"";
}
inline std::string lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return s;
}
inline const char *media_type(const std::string &path) {
  auto p = path.find_last_of('.');
  if (p == std::string::npos)
    return nullptr;
  const auto ext = lower(path.substr(p));
  if (ext == ".mp3")
    return "audio/mpeg";
  if (ext == ".flac")
    return "audio/flac";
  if (ext == ".wav")
    return "audio/wav";
  if (ext == ".mp4" || ext == ".m4v")
    return "video/mp4";
  if (ext == ".avi")
    return "video/x-msvideo";
  if (ext == ".mjpeg" || ext == ".mjpg")
    return "video/x-motion-jpeg";
  return nullptr;
}
inline bool safe_media_path(const std::string &path) {
  if (path.size() > 512 || path.compare(0, 10, "/sd/media/") != 0 ||
      !media_type(path))
    return false;
  size_t start = 1;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i < path.size() && (static_cast<unsigned char>(path[i]) < 32 ||
                            path[i] == '\\' || path[i] == ':'))
      return false;
    if (i == path.size() || path[i] == '/') {
      if (i > 0 && (i == start || path.substr(start, i - start) == "." ||
                    path.substr(start, i - start) == ".."))
        return false;
      start = i + 1;
    }
  }
  return true;
}
inline bool decimal(const std::string &s, uint64_t &value) {
  if (s.empty())
    return false;
  value = 0;
  for (char c : s) {
    if (c < '0' || c > '9' ||
        value > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10)
      return false;
    value = value * 10 + c - '0';
  }
  return true;
}
struct Header {
  std::string method, path, range;
  size_t end{0}, length{0};
};
// 0: incomplete, 1: complete header, -1: malformed/oversized request.
inline int parse_header(const std::string &s, Header &h) {
  auto end = s.find("\r\n\r\n");
  if (end == std::string::npos)
    return s.size() > 4096 ? -1 : 0;
  if (end + 4 > 4096)
    return -1;
  auto line = s.find("\r\n"), a = s.find(' '),
       b = s.find(' ', a == std::string::npos ? 0 : a + 1);
  if (a == std::string::npos || b == std::string::npos || b >= line ||
      (s.substr(b + 1, line - b - 1) != "HTTP/1.1" &&
       s.substr(b + 1, line - b - 1) != "HTTP/1.0"))
    return -1;
  h = {};
  h.method = s.substr(0, a);
  h.path = s.substr(a + 1, b - a - 1);
  h.end = end + 4;
  if (h.path.empty() || h.path.size() > 128 || h.path[0] != '/')
    return -1;
  bool length_seen = false, range_seen = false;
  for (size_t pos = line + 2; pos < end;) {
    auto next = s.find("\r\n", pos), colon = s.find(':', pos);
    if (colon == std::string::npos || colon >= next || next > end)
      return -1;
    auto key = lower(s.substr(pos, colon - pos));
    size_t value_start = colon + 1;
    while (value_start < next &&
           (s[value_start] == ' ' || s[value_start] == '\t'))
      ++value_start;
    auto value = s.substr(value_start, next - value_start);
    if (key == "content-length") {
      uint64_t n;
      if (length_seen || !decimal(value, n) || n > 4096)
        return -1;
      length_seen = true;
      h.length = static_cast<size_t>(n);
    } else if (key == "transfer-encoding")
      return -1;
    else if (key == "range") {
      if (range_seen)
        return -1;
      range_seen = true;
      h.range = value;
    }
    pos = next + 2;
  }
  return 1;
}
inline bool byte_range(const std::string &s, uint64_t size, uint64_t &first,
                       uint64_t &last) {
  if (!size)
    return false;
  first = 0;
  last = size - 1;
  if (s.empty())
    return true;
  if (s.compare(0, 6, "bytes=") != 0)
    return false;
  auto dash = s.find('-', 6);
  if (dash == std::string::npos || s.find(',', 6) != std::string::npos)
    return false;
  uint64_t n;
  if (dash == 6) {
    if (!decimal(s.substr(dash + 1), n) || !n)
      return false;
    first = n >= size ? 0 : size - n;
  } else {
    if (!decimal(s.substr(6, dash - 6), first) || first >= size)
      return false;
    if (dash + 1 < s.size()) {
      if (!decimal(s.substr(dash + 1), n) || n < first)
        return false;
      last = std::min(n, size - 1);
    }
  }
  return true;
}
} // namespace esphome::pax_media_bridge
