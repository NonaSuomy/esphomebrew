#define PAX_LOADER_USE_LVGL 1
#define PAX_APP_SIDE 1
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "pax_network_setup.h"
extern "C" {
void lv_mem_init() {}
void lv_mem_deinit() {}
void* lv_malloc_core(size_t n) { return std::malloc(n); }
void lv_free_core(void* p) { std::free(p); }
void* lv_realloc_core(void* p, size_t n) { return std::realloc(p, n); }
lv_result_t lv_mem_test_core() { return LV_RESULT_OK; }
void lv_mem_monitor_core(lv_mem_monitor_t* p) { std::memset(p, 0, sizeof(*p)); }
}
static int mode = 0, scans = 0, applies = 0;
static pax_wifi_request_t submitted{};
static pax_wifi_scan_status_t result{PAX_WIFI_SCAN_MAGIC};
static lv_obj_t* find(lv_obj_t* parent, const char* text) {
  if (lv_obj_check_type(parent, &lv_label_class) && std::strstr(lv_label_get_text(parent), text)) return parent;
  for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++)
    if (auto* child = find(lv_obj_get_child(parent, i), text)) return child;
  return nullptr;
}
static lv_obj_t* find_type(lv_obj_t* parent, const lv_obj_class_t* type, int& skip) {
  if (lv_obj_check_type(parent, type) && skip-- == 0) return parent;
  for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++)
    if (auto* child = find_type(lv_obj_get_child(parent, i), type, skip)) return child;
  return nullptr;
}
static lv_obj_t* typed(const lv_obj_class_t* type, int skip = 0) {
  auto* p = find_type(lv_layer_top(), type, skip);
  assert(p);
  return p;
}
static void click(const char* text) {
  auto* label = find(lv_layer_top(), text);
  assert(label);
  lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, nullptr);
}
int main() {
  lv_init();
  lv_display_create(1024, 600);
  auto* old = lv_group_create();
  lv_group_set_default(old);
  auto* input = lv_indev_create();
  lv_indev_set_type(input, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_group(input, old);
  pax_native_network::Backend backend{};
  backend.capabilities = 3;
  backend.mode = []() { return mode; };
  backend.select = [](int selected) {
    mode = selected;
    return 0;
  };
  backend.network_get = [](pax_network_config_t* n) {
    *n = {};
    n->connected = 1;
    n->network_type = PAX_NETWORK_WIFI;
    n->dhcp_enabled = 1;
    n->ipv4 = 0xc0a80102;
    n->netmask = 0xffffff00;
    return 0;
  };
  backend.network_set = [](const pax_network_config_t*) { return 0; };
  backend.wifi_get = [](pax_wifi_status_t* s) {
    *s = {};
    s->magic = PAX_WIFI_SETTINGS_MAGIC;
    std::strcpy(s->ssid, "Existing");
    s->password_set = 1;
    return true;
  };
  backend.scan_get = [](pax_wifi_scan_status_t* s) {
    *s = result;
    return true;
  };
  backend.scan_start = []() {
    scans++;
    result.state = 2;
    result.generation++;
    result.count = 1;
    std::strcpy(result.aps[0].ssid, "TestNetwork");
    result.aps[0].secured = 1;
    result.aps[0].rssi = -35;
    return 0;
  };
  backend.wifi_apply = [](const pax_wifi_request_t* r) {
    submitted = *r;
    applies++;
    return 0;
  };
  pax_native_network::Panel panel;
  panel.open(backend);
  lv_obj_update_layout(lv_layer_top());
  assert(panel.active() && lv_indev_get_group(input) != old);
  auto* ip = typed(&lv_textarea_class);
  auto* dhcp = typed(&lv_checkbox_class);
  assert(lv_obj_has_flag(ip, LV_OBJ_FLAG_HIDDEN));
  lv_obj_remove_state(dhcp, LV_STATE_CHECKED);
  lv_obj_send_event(dhcp, LV_EVENT_VALUE_CHANGED, nullptr);
  assert(!lv_obj_has_flag(ip, LV_OBJ_FLAG_HIDDEN));
  lv_obj_send_event(ip, LV_EVENT_FOCUSED, nullptr);
  auto* keyboard = typed(&lv_keyboard_class);
  assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
  lv_obj_add_state(dhcp, LV_STATE_CHECKED);
  lv_obj_send_event(dhcp, LV_EVENT_VALUE_CHANGED, nullptr);
  assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
  click("SCAN NEARBY");
  panel.tick(1000);
  assert(scans == 1);
  click("TestNetwork");
  auto* ssid = typed(&lv_textarea_class, 4);
  auto* password = typed(&lv_textarea_class, 5);
  assert(!std::strcmp(lv_textarea_get_text(ssid), "TestNetwork"));
  click("APPLY WI-FI");
  assert(applies == 0);
  lv_textarea_set_text(password, "password123");
  click("APPLY WI-FI");
  assert(applies == 1 && !std::strcmp(submitted.ssid, "TestNetwork") &&
         !std::strcmp(submitted.password, "password123") && submitted.replace_password == 1);
  assert(!*lv_textarea_get_text(password));
  click("ETHERNET");
  assert(mode == 2);
  panel.key(27);
  assert(!panel.active() && lv_indev_get_group(input) == old);
  panel.open(backend);
  panel.close();
  assert(lv_indev_get_group(input) == old);
  std::puts("Native LVGL panel: DHCP, keyboard, scan, staging, validation, apply, selection and close passed");
}
