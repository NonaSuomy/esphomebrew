#include "pax_editor.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dr_mp3.h>
#include <dr_wav.h>
#include <fstream>
#include <iomanip>
#include <lunasvg.h>
#include <sstream>
#include <stb_image.h>
namespace PaxEdit {
static std::string digest(const std::string &data) {
  std::vector<uint8_t> bytes(data.begin(), data.end());
  uint64_t bits = (uint64_t)bytes.size() * 8;
  bytes.push_back(0x80);
  while (bytes.size() % 64 != 56)
    bytes.push_back(0);
  for (int i = 0; i < 8; i++)
    bytes.push_back(bits >> (8 * i));
  uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  const int shifts[16] = {7, 12, 17, 22, 5, 9,  14, 20,
                          4, 11, 16, 23, 6, 10, 15, 21};
  static constexpr uint32_t constants[64] = {
      0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
      0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
      0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
      0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
      0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
      0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
      0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
      0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
      0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
      0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
      0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
      0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
      0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
  for (size_t off = 0; off < bytes.size(); off += 64) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
      m[i] = 0;
      for (int j = 0; j < 4; j++)
        m[i] |= (uint32_t)bytes[off + 4 * i + j] << (8 * j);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
      uint32_t f, g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = 7 * i % 16;
      }
      uint32_t v = a + f + constants[i] + m[g];
      int shift = shifts[(i / 16) * 4 + i % 4];
      uint32_t next = b + ((v << shift) | (v >> (32 - shift)));
      a = d;
      d = c;
      c = b;
      b = next;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (auto word : h)
    for (int i = 0; i < 4; i++)
      out << std::setw(2) << ((word >> (8 * i)) & 255);
  return out.str();
}
std::string Project::storeAsset(const std::string &data,
                                const std::string &extension) {
  auto id = digest(data);
  auto key = id + "." + extension;
  size_t used = 0;
  for (const auto &entry : assets_)
    used += entry.second.size();
  if (!assets_.count(key) && used + data.size() > 8 * 1024 * 1024) {
    error = "Save and reopen the project before more asset edits";
    return "";
  }
  assets_[key] = data;
  return id;
}
static std::string uniqueName(const Json &collection, std::string name) {
  if (name.empty())
    name = "asset";
  name = name.substr(0, 50);
  auto base = name;
  int n = 2;
  while (
      std::any_of(collection.begin(), collection.end(), [&](const Json &item) {
        return item.value("name", "") == name;
      }))
    name = base + " " + std::to_string(n++);
  return name;
}
bool Project::paintCostume(const std::string &name,
                           const std::vector<uint32_t> &pixels, int width,
                           int height, double centerX, double centerY) {
  if (pixels.size() != 1024 || width < 1 || height < 1 || width > 4096 ||
      height > 4096) {
    error = "Invalid paint canvas";
    return false;
  }
  std::ostringstream svg;
  svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << width
      << "' height='" << height
      << "' viewBox='0 0 32 32' preserveAspectRatio='none'>";
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      auto c = pixels[y * 32 + x];
      if (!(c & 255))
        continue;
      svg << "<rect x='" << x << "' y='" << y
          << "' width='1' height='1' fill='#" << std::hex << std::setw(6)
          << std::setfill('0') << (c >> 8) << std::dec << "' opacity='"
          << (c & 255) / 255. << "'/>";
    }
  svg << "</svg>";
  auto id = storeAsset(svg.str(), "svg");
  if (id.empty())
    return false;
  auto &costumes = target()["costumes"];
  costumes.push_back(
      {{"assetId", id},
       {"name", uniqueName(costumes, name)},
       {"md5ext", id + ".svg"},
       {"dataFormat", "svg"},
       {"bitmapResolution", 1},
       {"rotationCenterX", std::isfinite(centerX) ? centerX : width / 2.},
       {"rotationCenterY", std::isfinite(centerY) ? centerY : height / 2.}});
  target()["currentCostume"] = costumes.size() - 1;
  error.clear();
  return true;
}
bool Project::costumePixels(int index, std::vector<uint32_t> &pixels,
                            int &width, int &height) const {
  const auto &costumes = doc_["targets"][target_]["costumes"];
  if (index < 0 || index >= (int)costumes.size())
    return false;
  const auto &c = costumes[index];
  auto bytes = asset(c.value("md5ext", ""));
  if (bytes.empty())
    return false;
  pixels.assign(1024, 0);
  if (c.value("dataFormat", "") == "svg") {
    auto svg = lunasvg::Document::loadFromData(
        std::string(bytes.begin(), bytes.end()));
    if (!svg)
      return false;
    width = std::clamp((int)svg->width(), 1, 4096);
    height = std::clamp((int)svg->height(), 1, 4096);
    if (svg->width() <= 0 || svg->height() <= 0)
      return true;
    auto bm = svg->renderToBitmap(32, 32);
    bm.convertToRGBA();
    if (!bm.data())
      return false;
    for (int i = 0; i < 1024; i++) {
      auto p = bm.data() + i * 4;
      pixels[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                  (uint32_t)p[2] << 8 | p[3];
    }
    return true;
  }
  int channels;
  if (!stbi_info_from_memory(bytes.data(), bytes.size(), &width, &height,
                             &channels) ||
      width > 1024 || height > 1024)
    return false;
  auto *raw = stbi_load_from_memory(bytes.data(), bytes.size(), &width, &height,
                                    &channels, 4);
  if (!raw)
    return false;
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      auto p = raw + ((y * height / 32) * width + x * width / 32) * 4;
      pixels[y * 32 + x] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                           (uint32_t)p[2] << 8 | p[3];
    }
  stbi_image_free(raw);
  return true;
}
static bool decodePcm(const std::vector<unsigned char> &bytes,
                      std::vector<int16_t> &out, int &rate) {
  if (bytes.empty())
    return false;
  std::vector<int16_t> interleaved;
  uint64_t frames = 0;
  unsigned channels = 0;
  drwav wav{};
  if (drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr)) {
    rate = wav.sampleRate;
    channels = wav.channels;
    frames = wav.totalPCMFrameCount;
    if (!rate || channels < 1 || channels > 2 || frames > 1000000) {
      drwav_uninit(&wav);
      return false;
    }
    interleaved.resize(frames * channels);
    frames = drwav_read_pcm_frames_s16(&wav, frames, interleaved.data());
    drwav_uninit(&wav);
  } else {
    drmp3 mp3{};
    if (!drmp3_init_memory(&mp3, bytes.data(), bytes.size(), nullptr))
      return false;
    rate = mp3.sampleRate;
    channels = mp3.channels;
    frames = drmp3_get_pcm_frame_count(&mp3);
    if (!rate || channels < 1 || channels > 2 || frames > 1000000) {
      drmp3_uninit(&mp3);
      return false;
    }
    drmp3_seek_to_pcm_frame(&mp3, 0);
    interleaved.resize(frames * channels);
    frames = drmp3_read_pcm_frames_s16(&mp3, frames, interleaved.data());
    drmp3_uninit(&mp3);
  }
  if (!frames)
    return false;
  out.resize(frames);
  for (size_t i = 0; i < frames; i++) {
    int value = interleaved[i * channels];
    if (channels == 2)
      value = (value + interleaved[i * 2 + 1]) / 2;
    out[i] = value;
  }
  return true;
}
bool Project::soundPcm(int index, std::vector<int16_t> &samples,
                       int &rate) const {
  const auto &sounds = doc_["targets"][target_]["sounds"];
  return index >= 0 && index < (int)sounds.size() &&
         decodePcm(asset(sounds[index].value("md5ext", "")), samples, rate);
}
static std::string wavBytes(const std::vector<int16_t> &samples, int rate) {
  std::string data(44 + samples.size() * 2, '\0');
  auto put = [&](int at, uint32_t v, int n) {
    for (int i = 0; i < n; i++)
      data[at + i] = v >> (8 * i);
  };
  memcpy(&data[0], "RIFF", 4);
  put(4, data.size() - 8, 4);
  memcpy(&data[8], "WAVEfmt ", 8);
  put(16, 16, 4);
  put(20, 1, 2);
  put(22, 1, 2);
  put(24, rate, 4);
  put(28, rate * 2, 4);
  put(32, 2, 2);
  put(34, 16, 2);
  memcpy(&data[36], "data", 4);
  put(40, samples.size() * 2, 4);
  for (size_t i = 0; i < samples.size(); i++)
    put(44 + i * 2, (uint16_t)samples[i], 2);
  return data;
}
bool Project::editSound(int index, double from, double to, double gain,
                        bool reverse, bool fadeIn, bool fadeOut) {
  std::vector<int16_t> samples;
  int rate;
  if (!soundPcm(index, samples, rate)) {
    error = "Edit WAV/MP3 sounds with at most 1 million frames";
    return false;
  }
  if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(gain) ||
      from < 0 || to > 100 || from >= to || gain < 0 || gain > 8) {
    error = "Use a nonempty selection between 0 and 100 percent";
    return false;
  }
  size_t first = samples.size() * from / 100, last = samples.size() * to / 100;
  if (last <= first) {
    error = "Sound selection is empty";
    return false;
  }
  std::vector<int16_t> edited(samples.begin() + first, samples.begin() + last);
  if (reverse)
    std::reverse(edited.begin(), edited.end());
  for (size_t i = 0; i < edited.size(); i++) {
    double volume = gain;
    if (fadeIn)
      volume *= i / (double)std::max<size_t>(1, edited.size() - 1);
    if (fadeOut)
      volume *= 1 - i / (double)std::max<size_t>(1, edited.size() - 1);
    edited[i] = std::clamp((int)std::lround(edited[i] * volume), -32768, 32767);
  }
  auto id = storeAsset(wavBytes(edited, rate), "wav");
  if (id.empty())
    return false;
  auto &sound = target()["sounds"][index];
  sound["assetId"] = id;
  sound["md5ext"] = id + ".wav";
  sound["dataFormat"] = "wav";
  sound["format"] = "";
  sound["rate"] = rate;
  sound["sampleCount"] = edited.size();
  error.clear();
  return true;
}
bool Project::addTone(const std::string &name, double frequency,
                      double seconds) {
  if (!std::isfinite(frequency) || !std::isfinite(seconds) || frequency < 20 ||
      frequency > 5000 || seconds < .05 || seconds > 10) {
    error = "Tone frequency 20-5000 Hz, duration 0.05-10 seconds";
    return false;
  }
  int rate = 22050;
  std::vector<int16_t> samples((size_t)(rate * seconds));
  for (size_t i = 0; i < samples.size(); i++)
    samples[i] = std::sin(6.283185307179586 * frequency * i / rate) * 12000 *
                 std::min(1., i / 220.) *
                 std::min(1., (samples.size() - i) / 220.);
  auto id = storeAsset(wavBytes(samples, rate), "wav");
  if (id.empty())
    return false;
  auto &sounds = target()["sounds"];
  sounds.push_back({{"assetId", id},
                    {"name", uniqueName(sounds, name)},
                    {"md5ext", id + ".wav"},
                    {"dataFormat", "wav"},
                    {"format", ""},
                    {"rate", rate},
                    {"sampleCount", samples.size()}});
  error.clear();
  return true;
}
bool Project::importAsset(const std::string &path, bool sound) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Could not open asset";
    return false;
  }
  file.seekg(0, std::ios::end);
  auto length = file.tellg();
  if (length <= 0 || length > 8 * 1024 * 1024) {
    error = "Import files up to 8 MiB";
    return false;
  }
  file.seekg(0);
  std::string bytes((size_t)length, '\0');
  if (!file.read(&bytes[0], bytes.size())) {
    error = "Could not read asset";
    return false;
  }
  auto dot = path.find_last_of('.');
  auto extension = dot == std::string::npos ? "" : path.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  auto start = path.find_last_of('/');
  std::string name = path.substr(start == std::string::npos ? 0 : start + 1);
  if (name.find_last_of('.') != std::string::npos)
    name.resize(name.find_last_of('.'));
  if (sound) {
    std::vector<int16_t> pcm;
    int rate;
    if (!decodePcm(std::vector<unsigned char>(bytes.begin(), bytes.end()), pcm,
                   rate)) {
      error = "Import WAV or MP3 sounds with at most 1 million frames";
      return false;
    }
    auto id = storeAsset(wavBytes(pcm, rate), "wav");
    if (id.empty())
      return false;
    auto &sounds = target()["sounds"];
    sounds.push_back({{"assetId", id},
                      {"name", uniqueName(sounds, name)},
                      {"md5ext", id + ".wav"},
                      {"dataFormat", "wav"},
                      {"rate", rate},
                      {"sampleCount", pcm.size()}});
  } else {
    int w = 0, h = 0, channels = 0;
    if (extension == "svg") {
      auto doc = lunasvg::Document::loadFromData(bytes);
      if (!doc) {
        error = "Invalid SVG";
        return false;
      }
      w = std::clamp((int)doc->width(), 1, 4096);
      h = std::clamp((int)doc->height(), 1, 4096);
    } else if (extension != "png" ||
               !stbi_info_from_memory((const unsigned char *)bytes.data(),
                                      bytes.size(), &w, &h, &channels) ||
               w > 1024 || h > 1024) {
      error = "Import SVG or PNG images (PNG up to 1024x1024)";
      return false;
    }
    auto id = storeAsset(bytes, extension);
    if (id.empty())
      return false;
    auto &costumes = target()["costumes"];
    costumes.push_back({{"assetId", id},
                        {"name", uniqueName(costumes, name)},
                        {"md5ext", id + "." + extension},
                        {"dataFormat", extension},
                        {"bitmapResolution", 1},
                        {"rotationCenterX", w / 2.},
                        {"rotationCenterY", h / 2.}});
    target()["currentCostume"] = costumes.size() - 1;
  }
  error.clear();
  return true;
}
bool Project::renameAsset(bool sound, int index, const std::string &name) {
  auto &collection = target()[sound ? "sounds" : "costumes"];
  if (index < 0 || index >= (int)collection.size() || name.empty() ||
      name.size() > 64) {
    error = "Use a name of 1 to 64 characters";
    return false;
  }
  for (int i = 0; i < (int)collection.size(); i++)
    if (i != index && collection[i].value("name", "") == name) {
      error = "That asset name already exists";
      return false;
    }
  auto old = collection[index].value("name", "");
  collection[index]["name"] = name;
  std::string key = sound                              ? "SOUND_MENU"
                    : target().value("isStage", false) ? "BACKDROP"
                                                       : "COSTUME";
  for (int i = 0; i < (int)doc_["targets"].size(); i++)
    if (i == target_ || key == "BACKDROP")
      for (auto &b : doc_["targets"][i]["blocks"]) {
        if (b.contains("fields") && b["fields"].contains(key) &&
            b["fields"][key][0] == old)
          b["fields"][key][0] = name;
        if (b.contains("inputs") && b["inputs"].contains(key))
          for (auto &v : b["inputs"][key])
            if (v.is_array() && v.size() == 2 && v[0] == 10 && v[1] == old)
              v[1] = name;
      }
  error.clear();
  return true;
}
bool Project::deleteAsset(bool sound, int index) {
  auto &collection = target()[sound ? "sounds" : "costumes"];
  if (index < 0 || index >= (int)collection.size() ||
      (!sound && collection.size() < 2)) {
    error = "Keep at least one costume or backdrop";
    return false;
  }
  collection.erase(collection.begin() + index);
  if (!sound) {
    int selected = target().value("currentCostume", 0);
    if (index < selected)
      selected--;
    target()["currentCostume"] =
        std::clamp(selected, 0, (int)collection.size() - 1);
  }
  error.clear();
  return true;
}
} // namespace PaxEdit
