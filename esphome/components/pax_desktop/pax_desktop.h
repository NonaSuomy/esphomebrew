#pragma once
#include <cstdlib>
#include <string>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
#include "esphome/core/log.h"
#include "esphome/core/component.h"
#include "lvgl.h"
#include <SDL.h>

namespace esphome::pax_desktop {
// Shared UI adapter: run selected PAX images in the separate desktop ABI host.
class Runtime : public Component {
 public:
  void set_network_url(const std::string &url) { network_url_ = url; }
  void bind(lv_obj_t *status, lv_obj_t *storage, lv_obj_t *network, lv_obj_t *github) {
    status_ = status;
    buttons_[0] = storage;
    buttons_[1] = network;
    buttons_[2] = github;
    smoke_ = std::getenv("PAX_SDL_SMOKE_TEST") != nullptr;
    launch_smoke_ = std::getenv("PAX_SDL_LAUNCH_SMOKE_TEST") != nullptr;
  }
  void request_launch(const std::string &selected_path) {
    std::string path = selected_path;
    if (path.rfind("http://", 0) == 0) {
      if (const char *override_url = std::getenv("PAX_DESKTOP_NETWORK_URL")) path = override_url;
    }
    ESP_LOGI("pax_desktop", "UI preview selected: %s", path.c_str());
    requests_++;
    if (smoke_) { status("UI selection: " + path); return; }
    if (child_ > 0) { status("Launcher is already running"); return; }
    std::filesystem::path root;
    if (const char *configured = std::getenv("PAX_ROOT")) root = configured;
    else {
      root = std::filesystem::current_path();
      while (!root.empty() && !std::filesystem::exists(root / "pax-tools/desktop/launch.py")) {
        auto parent = root.parent_path();
        if (parent == root) { root.clear(); break; }
        root = parent;
      }
    }
    if (root.empty()) { status("Cannot find PAX workspace; start with build-firmware.sh run desktop"); return; }
    const std::string python = (root / "pax-tools/python-desktop/bin/python").string();
    std::string network_url = network_url_;
    if (const char *override_url = std::getenv("PAX_DESKTOP_NETWORK_URL")) network_url = override_url;
    const std::string launcher = (root / "pax-tools/desktop/launch.py").string();
    if (!std::filesystem::exists(python)) { status("Run pax-tools/setup-desktop.sh first"); return; }
    status("Opening " + path);
    child_ = fork();
    if (child_ == 0) {
      execl(python.c_str(), python.c_str(), launcher.c_str(), path.c_str(), "--network-url", network_url.c_str(), static_cast<char *>(nullptr));
      _exit(127);
    }
    if (child_ < 0) { child_ = -1; status("Unable to start desktop PAX runner"); return; }
    window_ = SDL_GetKeyboardFocus();
    if (window_ != nullptr) SDL_HideWindow(window_);
  }
  void tick() {
    if (child_ > 0) {
      int result = 0;
      if (waitpid(child_, &result, WNOHANG) == child_) {
        child_ = -1;
        const bool success = WIFEXITED(result) && WEXITSTATUS(result) == 0;
        status(success ? "Launcher closed | select Storage, Network or GitHub" : "Launch failed; see terminal output for details");
        if (window_ != nullptr) { SDL_ShowWindow(window_); SDL_RaiseWindow(window_); }
        if (launch_smoke_) std::exit(success ? 0 : 1);
      }
    }
    if (launch_smoke_) {
      if (ticks_ == 10) {
        lv_area_t bounds;
        lv_obj_get_coords(buttons_[1], &bounds);
        last_x_ = (bounds.x1 + bounds.x2) / 2;
        last_y_ = (bounds.y1 + bounds.y2) / 2;
        mouse(SDL_MOUSEBUTTONDOWN);
      }
      if (ticks_ == 16) mouse(SDL_MOUSEBUTTONUP);
      if (ticks_++ >= 600) std::exit(1);
      return;
    }
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
  void status(const std::string &text) {
    if (status_ != nullptr) lv_label_set_text(status_, text.c_str());
  }
  std::string network_url_{"http://10.13.37.84:8000/launcher_ui.pax"};
  pid_t child_{-1};
  SDL_Window *window_{nullptr};
  bool launch_smoke_{false};
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
