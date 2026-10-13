#pragma once

// The PAX canvas: the part of the panel an app draws.
//
// An app gets the ABI v1 canvas of 800x480 unless it asks for another size
// (psram_app.h display_get_size / display_set_canvas). The canvas is centred
// on the panel; the loader's three controls share a centered top toolbar that
// may slide upward off the panel while it auto-hides.
//
// Everything here is plain C++ with no ESP-IDF dependency, so it can be unit
// tested on a host (tests/cpp/test_pax_canvas.cpp).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace pax_loader {
namespace canvas {

// What apps that never ask for a size draw (and what every app starts with).
static constexpr int LEGACY_WIDTH = 800;
static constexpr int LEGACY_HEIGHT = 480;
// The panel this loader was written for, assumed when the display reports no
// usable size.
static constexpr int FALLBACK_PANEL_WIDTH = 1024;
static constexpr int FALLBACK_PANEL_HEIGHT = 600;
// The smallest and largest canvas an app may ask for (the largest is also
// capped at the panel).
static constexpr int MIN_WIDTH = 320;
static constexpr int MIN_HEIGHT = 240;
static constexpr int MAX_SIDE = 4096;

// The on-screen controls use one square button size and fixed gaps so they
// remain aligned as a group on every canvas size.
static constexpr int CLOSE_SIZE = 58;
static constexpr int CLOSE_GAP = 12;
// The legacy 800x480 canvas is centered on the 1024x600 panel, leaving a
// 60-pixel top margin.  Keep the launcher controls inside that margin so the
// close/volume/brightness overlays never flash over the first gameplay rows.
static constexpr int CLOSE_MARGIN = 0;
static constexpr int CLOSE_HIT_PADDING = 10;
static constexpr int RELOAD_SIZE = CLOSE_SIZE;
static constexpr int RELOAD_HIT_PADDING = CLOSE_HIT_PADDING;
static constexpr int VOLUME_SIZE = CLOSE_SIZE;
static constexpr int VOLUME_HIT_PADDING = CLOSE_HIT_PADDING;
static constexpr int BRIGHTNESS_SIZE = CLOSE_SIZE;
static constexpr int BRIGHTNESS_GAP = CLOSE_GAP;
static constexpr int BRIGHTNESS_HIT_PADDING = CLOSE_HIT_PADDING;
static constexpr int GAME_MENU_SIZE = CLOSE_SIZE;
static constexpr int GAME_MENU_GAP = CLOSE_GAP;
// When the in-app speaker is tapped, this horizontal control appears just to
// its left. It uses the same speaker button as the launcher and lets a user
// drag the level both down and back up without leaving the running app.
static constexpr int VOLUME_SLIDER_WIDTH = 240;
static constexpr int VOLUME_SLIDER_HEIGHT = VOLUME_SIZE;
static constexpr int VOLUME_SLIDER_GAP = 8;
static constexpr int BRIGHTNESS_SLIDER_WIDTH = VOLUME_SLIDER_WIDTH;
static constexpr int BRIGHTNESS_SLIDER_HEIGHT = VOLUME_SLIDER_HEIGHT;

// Framebuffer transfers (cache write-back, PPA output) work in whole cache
// lines. 128 covers the ESP32-P4's 64- and 128-byte line settings.
static constexpr size_t CACHE_LINE = 128;

inline size_t align_bytes(size_t bytes) { return (bytes + CACHE_LINE - 1) / CACHE_LINE * CACHE_LINE; }

// `bytes` rounded up to whole cache lines, but never past the allocation.
inline size_t sync_bytes(size_t bytes, size_t allocated) { return std::min(align_bytes(bytes), allocated); }

inline size_t frame_bytes(int width, int height) {
  return static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint16_t);
}

