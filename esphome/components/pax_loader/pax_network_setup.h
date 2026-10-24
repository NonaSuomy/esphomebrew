#pragma once
#ifdef PAX_LOADER_USE_LVGL
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "lvgl.h"
#include "pax_wifi_scan.h"
#include "pax_wifi_settings.h"
#include "psram_app.h"
namespace pax_native_network {
struct Backend {
  uint32_t capabilities;  // 1=Wi-Fi, 2=Ethernet
  int (*mode)();
  int (*select)(int);
  int (*network_get)(pax_network_config_t*);
  int (*network_set)(const pax_network_config_t*);
  bool (*wifi_get)(pax_wifi_status_t*);
  int (*wifi_apply)(const pax_wifi_request_t*);
  bool (*scan_get)(pax_wifi_scan_status_t*);
  int (*scan_start)();
};
class Panel {
 public:
  bool active() const { return panel_ != nullptr; }
  void open(const Backend& backend) {
    if (active()) return;
    wifi_ = {};
    scan_result_ = {};
    keyboard_ = nullptr;
    backend_ = backend;
    dirty_ = false;
    password_changed_ = false;
    needs_password_ = false;
    scan_generation_ = UINT32_MAX;
    last_refresh_ = 0;
    selected_mode_ = -1;
    editing_ = nullptr;
    password_visible_ = false;
    saved_default_ = lv_group_get_default();
    group_ = lv_group_create();
    lv_group_set_default(group_);
    panel_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(panel_, lv_pct(100), lv_pct(100));
    lv_obj_center(panel_);
    lv_obj_remove_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(panel_, 8, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(0x061326), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(panel_, lv_color_hex(0xD7F3FF), 0);
    lv_obj_t* title = label(panel_, "NETWORK SETUP");
    lv_obj_set_width(title, lv_pct(65));
    lv_obj_set_pos(title, 8, 8);
    back_ = button(panel_, "BACK", 1);
    lv_obj_set_size(back_, 100, 40);
    lv_obj_align(back_, LV_ALIGN_TOP_RIGHT, -4, 0);
    body_ = lv_obj_create(panel_);
    lv_obj_set_width(body_, lv_pct(100));
    lv_obj_set_pos(body_, 0, 50);
    lv_obj_set_style_bg_opa(body_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body_, 0, 0);
    lv_obj_set_style_pad_all(body_, 4, 0);
    lv_obj_set_style_pad_row(body_, 10, 0);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body_, LV_DIR_VER);
    status_ = label(body_, "Reading network status...");
    lv_obj_t* modes = row(body_);
    mode_buttons_[0] = button(modes, "AUTO", 10);
    mode_buttons_[1] = button(modes, "WI-FI", 11);
    mode_buttons_[2] = button(modes, "ETHERNET", 12);
    for (auto* item : mode_buttons_) lv_obj_set_width(item, lv_pct(31));
    if (!(backend_.capabilities & 1)) lv_obj_add_state(mode_buttons_[1], LV_STATE_DISABLED);
    if (!(backend_.capabilities & 2)) lv_obj_add_state(mode_buttons_[2], LV_STATE_DISABLED);
    label(body_, "IP ADDRESS SETTINGS");
    lv_obj_t* dhcp_row = row(body_);
    lv_obj_t* dhcp_caption = label(dhcp_row, "DHCP (automatic IP)");
    lv_obj_set_width(dhcp_caption, lv_pct(75));
    dhcp_ = lv_checkbox_create(dhcp_row);
    lv_checkbox_set_text(dhcp_, "");
    lv_obj_set_user_data(dhcp_, reinterpret_cast<void*>(7));
    wire(dhcp_, LV_EVENT_VALUE_CHANGED);
    wire(dhcp_, LV_EVENT_KEY);
    const char* names[] = {"IP address", "Subnet mask", "Gateway", "DNS server"};
    for (int i = 0; i < 4; i++) ip_[i] = field(body_, names[i], "0.0.0.0", 15, 10 + i, false);
    network_apply_ = button(body_, "APPLY IP SETTINGS", 20);
    label(body_, "WI-FI NETWORK");
    scan_ = button(body_, "SCAN NEARBY", 30);
    aps_ = lv_list_create(body_);
    lv_obj_set_width(aps_, lv_pct(100));
    lv_obj_set_height(aps_, 150);
    lv_obj_set_style_bg_color(aps_, lv_color_hex(0x0B2A4A), 0);
    lv_obj_add_flag(aps_, LV_OBJ_FLAG_HIDDEN);
    scan_status_ = label(body_, "Scan nearby, or enter a Wi-Fi name manually.");
    ssid_ = field(body_, "Wi-Fi name (SSID)", "Network name", 32, 1, false);
    password_ = field(body_, "Wi-Fi password", "Leave blank for an open network", 64, 2, true);
    lv_obj_t* actions = row(body_);
    show_ = button(actions, "SHOW PASSWORD", 31);
    apply_ = button(actions, "APPLY WI-FI", 32);
    lv_obj_set_width(show_, lv_pct(48));
    lv_obj_set_width(apply_, lv_pct(48));
    if (!(backend_.capabilities & 1)) {
      for (auto* object : {scan_, ssid_, password_, show_, apply_}) lv_obj_add_state(object, LV_STATE_DISABLED);
      lv_label_set_text(scan_status_, "Wi-Fi is not configured in this firmware.");
    }
    message_ = label(body_, "Changes apply only when you choose an interface or press APPLY.");
    keyboard_ = lv_keyboard_create(panel_);
    lv_obj_set_width(keyboard_, lv_pct(100));
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    wire(keyboard_, LV_EVENT_READY);
    wire(keyboard_, LV_EVENT_CANCEL);
    wire(keyboard_, LV_EVENT_KEY);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_group_set_default(saved_default_);
    inputs_count_ = 0;
    lv_indev_t* input = nullptr;
    while ((input = lv_indev_get_next(input)) && inputs_count_ < 16) {
      if (lv_indev_get_type(input) == LV_INDEV_TYPE_KEYPAD || lv_indev_get_type(input) == LV_INDEV_TYPE_ENCODER) {
        inputs_[inputs_count_] = input;
        old_groups_[inputs_count_++] = lv_indev_get_group(input);
        lv_indev_set_group(input, group_);
      }
    }
    resize(false);
    refresh(true);
    lv_group_focus_obj(back_);
  }
  void close() {
    if (!active()) return;
    wipe_password();
    for (unsigned i = 0; i < inputs_count_; i++) lv_indev_set_group(inputs_[i], old_groups_[i]);
    lv_obj_t* old = panel_;
    panel_ = nullptr;
    lv_obj_delete(old);
    lv_group_delete(group_);
    group_ = nullptr;
    editing_ = nullptr;
  }
  void key(int key) {
    if (!active()) return;
    if (key == 27) {
      if (!lv_obj_has_flag(keyboard_, LV_OBJ_FLAG_HIDDEN))
        hide_keyboard();
      else
        close();
      return;
    }
    if (!editing_) return;
    if (key == 13) {
      hide_keyboard();
      return;
    }
    if (key == 8 || key == 127)
      lv_textarea_delete_char(editing_);
    else if (key >= 32 && key <= 126)
      lv_textarea_add_char(editing_, static_cast<uint32_t>(key));
  }
  void tick(uint32_t now) {
    if (active() && (uint32_t)(now - last_refresh_) >= 1000) {
      last_refresh_ = now;
      refresh(false);
    }
  }

