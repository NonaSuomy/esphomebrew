/* Dashboard menu actions must emit browser key chords without losing Alt state.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define app_entry desktop_entry
#include "../main.c"
#undef app_entry
static pax_game_menu_page_t captured;
static int closed;
static int64_t fake_now(void) { return 0; }
static int set_page(const pax_game_menu_page_t *p) {
  captured = *p;
  return 0;
}
static void close_menu(void) { closed++; }
static unsigned usage(unsigned n) {
  return relay.outgoing[n * 13 + 4] | (unsigned)relay.outgoing[n * 13 + 5] << 8;
}
int main(void) {
  app_services_t api = {0};
  api.game_menu_set_page = set_page;
  api.game_menu_close = close_menu;
  api.get_time_us = fake_now;
  svc = &api;
  relay.io.now = relay_now;
  menu_action(NULL, 0, 0);
  assert(captured.count == 6 && captured.items[3].action == 6);
  menu_action(NULL, 6, 0);
  assert(dashboard_mode && listen_requested && closed == 1);
  relay_active = 1;
  relay_peer = -1;
  menu_action(NULL, 0, 0);
  assert(captured.count == 4 &&
         captured.items[1].flags == PAX_GAME_MENU_ITEM_DISABLED);
  relay_peer = 7;
  relay.input_enabled = 1;
  menu_action(NULL, 0, 0);
  assert(!captured.items[1].flags && !captured.items[2].flags);
  menu_action(NULL, 4, 0);
  assert(relay.out_end == 26 && usage(0) == 0x3e && usage(1) == 0x3e);
  assert(relay.outgoing[7] == 1 && relay.outgoing[20] == 0);
  relay.out_end = 0;
  menu_action(NULL, 5, 0);
  assert(relay.out_end == 52 && !relay.modifiers);
  assert(usage(0) == 0xe2 && usage(1) == 0x50 && usage(2) == 0x50 &&
         usage(3) == 0xe2);
  assert(relay.outgoing[19] == 4 && relay.outgoing[32] == 4);
  relay.out_end = 0;
  relay.modifiers = 4;
  menu_action(NULL, 5, 0);
  assert(relay.out_end == 26 && relay.modifiers == 4);
  assert(usage(0) == 0x50 && usage(1) == 0x50);
  relay.wire_mode = 2;
  menu_action(NULL, 0, 0);
  assert(captured.count == 5 && captured.items[1].action == 8);
  menu_action(NULL, 8, 0);
  assert(relay.avi_paused);
  menu_action(NULL, 10, 0);
  assert(relay_muted);
  menu_action(NULL, 9, 0);
  assert(relay.avi_stop);
  api.mem_alloc = malloc;
  api.mem_free = free;
  browser_inputs(1u << PAX_INPUT_X, 0, 0, 0);
  assert(browser_action == 3);
  browser->sd = 1;
  browser_inputs(1u << PAX_INPUT_X, 0, 0, 0);
  assert(browser_action == 2);
  browser_inputs(1u << PAX_INPUT_START, 0, 0, 0);
  assert(browser_action == 8);
  browser_inputs(1u << PAX_INPUT_SELECT, 0, 0, 0);
  assert(browser_action == 9);
  browser_inputs(1u << PAX_INPUT_L, 0, 0, 0);
  assert(browser_action == 10);
  browser_free();
  puts("Dashboard menu: entry, disconnected guards, F5 and Alt+Left passed");
  return 0;
}
