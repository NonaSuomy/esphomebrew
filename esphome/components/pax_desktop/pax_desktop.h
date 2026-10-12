#pragma once
#include <cstdlib>
#include <string>
#include "esphome/core/log.h"
#include "esphome/core/component.h"
#include "lvgl.h"
#include <SDL.h>

namespace esphome::pax_desktop {
// Desktop UI adapter. It deliberately does not execute RISC-V PAX binaries.
class Runtime : public Component {
 public:
  void bind(lv_obj_t *status, lv_obj_t *storage, lv_obj_t *network, lv_obj_t *github) {
    status_ = status;
    buttons_[0] = storage;
    buttons_[1] = network;
    buttons_[2] = github;
    smoke_ = std::getenv("PAX_SDL_SMOKE_TEST") != nullptr;
  }
  void request_launch(const std::string &path) {
    ESP_LOGI("pax_desktop", "UI preview selected: %s", path.c_str());
    if (status_ != nullptr) {
      const std::string text = "UI preview: " + path;
      lv_label_set_text(status_, text.c_str());
    }
    requests_++;
  }
  void tick() {
    if (!smoke_ || status_ == nullptr) return;
    // Use the actual SDL event path, including mouse touch and keyboard focus.
    if (ticks_ == 10 || ticks_ == 30 || ticks_ == 50) {
      const int index = (ticks_ - 10) / 20;
      lv_area_t bounds;
      lv_obj_get_coords(buttons_[index], &bounds);
      last_x_ = (bounds.x1 + bounds.x2) / 2;
      last_y_ = (bounds.y1 + bounds.y2) / 2;
      mouse(SDL_MOUSEBUTTONDOWN);
    }
    if (ticks_ == 16 || ticks_ == 36 || ticks_ == 56) mouse(SDL_MOUSEBUTTONUP);
    if (ticks_ == 70) {
      lv_group_focus_obj(buttons_[0]);
      key(SDL_KEYDOWN, SDLK_RIGHT);
    }
    if (ticks_ == 76) key(SDL_KEYUP, SDLK_RIGHT);
    if (ticks_ == 82) key(SDL_KEYDOWN, SDLK_RETURN);
    if (ticks_ == 88) key(SDL_KEYUP, SDLK_RETURN);
    if (ticks_++ >= 110) {
      ESP_LOGI("pax_desktop", "SDL smoke test: %u launch selections", requests_);
      std::exit(requests_ == 4 ? 0 : 1);
    }
  }
 private:
  void mouse(uint32_t type) {
    SDL_Event event{};
    event.type = type;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
    event.button.x = last_x_;
    event.button.y = last_y_;
    SDL_PushEvent(&event);
  }
  void key(uint32_t type, SDL_Keycode code) {
    SDL_Event event{};
    event.type = type;
    event.key.keysym.sym = code;
    event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&event);
  }
  lv_obj_t *status_{nullptr};
  lv_obj_t *buttons_[3]{};
  unsigned requests_{0};
  unsigned ticks_{0};
  int last_x_{0};
  int last_y_{0};
  bool smoke_{false};
};
}  // namespace pax_desktop
