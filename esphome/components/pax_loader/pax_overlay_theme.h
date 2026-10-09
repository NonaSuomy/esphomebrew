#pragma once
#include <cstdint>

namespace esphome {
namespace pax_loader {
namespace overlay_theme {
// Same RGB palette as launcher_ui; direct panel overlays use native RGB565.
constexpr uint16_t rgb565(uint32_t rgb) {
  return static_cast<uint16_t>(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}
static constexpr uint16_t BACKGROUND = rgb565(0x071525);
static constexpr uint16_t PANEL = rgb565(0x102844);
static constexpr uint16_t SELECTED = rgb565(0x15699D);
static constexpr uint16_t ACCENT = rgb565(0x25C7FF);
static constexpr uint16_t TEXT = rgb565(0xF2F7FF);
static constexpr uint16_t MUTED = rgb565(0x8BA7C4);
static constexpr uint16_t ERROR = rgb565(0xF87171);
}  // namespace overlay_theme
}  // namespace pax_loader
}  // namespace esphome