// ESP32-P4 SRM truncates scales to 1/16. Only accelerate exact ratios:
// otherwise part of the requested output rectangle is never written (and
// appears at the top/left after panel rotation). Software fills the full size.
inline bool ppa_scale_exact(int input, int output) {
  if (input <= 0 || output <= 0)
    return false;
  const int64_t ticks = static_cast<int64_t>(output) * 16;
  return ticks >= input && ticks < static_cast<int64_t>(input) * 4096 && ticks % input == 0;
}

// A canvas an app may use on a panel whose largest canvas is max_w x max_h:
// even sizes (the canvas is centred, and the panel turned 180 degrees), from
// MIN_WIDTH x MIN_HEIGHT up.
inline bool size_ok(int width, int height, int max_w, int max_h) {
  return width >= MIN_WIDTH && height >= MIN_HEIGHT && width <= max_w && height <= max_h && width <= MAX_SIDE &&
         height <= MAX_SIDE && width % 2 == 0 && height % 2 == 0;
}

// "1024x600" -> 1024, 600. Digits, an 'x' (or 'X'), digits; nothing else.
inline bool parse_size(const std::string &text, int *width, int *height) {
  const size_t x = text.find_first_of("xX");
  if (x == std::string::npos || x == 0 || x + 1 >= text.size() || x > 5 || text.size() - x - 1 > 5)
    return false;
  int values[2] = {0, 0};
  const std::string parts[2] = {text.substr(0, x), text.substr(x + 1)};
  for (int i = 0; i < 2; i++) {
    for (char c : parts[i]) {
      if (!std::isdigit(static_cast<unsigned char>(c)))
        return false;
      values[i] = values[i] * 10 + (c - '0');
    }
  }
  if (values[0] <= 0 || values[1] <= 0)
    return false;
  if (width != nullptr)
    *width = values[0];
  if (height != nullptr)
    *height = values[1];
  return true;
}

inline std::string format_size(int width, int height) {
  char text[24];
  std::snprintf(text, sizeof(text), "%dx%d", width, height);
  return text;
}

// Where the canvas and the close control are on a panel. "Screen"
// coordinates are the touch panel's (landscape, top-left origin). The panel's
// raw draw path is turned 180 degrees, so a rectangle at screen (x, y) is
// drawn at raw (panel_w - x - w, panel_h - y - h).
struct Geometry {
  int panel_w{FALLBACK_PANEL_WIDTH};
  int panel_h{FALLBACK_PANEL_HEIGHT};
  int canvas_w{LEGACY_WIDTH};
  int canvas_h{LEGACY_HEIGHT};

  int x() const { return (this->panel_w - this->canvas_w) / 2; }
  int y() const { return (this->panel_h - this->canvas_h) / 2; }
  int raw_x() const { return this->panel_w - this->x() - this->canvas_w; }
  int raw_y() const { return this->panel_h - this->y() - this->canvas_h; }

  int control_group_width() const {
    return CLOSE_SIZE + CLOSE_GAP + RELOAD_SIZE + CLOSE_GAP + BRIGHTNESS_SIZE +
           BRIGHTNESS_GAP + VOLUME_SIZE + GAME_MENU_GAP + GAME_MENU_SIZE;
  }
  int control_group_x() const {
    return std::max(0, (this->panel_w - this->control_group_width()) / 2);
  }
  int control_top_y() const {
    // Put the toolbar immediately above the canvas when letterboxing leaves
    // enough room.  A full-panel canvas has no reserved area, so it safely
    // falls back to the panel edge.
    return std::max(0, this->y() - CLOSE_SIZE + CLOSE_MARGIN);
  }
  // Kept as a compatibility query for the close-touch code. The close
  // control is now a dedicated toolbar button, not a canvas-side control.
  bool close_beside() const { return false; }
  int close_x(int slide = 0) const {
    (void) slide;
    return this->control_group_x();
  }
  int close_y(int slide = 0) const { return this->control_top_y() - slide; }
  int close_raw_x(int slide = 0) const {
    (void) slide;
    return this->panel_w - this->close_x() - CLOSE_SIZE;
  }
  int close_raw_y(int slide = 0) const { return this->panel_h - this->close_y(slide) - CLOSE_SIZE; }