 private:
  Backend backend_{};
  lv_obj_t *panel_ = nullptr, *body_ = nullptr, *back_ = nullptr, *status_ = nullptr, *message_ = nullptr;
  lv_obj_t *mode_buttons_[3]{}, *dhcp_ = nullptr, *ip_[4]{}, *ip_labels_[4]{}, *network_apply_ = nullptr,
                                *scan_ = nullptr, *aps_ = nullptr;
  lv_obj_t *scan_status_ = nullptr, *ssid_ = nullptr, *password_ = nullptr, *show_ = nullptr, *apply_ = nullptr,
           *keyboard_ = nullptr, *editing_ = nullptr;
  lv_group_t *group_ = nullptr, *saved_default_ = nullptr, *old_groups_[16]{};
  lv_indev_t* inputs_[16]{};
  unsigned inputs_count_ = 0;
  bool syncing_ = false, dirty_ = false, password_changed_ = false, needs_password_ = false, password_visible_ = false;
  uint32_t last_refresh_ = 0, scan_generation_ = UINT32_MAX;
  int selected_mode_ = -1;
  pax_wifi_status_t wifi_{};
  pax_wifi_scan_status_t scan_result_{};
  void wire(lv_obj_t* object, lv_event_code_t code) {
    lv_obj_add_event_cb(
        object, [](lv_event_t* event) { static_cast<Panel*>(lv_event_get_user_data(event))->event(event); }, code,
        this);
  }
  lv_obj_t* label(lv_obj_t* parent, const char* text) {
    lv_obj_t* object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_width(object, lv_pct(100));
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(object, lv_color_hex(0xD7F3FF), 0);
    return object;
  }
  lv_obj_t* row(lv_obj_t* parent) {
    lv_obj_t* object = lv_obj_create(parent);
    lv_obj_set_size(object, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(object, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
  }
  lv_obj_t* button(lv_obj_t* parent, const char* text, int action) {
    lv_obj_t* object = lv_button_create(parent);
    lv_obj_set_size(object, lv_pct(100), 42);
    lv_obj_set_style_pad_all(object, 5, 0);
    lv_obj_set_user_data(object, reinterpret_cast<void*>(static_cast<uintptr_t>(action)));
    lv_obj_t* caption = lv_label_create(object);
    lv_label_set_text(caption, text);
    lv_obj_center(caption);
    wire(object, LV_EVENT_CLICKED);
    wire(object, LV_EVENT_KEY);
    return object;
  }
  lv_obj_t* field(lv_obj_t* parent, const char* name, const char* placeholder, unsigned max, int kind, bool secret) {
    lv_obj_t* caption = label(parent, name);
    if (kind >= 10 && kind <= 13) ip_labels_[kind - 10] = caption;
    lv_obj_t* object = lv_textarea_create(parent);
    lv_obj_set_size(object, lv_pct(100), 42);
    lv_textarea_set_one_line(object, true);
    lv_textarea_set_max_length(object, max);
    lv_textarea_set_placeholder_text(object, placeholder);
    lv_textarea_set_password_mode(object, secret);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x0B2A4A), 0);
    lv_obj_set_style_text_color(object, lv_color_hex(0xD7F3FF), 0);
    lv_obj_set_user_data(object, reinterpret_cast<void*>(static_cast<uintptr_t>(kind)));
    wire(object, LV_EVENT_FOCUSED);
    wire(object, LV_EVENT_CLICKED);
    wire(object, LV_EVENT_VALUE_CHANGED);
    wire(object, LV_EVENT_KEY);
    return object;
  }
  void resize(bool keyboard) {
    int h = lv_obj_get_height(panel_);
    if (h < 100) h = lv_display_get_vertical_resolution(lv_obj_get_display(panel_));
    int kh = h / 3;
    if (kh < 140) kh = 140;
    if (kh > 240) kh = 240;
    lv_obj_set_height(keyboard_, kh);
    lv_obj_set_height(body_, h - 70 - (keyboard ? kh : 0));
  }
  void hide_keyboard() {
    lv_obj_t* previous = editing_;
    editing_ = nullptr;
    lv_keyboard_set_textarea(keyboard_, nullptr);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    resize(false);
    lv_group_set_editing(group_, false);
    lv_group_focus_obj(previous == ssid_ || previous == password_ ? apply_ : network_apply_);
  }
  void wipe_password() {
    const char* value = lv_textarea_get_text(password_);
    if (value && *value) pax_wifi_wipe(const_cast<char*>(value), std::strlen(value));
    syncing_ = true;
    lv_textarea_set_text(password_, "");
    syncing_ = false;
  }
  static void address_text(uint32_t ip, char* out, size_t size) {
    lv_snprintf(out, size, "%u.%u.%u.%u", static_cast<unsigned>(ip >> 24), static_cast<unsigned>((ip >> 16) & 255),
                static_cast<unsigned>((ip >> 8) & 255), static_cast<unsigned>(ip & 255));
  }
  static bool address_value(const char* text, uint32_t& value) {
    value = 0;
    for (int part = 0; part < 4; part++) {
      unsigned byte = 0, digits = 0;
      while (*text >= '0' && *text <= '9') {
        byte = byte * 10 + static_cast<unsigned>(*text++ - '0');
        if (++digits > 3 || byte > 255) return false;
      }
      if (!digits) return false;
      value = (value << 8) | byte;
      if (part < 3) {
        if (*text++ != '.') return false;
      } else if (*text)
        return false;
    }
    return true;
  }
  void refresh(bool initial) {
    if (!active()) return;
    syncing_ = true;
    int mode = backend_.mode();
    if (mode != selected_mode_) {
      for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(mode_buttons_[i], lv_color_hex(i == mode ? 0x14527A : 0x0B2A4A), 0);
      selected_mode_ = mode;
    }
    pax_network_config_t network{};
    if (backend_.network_get(&network) == 0) {
      char ip[20], mask[20], gateway[20], dns[20], summary[240];
      address_text(network.ipv4, ip, sizeof(ip));
      address_text(network.netmask, mask, sizeof(mask));
      address_text(network.gateway, gateway, sizeof(gateway));
      address_text(network.dns, dns, sizeof(dns));
      bool connected = network.connected && network.network_type != PAX_NETWORK_NONE;
      lv_snprintf(summary, sizeof(summary), "%s | %s | %s\nIP %s   Mask %s\nGateway %s   DNS %s",
                  network.network_type == PAX_NETWORK_ETHERNET ? "Ethernet"
                  : network.network_type == PAX_NETWORK_WIFI   ? "Wi-Fi"
                                                               : "No active link",
                  connected ? "Connected" : "Disconnected", network.dhcp_enabled ? "DHCP" : "Static IPv4",
                  connected ? ip : "Not assigned", mask, gateway, dns);
      if (std::strcmp(lv_label_get_text(status_), summary)) lv_label_set_text(status_, summary);
      if (initial || (!dirty_ && !editing_)) {
        if (network.dhcp_enabled)
          lv_obj_add_state(dhcp_, LV_STATE_CHECKED);
        else
          lv_obj_remove_state(dhcp_, LV_STATE_CHECKED);
        const char* values[] = {ip, mask, gateway, dns};
        for (int i = 0; i < 4; i++)
          if (std::strcmp(lv_textarea_get_text(ip_[i]), values[i])) lv_textarea_set_text(ip_[i], values[i]);
      }
    }
    bool automatic = lv_obj_has_state(dhcp_, LV_STATE_CHECKED);
    for (int i = 0; i < 4; i++) {
      for (auto* object : {ip_[i], ip_labels_[i]}) {
        if (automatic)
          lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        else
          lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
      }
    }
    pax_wifi_status_t wifi{};
    if (backend_.wifi_get(&wifi)) {
      if (initial) lv_textarea_set_text(ssid_, wifi.ssid);
      if (wifi.status != wifi_.status && !initial) {
        if (wifi.status == 1)
          lv_label_set_text(message_, "Connecting to Wi-Fi...");
        else if (wifi.status == 2)
          lv_label_set_text(message_, "Wi-Fi connected. Credentials saved; IP details update here.");
        else if (wifi.status == -1)
          lv_label_set_text(message_, "Connection failed; restoring the previous working Wi-Fi.");
        else if (wifi.status == -2)
          lv_label_set_text(message_, "Not connected. Check the selected name and password; retry when ready.");
      }
      wifi_ = wifi;
    }
    pax_wifi_scan_status_t scan{};
    if (backend_.scan_get(&scan) && scan.generation != scan_generation_) {
      scan_generation_ = scan.generation;
      scan_result_ = scan;
      lv_label_set_text(scan_status_, scan.message[0] ? scan.message : "Scan nearby, or enter a Wi-Fi name manually.");
      lv_obj_clean(aps_);
      if (scan.state == 2 && scan.count) {
        lv_obj_remove_flag(aps_, LV_OBJ_FLAG_HIDDEN);
        for (uint32_t i = 0; i < scan.count && i < 24; i++) {
          char name[33], text[96];
          std::memcpy(name, scan.aps[i].ssid, 33);
          for (char* p = name; *p; p++)
            if (static_cast<unsigned char>(*p) < 32) *p = '?';
          lv_snprintf(text, sizeof(text), "%s  %ld dBm  %s", name, static_cast<long>(scan.aps[i].rssi),
                      scan.aps[i].secured ? "Secured" : "Open");
          lv_obj_t* item = lv_list_add_button(aps_, nullptr, text);
          lv_obj_set_user_data(item, reinterpret_cast<void*>(static_cast<uintptr_t>(100 + i)));
          wire(item, LV_EVENT_CLICKED);
          wire(item, LV_EVENT_KEY);
        }
      } else
        lv_obj_add_flag(aps_, LV_OBJ_FLAG_HIDDEN);
    }
    syncing_ = false;
  }
  void event(lv_event_t* event) {
    lv_obj_t* object = static_cast<lv_obj_t*>(lv_event_get_target(event));
    lv_event_code_t code = lv_event_get_code(event);
    int action = static_cast<int>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(object)));
    if (code == LV_EVENT_KEY) {
      if (lv_event_get_key(event) == LV_KEY_ESC) key(27);
      return;
    }
    if (object == keyboard_) {
      hide_keyboard();
      return;
    }
    if (keyboard_ &&
        (code == LV_EVENT_FOCUSED || (code == LV_EVENT_CLICKED && lv_obj_check_type(object, &lv_textarea_class)))) {
      editing_ = object;
      lv_keyboard_set_textarea(keyboard_, object);
      lv_obj_remove_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
      resize(true);
      lv_obj_scroll_to_view_recursive(object, LV_ANIM_OFF);
      return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
      if (syncing_) return;
      if (object == dhcp_ || action >= 10) {
        if (object == dhcp_ && editing_) hide_keyboard();
        dirty_ = true;
        refresh(false);
      } else if (object == password_)
        password_changed_ = true;
      else if (object == ssid_) {
        needs_password_ = false;
      }
      return;
    }
    if (code != LV_EVENT_CLICKED) return;
    if (action == 1) {
      close();
      return;
    }
    if (action >= 10 && action <= 12) {
      if (editing_) hide_keyboard();
      int result = backend_.select(action - 10);
      if (result == 0) dirty_ = false;
      lv_label_set_text(message_, result == 0    ? "Network interface choice saved."
                                  : result == -2 ? "Wait for the current scan or connection attempt."
                                                 : "This interface is unavailable, or the choice could not be saved.");
      refresh(false);
      return;
    }
    if (action == 20) {
      pax_network_config_t config{};
      config.dhcp_enabled = lv_obj_has_state(dhcp_, LV_STATE_CHECKED);
      uint32_t values[4]{};
      bool valid = true;
      for (int i = 0; i < 4; i++) valid = address_value(lv_textarea_get_text(ip_[i]), values[i]) && valid;
      config.ipv4 = values[0];
      config.netmask = values[1];
      config.gateway = values[2];
      config.dns = values[3];
      if (!config.dhcp_enabled && !valid) {
        lv_label_set_text(message_, "Enter valid IPv4 addresses, for example 192.168.1.20.");
        return;
      }
      int result = backend_.network_set(&config);
      if (result == 0) dirty_ = false;
      lv_label_set_text(message_, result == 0 ? "IP settings saved. Waiting for current connection details..."
                                              : "IP settings rejected; check the interface, address and subnet mask.");
      refresh(false);
      return;
    }
    if (action == 30) {
      int result = backend_.scan_start();
      lv_label_set_text(scan_status_, result == 0
                                          ? "Scanning nearby Wi-Fi networks..."
                                          : "Select Wi-Fi or AUTO, then retry when the current operation finishes.");
      refresh(false);
      return;
    }
    if (action == 31) {
      password_visible_ = !password_visible_;
      lv_textarea_set_password_mode(password_, !password_visible_);
      lv_label_set_text(lv_obj_get_child(show_, 0), password_visible_ ? "HIDE PASSWORD" : "SHOW PASSWORD");
      return;
    }
    if (action >= 100 && action < 124) {
      unsigned index = static_cast<unsigned>(action - 100);
      if (index >= scan_result_.count) return;
      auto& ap = scan_result_.aps[index];
      bool current = std::strcmp(ap.ssid, wifi_.ssid) == 0;
      syncing_ = true;
      lv_textarea_set_text(ssid_, ap.ssid);
      syncing_ = false;
      if (!current || !ap.secured) {
        wipe_password();
        password_changed_ = true;
      }
      needs_password_ = ap.secured && !current;
      lv_label_set_text(message_, "Network selected. Enter its password if needed, then APPLY WI-FI.");
      lv_obj_scroll_to_view_recursive(needs_password_ ? password_ : ssid_, LV_ANIM_OFF);
      return;
    }
    if (action == 32) {
      pax_wifi_request_t request{};
      request.magic = PAX_WIFI_SETTINGS_MAGIC;
      lv_snprintf(request.ssid, sizeof(request.ssid), "%s", lv_textarea_get_text(ssid_));
      lv_snprintf(request.password, sizeof(request.password), "%s", lv_textarea_get_text(password_));
      request.replace_password = password_changed_ || std::strcmp(request.ssid, wifi_.ssid) != 0;
      if ((needs_password_ && !request.password[0]) || !pax_wifi_request_valid(&request)) {
        pax_wifi_wipe(&request, sizeof(request));
        lv_label_set_text(message_,
                          "Enter a network name and a valid password (8-63 characters, or blank for an open network).");
        return;
      }
      int result = backend_.wifi_apply(&request);
      pax_wifi_wipe(&request, sizeof(request));
      if (result == 0) {
        wipe_password();
        password_changed_ = false;
        needs_password_ = false;
        hide_keyboard();
      }
      lv_label_set_text(message_, result == 0
                                      ? "Connecting to the selected Wi-Fi network..."
                                      : "Wi-Fi request rejected; finish the current scan/connection and try again.");
      return;
    }
  }
};
}  // namespace pax_native_network
#endif
