#pragma once
#include <cstdint>
#include "pax_theme_profile.h"

namespace esphome {
namespace pax_loader {
namespace overlay_theme {
// Same RGB palette as launcher_ui; direct panel overlays use native RGB565.
constexpr uint16_t rgb565(uint32_t rgb) {
  return static_cast<uint16_t>(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}
inline uint16_t BACKGROUND = rgb565(0x071525);
inline uint16_t PANEL = rgb565(0x102844);
inline uint16_t SELECTED = rgb565(0x15699D);
inline uint16_t ACCENT = rgb565(0x25C7FF);
inline uint16_t TEXT = rgb565(0xF2F7FF);
inline uint16_t MUTED = rgb565(0x8BA7C4);
inline uint16_t ERROR = rgb565(0xF87171);
inline void apply(const pax_theme_profile_t &theme) {
 BACKGROUND=rgb565(theme.colors[PAX_THEME_BACKGROUND]);PANEL=rgb565(theme.colors[PAX_THEME_PANEL]);
 SELECTED=rgb565(theme.colors[PAX_THEME_SELECTED]);ACCENT=rgb565(theme.colors[PAX_THEME_ACCENT]);
 TEXT=rgb565(theme.colors[PAX_THEME_TEXT]);MUTED=rgb565(theme.colors[PAX_THEME_MUTED]);ERROR=rgb565(theme.colors[PAX_THEME_ERROR]);
}
}  // namespace overlay_theme
}  // namespace pax_loader
}  // namespace esphome