  int reload_x(int slide = 0) const {
    (void) slide;
    return this->close_x() + CLOSE_SIZE + CLOSE_GAP;
  }
  int reload_y(int slide = 0) const { return this->close_y(slide); }
  int reload_raw_x(int slide = 0) const {
    (void) slide;
    return this->panel_w - this->reload_x() - RELOAD_SIZE;
  }
  int reload_raw_y(int slide = 0) const { return this->panel_h - this->reload_y(slide) - RELOAD_SIZE; }

  int brightness_x(int slide = 0) const {
    (void) slide;
    return this->reload_x() + RELOAD_SIZE + CLOSE_GAP;
  }
  int brightness_y(int slide = 0) const { return this->close_y(slide); }
  int brightness_raw_x(int slide = 0) const {
    (void) slide;
    return this->panel_w - this->brightness_x() - BRIGHTNESS_SIZE;
  }
  int brightness_raw_y(int slide = 0) const {
    return this->panel_h - this->brightness_y(slide) - BRIGHTNESS_SIZE;
  }

  int volume_x(int slide = 0) const {
    (void) slide;
    return this->brightness_x() + BRIGHTNESS_SIZE + BRIGHTNESS_GAP;
  }
  int volume_y(int slide = 0) const { return this->close_y(slide); }
  int volume_raw_x(int slide = 0) const {
    (void) slide;
    return this->panel_w - this->volume_x() - VOLUME_SIZE;
  }
  int volume_raw_y(int slide = 0) const { return this->panel_h - this->volume_y(slide) - VOLUME_SIZE; }

  int game_menu_x(int slide = 0) const {
    return this->volume_x(slide) + VOLUME_SIZE + GAME_MENU_GAP;
  }
  int game_menu_y(int slide = 0) const { return this->close_y(slide); }
  int game_menu_raw_x(int slide = 0) const {
    (void) slide;
    return this->panel_w - this->game_menu_x() - GAME_MENU_SIZE;
  }
  int game_menu_raw_y(int slide = 0) const {
    return this->panel_h - this->game_menu_y(slide) - GAME_MENU_SIZE;
  }

  int volume_slider_x() const {
    return std::max(0, (this->panel_w - VOLUME_SLIDER_WIDTH) / 2);
  }
  // Keep the slider row below the compact toolbar.  Placing it in the canvas
  // is important: the next full-frame transfer redraws this area after the
  // slider is hidden, so clearing the slider cannot leave an opaque black
  // rectangle in the letterbox above the game.
  int volume_slider_y() const { return this->control_top_y() + CLOSE_SIZE; }
  int volume_slider_raw_x() const { return this->panel_w - this->volume_slider_x() - VOLUME_SLIDER_WIDTH; }
  int volume_slider_raw_y() const { return this->panel_h - this->volume_slider_y() - VOLUME_SLIDER_HEIGHT; }

  int brightness_slider_x() const { return this->volume_slider_x(); }
  int brightness_slider_y() const { return this->volume_slider_y(); }
  int brightness_slider_raw_x() const { return this->panel_w - this->brightness_slider_x() - BRIGHTNESS_SLIDER_WIDTH; }
  int brightness_slider_raw_y() const { return this->panel_h - this->brightness_slider_y() - BRIGHTNESS_SLIDER_HEIGHT; }

  // A touch on the close control, with some padding around it.
  bool in_close(int sx, int sy, int slide = 0) const {
    return sx >= this->close_x() - CLOSE_HIT_PADDING && sx < this->close_x() + CLOSE_SIZE + CLOSE_HIT_PADDING &&
           sy >= this->close_y(slide) - CLOSE_HIT_PADDING && sy < this->close_y(slide) + CLOSE_SIZE + CLOSE_HIT_PADDING;
  }

  bool in_reload(int sx, int sy, int slide = 0) const {
    return sx >= this->reload_x() - RELOAD_HIT_PADDING &&
           sx < this->reload_x() + RELOAD_SIZE + RELOAD_HIT_PADDING &&
           sy >= this->reload_y(slide) - RELOAD_HIT_PADDING &&
           sy < this->reload_y(slide) + RELOAD_SIZE + RELOAD_HIT_PADDING;
  }

  bool in_volume(int sx, int sy, int slide = 0) const {
    return sx >= this->volume_x() - VOLUME_HIT_PADDING && sx < this->volume_x() + VOLUME_SIZE + VOLUME_HIT_PADDING &&
           sy >= this->volume_y(slide) - VOLUME_HIT_PADDING && sy < this->volume_y(slide) + VOLUME_SIZE + VOLUME_HIT_PADDING;
  }

  bool in_game_menu(int sx, int sy, int slide = 0) const {
    return sx >= this->game_menu_x(slide) - CLOSE_HIT_PADDING &&
           sx < this->game_menu_x(slide) + GAME_MENU_SIZE + CLOSE_HIT_PADDING &&
           sy >= this->game_menu_y(slide) - CLOSE_HIT_PADDING &&
           sy < this->game_menu_y(slide) + GAME_MENU_SIZE + CLOSE_HIT_PADDING;
  }

  bool in_volume_slider(int sx, int sy) const {
    return sx >= this->volume_slider_x() && sx < this->volume_slider_x() + VOLUME_SLIDER_WIDTH &&
           sy >= this->volume_slider_y() && sy < this->volume_slider_y() + VOLUME_SLIDER_HEIGHT;
  }

  bool in_brightness(int sx, int sy, int slide = 0) const {
    return sx >= this->brightness_x() - BRIGHTNESS_HIT_PADDING &&
           sx < this->brightness_x() + BRIGHTNESS_SIZE + BRIGHTNESS_HIT_PADDING &&
           sy >= this->brightness_y(slide) - BRIGHTNESS_HIT_PADDING &&
           sy < this->brightness_y(slide) + BRIGHTNESS_SIZE + BRIGHTNESS_HIT_PADDING;
  }

  bool in_brightness_slider(int sx, int sy) const {
    return sx >= this->brightness_slider_x() &&
           sx < this->brightness_slider_x() + BRIGHTNESS_SLIDER_WIDTH &&
           sy >= this->brightness_slider_y() &&
           sy < this->brightness_slider_y() + BRIGHTNESS_SLIDER_HEIGHT;
  }

  // Screen -> canvas coordinates; false outside the canvas.
  bool to_canvas(int sx, int sy, int *cx, int *cy) const {
    const int dx = sx - this->x(), dy = sy - this->y();
    if (dx < 0 || dy < 0 || dx >= this->canvas_w || dy >= this->canvas_h)
      return false;
    if (cx != nullptr)
      *cx = dx;
    if (cy != nullptr)
      *cy = dy;
    return true;
  }
};

// The per-app settings key: the file name without folders, query, ".pax" and
// a "-<version>" suffix, so a store download (psram_tulip-0.1.0.pax), an
// installed copy (/sd/roms/pax/psram_tulip.pax) and the listing's name
// (psram_tulip) agree.
inline std::string app_key(const std::string &path) {
  std::string name = path.substr(0, path.find_first_of("?#"));
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  if (name.size() > 4) {
    std::string ext = name.substr(name.size() - 4);
    for (char &c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".pax")
      name.resize(name.size() - 4);
  }
  // A version is digits in dot-separated groups: 0.1.0, 2, 10.4.
  const size_t dash = name.rfind('-');
  if (dash != std::string::npos && dash > 0 && dash + 1 < name.size()) {
    bool version = true;
    bool digit = false;
    for (size_t i = dash + 1; i < name.size() && version; i++) {
      const char c = name[i];
      if (std::isdigit(static_cast<unsigned char>(c))) {
        digit = true;
      } else if (c == '.' && digit) {
        digit = false;
      } else {
        version = false;
      }
    }
    if (version && digit)
      name.resize(dash);
  }
  return name;
}

// The sizes the store's Screen setting offers (after "Default"), largest
// first, without repeats, each one that fits in max_w x max_h: the listing's
// own sizes when it names some, else the panel, 1024x600, 800x480 and 640x480.
inline std::vector<std::string> choices(int max_w, int max_h, const std::vector<std::string> &listed) {
  std::vector<std::pair<int, int>> sizes;
  if (listed.empty()) {
    sizes = {{max_w - max_w % 2, max_h - max_h % 2}, {1024, 600}, {800, 480}, {640, 480}};
  } else {
    for (const auto &text : listed) {
      int w = 0, h = 0;
      if (parse_size(text, &w, &h))
        sizes.emplace_back(w, h);
    }
  }
  std::stable_sort(sizes.begin(), sizes.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) {
    return static_cast<long>(a.first) * a.second > static_cast<long>(b.first) * b.second;
  });
  std::vector<std::string> result;
  for (const auto &size : sizes) {
    if (!size_ok(size.first, size.second, max_w, max_h))
      continue;
    const std::string text = format_size(size.first, size.second);
    if (std::find(result.begin(), result.end(), text) == result.end())
      result.push_back(text);
  }
  return result;
}

// The listing's recommended size ("800x480"), normalised, when this panel can
// show it and it is one of the app's `listed` sizes (an app that takes any
// size, listed empty, may recommend any); "" otherwise.
inline std::string recommended(int max_w, int max_h, const std::vector<std::string> &listed,
                               const std::string &wanted) {
  int w = 0, h = 0;
  if (wanted.empty() || !parse_size(wanted, &w, &h) || !size_ok(w, h, max_w, max_h))
    return {};
  bool found = listed.empty();
  for (const auto &text : listed) {
    int lw = 0, lh = 0;
    found = found || (parse_size(text, &lw, &lh) && lw == w && lh == h);
  }
  return found ? format_size(w, h) : std::string();
}

// The sizes the Screen setting offers: choices(), plus the recommended size
// (from recommended()) when an app that takes any size names one of its own.
inline std::vector<std::string> offered(int max_w, int max_h, const std::vector<std::string> &listed,
                                        const std::string &recommended_size) {
  std::vector<std::string> sizes = choices(max_w, max_h, listed);
  if (!recommended_size.empty() && std::find(sizes.begin(), sizes.end(), recommended_size) == sizes.end()) {
    sizes.push_back(recommended_size);
    sizes = choices(max_w, max_h, sizes);  // largest first again
  }
  return sizes;
}

// What one press of the store's Screen button steps through, in order. ""
// stands for "no saved setting". Without a recommended size that is
// "Default" (the device's canvas) and comes first; with one (`recommended`
// from above) it is the recommended size, in its place among the others.
// `offered` is from offered().
inline std::vector<std::string> screen_options(const std::vector<std::string> &offered,
                                               const std::string &recommended_size) {
  std::vector<std::string> options;
  if (recommended_size.empty())
    options.emplace_back();
  for (const auto &size : offered)
    options.push_back(size == recommended_size ? std::string() : size);
  return options;
}

// The option after `current` (the saved setting, "" for none); a setting that
// is no longer offered goes back to "".
inline std::string next_screen_option(const std::vector<std::string> &options, const std::string &current) {
  for (size_t i = 0; i < options.size(); i++) {
    if (options[i] == current)
      return options[(i + 1) % options.size()];
  }
  return {};
}

}  // namespace canvas
}  // namespace pax_loader
}  // namespace esphome
