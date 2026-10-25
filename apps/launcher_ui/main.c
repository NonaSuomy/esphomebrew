#include "pax_wifi_settings.h"
#include "pax_wifi_scan.h"
#include "pax_c6_update.h"
#include "pax_firmware_update.h"
#include "rom_profiles.h"
/*
 * launcher_ui — phase 2 LVGL PAX.
 *
 * This is the first real launcher screen behind the PAX boundary. The PAX
 * owns its LVGL 9.5 instance, but every LVGL call is dispatched to the
 * loader's ESPHome task through ui_call_sync(). The entry task owns the model
 * and pumps input/UI while a single cancellable worker handles catalog I/O.
 */

#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "iso9660_extract.h"

/* Font Awesome Free Solid 5.15.4 subset used by the original ESPHome UI.
 * Keep the glyphs compact so they fit the launcher's small icon buttons. */
#include "fontawesome_icons.c"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "input_repeat.h"
#include "list_presentation.h"
#include "network_file_info.h"
#include "list_sort.h"
#include "pax_theme_profile.h"

#define DEFAULT_PANEL_W 800
#define DEFAULT_PANEL_H 480
#define DEFAULT_CANVAS_W 400
#define DEFAULT_CANVAS_H 240
#define MAX_APPS       192
#define VISIBLE_ROWS   5
#define NAME_BYTES     64
#define PATH_BYTES     256
#define SEARCH_QUERY_BYTES 384
#define SEARCH_ENCODED_BYTES (SEARCH_QUERY_BYTES * 3)
#define SEARCH_URL_BYTES 1408
#define LIST_BYTES     8192
#define REMOTE_BYTES   204800 /* 192 KiB listing plus HTTP headers */
#define DETAIL_TEXT_BYTES 1024
#define FAVORITES_FILE "/sd/roms/pax/.launcher_ui_favorites"
#define BACKGROUND_PREF_FILE "/sd/roms/pax/.launcher_ui_background"
#define ARCHIVE_COLLECTION_FILE "/sd/roms/pax/.launcher_ui_archive_collection"
#define ARCHIVE_LOGIN_FILE "/sd/roms/pax/.launcher_ui_archive_login"
#define ARCHIVE_S3_KEYS_FILE "/sd/roms/pax/.launcher_ui_archive_s3"
#define APP_DETAILS_PREF_FILE "/sd/roms/pax/.launcher_ui_app_details"
#define DETAIL_COVER_TEMP_SD "/sd/roms/pax/.launcher_ui_detail_cover.png"
#define DETAIL_COVER_TEMP_USB "/usb0/roms/pax/.launcher_ui_detail_cover.png"
#define DISC_INSTALLER_PAX "/sd/roms/pax/recipe.pax"
#define DEFAULT_ARCHIVE_COLLECTION "open_source_software"
#define MAX_BACKGROUNDS 32
#define SETTINGS_FOCUS_COUNT 38
#define TOOLTIPS_PREF_FILE "/sd/roms/pax/.launcher_ui_tooltips"
#define LIST_PREF_FILE "/sd/roms/pax/.launcher_ui_list_view"
#define LIST_LABEL_BYTES (NAME_BYTES + 120)
#define DETAIL_ACTION_COUNT 5
#define DETAIL_ACTIONS_DEFAULT 4
#define MAX_ROM_CHOICES 128
#define ROM_PICKER_VISIBLE_ROWS 6
#define MAX_DETAIL_FILES 24
#define MAX_EXTRACT_MEDIA 4
#define MAX_EXTRACT_FILES 24
#define EXTRACT_MAX_IMAGE_BYTES (1024U * 1024U * 1024U)
#define EXTRACT_MAX_FILE_BYTES (128U * 1024U * 1024U)

#define SOURCE_STORAGE 0
#define SOURCE_NETWORK 1
#define SOURCE_STORE    2
#define SOURCE_ARCHIVE  3
#define SOURCE_COUNT    4
#define TOOLBAR_BUTTON_COUNT 12
#define ARCHIVE_LEVEL_SEARCH 0
#define ARCHIVE_LEVEL_ITEM   1
#define ARCHIVE_LEVEL_ZIP    2
#define ARCHIVE_RECORD_ITEM   1
#define ARCHIVE_RECORD_FILE   2
#define ARCHIVE_RECORD_ZIP    3
#define ARCHIVE_RECORD_MEMBER 4

/* lv_color_hex() takes RGB888, not RGB565.  Keeping these as RGB888 also
 * makes the text colors predictable on both the host-owned and PAX-owned
 * LVGL display configurations. */
#define COL_BG (g_theme.colors[0])
#define COL_PANEL (g_theme.colors[1])
#define COL_SELECTED (g_theme.colors[2])
#define COL_ACCENT (g_theme.colors[3])
#define COL_TEXT (g_theme.colors[4])
#define COL_MUTED (g_theme.colors[5])
#define COL_OK (g_theme.colors[6])
#define COL_WARN (g_theme.colors[7])
#define COL_ERROR (g_theme.colors[8])

static const app_services_t *g_svc;
static uint16_t *g_canvas;
/* Keep the directory records in the PAX image.  The loader service can write
 * into this memory while enumerating, and the worker can then pass a path
 * back to app_open() without relying on a service-allocated pointer being
 * visible in both address spaces. */
static char g_names[MAX_APPS][NAME_BYTES];
/* Display titles never replace the canonical package names or paths. */
static char g_titles[MAX_APPS][NAME_BYTES];
static char g_paths[MAX_APPS][PATH_BYTES];
static int g_item_source[MAX_APPS];
static int g_visible_indices[MAX_APPS];
static int g_view_count;
static char g_favorite_paths[MAX_APPS][PATH_BYTES];
static int g_favorite_count;
static bool g_favorites_mode;
static bool g_settings_mode;
static bool g_keyboard_open;
static pax_theme_profile_t g_theme;
static char g_network_url[PATH_BYTES];
static bool g_network_auto=true;
#define MAX_THEMES 20
static pax_theme_profile_t g_themes[MAX_THEMES];
static int g_theme_count, g_theme_index, g_theme_direction=1;
static bool g_app_details_enabled = true;
static bool g_tooltips_enabled = true;
static lv_obj_t *g_tooltips_value;
static bool g_hide_pax_extension, g_list_file_details;
static int g_sort_mode, g_sort_direction=1;
static int g_size_unit, g_size_direction=1;
static pax_file_stat_t g_list_stats[MAX_APPS];
static bool g_list_stats_valid[MAX_APPS];
static char g_detail_file_info[PATH_BYTES + 240];
static bool g_auto_brightness_enabled;
static volatile bool g_filter_dirty;
static volatile bool g_settings_dirty;
static char g_search_text[PATH_BYTES];
static int g_archive_level;
static char g_archive_identifier[128];
static char g_archive_title[NAME_BYTES];
static char g_archive_collection[64] = DEFAULT_ARCHIVE_COLLECTION;
static char g_archive_email[96];
static char g_archive_password[96];
static char g_archive_s3_access[96];
static char g_archive_s3_secret[128];
static char g_archive_s3_status[96] = "Keys not configured";
static bool g_archive_s3_access_revealed;
static bool g_archive_s3_secret_revealed;
static int g_archive_auth_edit_field = -1;
static bool g_archive_authenticated;
static char g_archive_auth_status[80] = "Not signed in";
static char g_archive_zip_url[PATH_BYTES];
static char g_archive_status[128];
static bool g_archive_download_active;
static bool g_archive_search_pending;
static uint32_t g_archive_search_changed_ms;
static int g_archive_record_type[MAX_APPS];
static char g_archive_filenames[MAX_APPS][128];
static char g_background_paths[MAX_BACKGROUNDS][PATH_BYTES];
static char g_background_options[MAX_BACKGROUNDS * (NAME_BYTES + 1) + 8];
static int g_background_count;
static int g_background_index;
static int g_background_choice;
static bool g_background_dropdown_open;
static uint16_t *g_background_pixels;
static uint16_t g_background_width;
static uint16_t g_background_height;
static lv_image_dsc_t g_background_descriptor;
static lv_obj_t *g_background_widget,*g_work_label;
static bool g_settings_scan_pending,g_settings_scan_busy,g_settings_reload_network;
static bool g_action_busy,g_transfer_busy,g_transfer_verify_pending;
static char g_transfer_name[65],g_transfer_message[160];

static int g_volume_level;
static int g_brightness_level;
static int g_settings_focus;
static pax_device_status_t g_device_status;
static pax_network_config_t g_network_config;
static pax_network_config_t g_network_live;
static bool g_network_config_dirty;
static uint32_t g_connection_poll_ms;
static pax_storage_status_t g_storage_status[PAX_STORAGE_COUNT];
static int g_storage_app_count[PAX_STORAGE_COUNT];
static bool g_storage_status_inferred[PAX_STORAGE_COUNT];
static bool g_storage_path_accessible[PAX_STORAGE_COUNT];
static int g_network_edit_field = -1;
static pax_wifi_status_t g_wifi_status;
static bool g_wifi_available, g_wifi_password_changed;
static char g_wifi_ssid[33], g_wifi_password[65];
static pax_wifi_scan_status_t g_wifi_scan;
static bool g_wifi_scan_available,g_wifi_dropdown_open,g_wifi_needs_password;
static uint32_t g_wifi_scan_poll_ms,g_wifi_scan_started_ms;
static int g_wifi_choice;
static lv_obj_t *g_wifi_dropdown;
static lv_obj_t *g_wifi_scan_message_label;
static char g_wifi_options[2300];
static int update_wifi_picker_on_host(void *unused);
static lv_obj_t *g_network_extra_labels[4];
static char g_search_before_settings_edit[sizeof(g_search_text)];
static char g_settings_message[128];
static char g_c6_source[512] = "board-recommended";
static char g_c6_checked_source[512];
static pax_c6_status_t g_c6_status;
static bool g_c6_available;
static lv_obj_t *g_c6_labels[4];
static pax_firmware_status_t g_firmware_status;
static bool g_firmware_available, g_firmware_confirm;
static uint64_t g_firmware_confirm_build;
static uint32_t g_firmware_last_state = UINT32_MAX;
static lv_obj_t *g_firmware_labels[3];
static lv_obj_t *g_c6_progress;
static uint32_t g_c6_last_state;
static int g_keyboard_focus;
static int g_toolbar_focus = -1;
static int g_pending_key;
static bool g_pending_key_valid;
static void save_favorites(void);
static bool ends_with(const char *text, const char *suffix);
static bool extract_ends_with_ci(const char *text, const char *suffix);
static bool extract_has_char(const char *text, char wanted);
static const char *detail_action_text(int action);
static int update_selection_on_host(void *arg);
/* Keep only the directory scratch buffer static.  The visible names must be
 * copied into the loader task's own stack: LVGL and its label allocator run
 * there, while PAX BSS is not a reliable backing store for host-side text. */
static char g_listing[LIST_BYTES];
static char g_remote[REMOTE_BYTES];
/* Login and HTTP scratch stays out of the 8 KiB PAX worker stack. The
 * launcher performs these synchronous requests serially on that one worker. */
static char g_http_request[SEARCH_URL_BYTES + 1400];
static char g_http_cookie[1100];
static char g_archive_encoded_email[288];
static char g_archive_encoded_password[288];
static char g_archive_form_body[640];
static char g_archive_logged_in_user[512];
static char g_archive_logged_in_sig[512];
static int g_app_count;
static bool g_catalog_failed;
static volatile int g_selected;
static int g_first_visible;
static lv_point_t g_list_touch_start;
static lv_point_t g_list_touch_last;
static int g_list_touch_scroll_accum;
static int g_list_touch_row_step;
static bool g_list_touch_tracking;
static bool g_list_touch_swiped;
static volatile int g_detail_index = -1;
static int g_detail_focus;
static lv_obj_t *settings_text(lv_obj_t *parent, const char *text, int x, int y,
                               int width, const lv_font_t *font, bool scroll);
static lv_obj_t *settings_button(lv_obj_t *parent, int focus, int x, int y,
                                 int width, int height, int action);
static char g_detail_name[NAME_BYTES];
static char g_detail_title[NAME_BYTES];
static char g_detail_version[32];
static char g_detail_local_version[32];
static char g_detail_description[DETAIL_TEXT_BYTES];
static char g_detail_author[96];
static char g_detail_category[48];
static char g_detail_license[96];
static char g_detail_controls[256];
typedef struct {
  char path[PATH_BYTES];
  char note[128];
  bool optional;
  bool download;
  bool found;
} detail_file_t;
typedef struct {
  char source[128];
  char target[PATH_BYTES];
  bool optional;
} extract_file_t;
typedef struct {
  char format[24];
  char entry[192];
  char zip_name[96];
  bool auto_entry;
  int first_file;
  int file_count;
} extract_media_t;
typedef struct {
  bool valid;
  char archive_dir[PATH_BYTES];
  int media_count;
  int file_count;
  extract_media_t media[MAX_EXTRACT_MEDIA];
  extract_file_t files[MAX_EXTRACT_FILES];
} extract_recipe_t;
static detail_file_t g_detail_files[MAX_DETAIL_FILES];
static int g_detail_file_count;
static extract_recipe_t g_detail_extract_recipe;
static int g_detail_action_count = DETAIL_ACTIONS_DEFAULT;
static const pax_rom_profile_t *g_rom_picker_profile;
static bool g_rom_picker_active;
static bool g_rom_prepare_busy;
static uint64_t g_rom_prepare_started;
static bool g_rom_picker_rom_selected;
static int g_rom_picker_count;
static int g_rom_picker_app_index = -1;
static volatile int g_rom_picker_selected;
static int g_rom_picker_first;
static int g_rom_picker_rows_shown;
static char g_rom_picker_names[MAX_ROM_CHOICES][NAME_BYTES];
static char g_rom_picker_paths[MAX_ROM_CHOICES][PATH_BYTES];
static lv_obj_t *g_detail_file_state_labels[MAX_DETAIL_FILES];
static char g_detail_sha256[65];
static char g_detail_local_path[PATH_BYTES];
static char g_detail_recipe_path[PATH_BYTES];
static int g_resume_detail_record = -1;
static uint32_t g_detail_size;
static bool g_detail_install_valid;
static bool g_detail_is_remote;
static bool g_detail_is_installed;
static bool g_detail_update_available;
static bool g_detail_uninstall_confirm;
static uint16_t *g_detail_cover_pixels;
static uint16_t g_detail_cover_width;
static uint16_t g_detail_cover_height;
static lv_image_dsc_t g_detail_cover_descriptor;
static uint32_t g_install_state;
static uint32_t g_install_done;
static uint32_t g_install_total;
static char g_install_status[96];
static volatile int g_open_index;
static volatile int g_source;
static bool g_pad_last[PAX_INPUT_MAX];
static PaxInputRepeat g_nav_repeat[6];
static bool g_keyboard_nav_down[4];
static bool g_keyboard_nav_pending[4];
static bool g_keyboard_enter_down;
static bool g_keyboard_enter_pending;

static lv_display_t *g_display;
static lv_indev_t *g_indev;
static lv_indev_t *g_mouse_indev;
static lv_obj_t *g_mouse_cursor;
static lv_coord_t g_mouse_x;
static lv_coord_t g_mouse_y;
static int g_mouse_buttons;
static bool g_mouse_cursor_seen;
static uint32_t g_mouse_last_activity_ms;
#define MOUSE_CURSOR_IDLE_MS 3000U
static volatile int g_mouse_wheel_pending;
static lv_obj_t *g_list;
static lv_obj_t *g_list_scrollbar;
static lv_obj_t *g_list_scrollbar_thumb;
static int g_list_scrollbar_height;
static lv_obj_t *g_settings_panel;
static lv_obj_t *g_detail_info_view;
static lv_obj_t *g_count_label;
static lv_obj_t *g_detail_buttons[DETAIL_ACTION_COUNT];
static lv_obj_t *g_detail_button_labels[DETAIL_ACTION_COUNT];
static lv_obj_t *g_rom_picker_rows[ROM_PICKER_VISIBLE_ROWS];
static lv_obj_t *g_rom_picker_row_labels[ROM_PICKER_VISIBLE_ROWS];
static lv_obj_t *g_rom_picker_status;
static lv_obj_t *g_detail_status_label;
static lv_obj_t *g_detail_progress;
static lv_obj_t *g_search_area;
static lv_obj_t *g_archive_up_button;
static lv_obj_t *g_keyboard;
static lv_obj_t *g_volume_slider;
static lv_obj_t *g_brightness_slider;
static lv_obj_t *g_volume_value;
static lv_obj_t *g_brightness_value;
static lv_obj_t *g_settings_back_button;
static lv_obj_t *g_app_details_switch;
static lv_obj_t *g_list_switches[2];
static lv_obj_t *g_theme_value_label;
static lv_obj_t *g_sort_value_label;
static lv_obj_t *g_size_value_label;
static lv_obj_t *g_auto_brightness_button;
static lv_obj_t *g_auto_brightness_state_label;
static lv_obj_t *g_empty_label;
static lv_obj_t *g_background_button;
static lv_obj_t *g_background_value_label;
static lv_obj_t *g_background_dropdown;
static lv_obj_t *g_archive_collection_button;
static lv_obj_t *g_archive_collection_value_label;
static lv_obj_t *g_settings_focus_widgets[SETTINGS_FOCUS_COUNT];
static lv_obj_t *g_network_status_label;
static lv_obj_t *g_network_value_labels[4];
static lv_obj_t *g_network_dhcp_button;
static lv_obj_t *g_network_dhcp_label;
static lv_obj_t *g_network_apply_button;
static lv_obj_t *g_storage_status_labels[PAX_STORAGE_COUNT];
static lv_obj_t *g_storage_buttons[PAX_STORAGE_COUNT];
static lv_obj_t *g_storage_button_labels[PAX_STORAGE_COUNT];
static lv_obj_t *g_settings_message_label;
static lv_obj_t *g_archive_auth_status_label;
static lv_obj_t *g_archive_s3_status_label;
static lv_obj_t *g_archive_email_value_label;
static lv_obj_t *g_archive_password_value_label;
static lv_obj_t *g_archive_s3_access_value_label;
static lv_obj_t *g_archive_s3_secret_value_label;
static lv_obj_t *g_archive_s3_eye_buttons[2];
static lv_obj_t *g_archive_s3_eye_slashes[2];
static lv_obj_t *g_archive_login_button_label;
static lv_obj_t *g_archive_login_button;
static lv_obj_t *g_status_clock_icon;
static lv_obj_t *g_status_clock;
static bool g_show_date;
static lv_obj_t *g_status_network_icon;
static lv_obj_t *g_status_battery_icon;
static lv_obj_t *g_status_battery_level;
static lv_obj_t *g_toolbar_buttons[TOOLBAR_BUTTON_COUNT];
static lv_obj_t *g_archive_status_label;
static lv_obj_t *g_archive_progress;
/* Reuse a small fixed number of LVGL rows while the selection moves through
 * the catalog. A store can contain hundreds of apps; allocating one button
 * and label per entry needlessly holds LVGL heap while the launcher runs. */
static lv_obj_t *g_buttons[VISIBLE_ROWS];
static lv_obj_t *g_labels[VISIBLE_ROWS];
static lv_obj_t *g_list_detail_labels[VISIBLE_ROWS];
/* The launcher is the one PAX that is allowed to use the panel's native
 * logical size.  It is unloaded before an emulator starts, so its larger
 * LVGL framebuffer does not consume game memory or video bandwidth. */
static int g_panel_w = DEFAULT_PANEL_W;
static int g_panel_h = DEFAULT_PANEL_H;
static int g_canvas_w = DEFAULT_PANEL_W;
static int g_canvas_h = DEFAULT_PANEL_H;
static float g_render_scale = 1.0f;
static float g_ui_scale = 1.0f;
static uint32_t g_next_handler_ms = 5;
static int g_flush_count;

static void play_ui_sound(uint8_t sound, uint8_t param)
{
  if (g_svc != NULL && g_svc->ui_sound != NULL)
    g_svc->ui_sound(sound, param);
}

#include "async_io.h"
#include "supply_discovery.h"
static bool mutations_busy(void){return g_action_busy || g_transfer_busy || g_archive_download_active;}
#include "timing.h"
static PaxTiming g_pump_time,g_flush_time,g_action_time,g_image_time,g_scan_time;
static uint64_t g_action_pending_us;
static uint64_t timing_now(void){return (uint64_t)g_svc->get_time_us();}
static void mark_ui_action(void){if(!g_action_pending_us)g_action_pending_us=timing_now();}
static void log_timing(const char *name,const PaxTiming *value)
{
  g_svc->log_printf("launcher_ui timing %s: samples=%u avg_us=%u max_us=%u over_16ms=%u\n",
      name,value->samples,value->samples ? (uint32_t)(value->total/value->samples) : 0,
      value->maximum,value->over_16ms);
}

static int ui_px(int value)
{
  int scaled = (int)(value * g_ui_scale + 0.5f);
  return scaled > 0 ? scaled : 1;
}

static int abs_int(int value)
{
  return value < 0 ? -value : value;
}

static const lv_font_t *ui_font(void)
{
  /* The original 14 px design was presented through a 2x renderer. Keep the
   * same physical text size when the launcher uses its native panel canvas. */
  if (g_ui_scale >= 2.0f) return &lv_font_montserrat_28;
  if (g_ui_scale >= 1.5f) return &lv_font_montserrat_20;
  return &lv_font_montserrat_14;
}

static const char *source_label(int source)
{
  if (source == SOURCE_NETWORK) return "LAN";
  if (source == SOURCE_STORE) return "STORE";
  if (source == SOURCE_ARCHIVE) return "ARCHIVE";
  return "SD/USB";
}

static const char *source_url(int source)
{
  if (source == SOURCE_NETWORK) return g_network_url;
  if (source == SOURCE_STORE) return "https://nonasuomy.github.io/esphomebrew/";
  return NULL;
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
  if (destination == NULL || capacity == 0) return;
  size_t i = 0;
  if (source != NULL) {
    while (i + 1 < capacity && source[i] != '\0') {
      destination[i] = source[i];
      i++;
    }
  }
  destination[i] = '\0';
}

/* Keep credentials and session tokens from lingering in stack buffers. A
 * volatile byte loop prevents the compiler from removing the wipe as dead
 * stores. */
static void secure_clear(void *memory, size_t length)
{
  volatile unsigned char *bytes = (volatile unsigned char *)memory;
  while (length-- > 0) *bytes++ = 0;
}

static void append_text(char *destination, size_t capacity, const char *source)
{
  if (destination == NULL || capacity == 0 || source == NULL) return;
  size_t used = 0;
  while (used + 1 < capacity && destination[used] != '\0') used++;
  size_t i = 0;
  while (used + 1 < capacity && source[i] != '\0') destination[used++] = source[i++];
  destination[used] = '\0';
}

static void append_uint(char *destination, size_t capacity, uint32_t value)
{
  char digits[10];
  size_t count = 0;
  do {
    digits[count++] = (char)('0' + (value % 10U));
    value /= 10U;
  } while (value != 0 && count < sizeof(digits));
  while (count > 0) {
    char digit[2] = {digits[--count], '\0'};
    append_text(destination, capacity, digit);
  }
}

static bool has_prefix(const char *text, const char *prefix)
{
  size_t i = 0;
  if (text == NULL || prefix == NULL) return false;
  while (prefix[i] != '\0') {
    if (text[i] != prefix[i]) return false;
    i++;
  }
  return true;
}

static bool has_prefix_ascii_case_insensitive(const char *text, const char *prefix)
{
  size_t i = 0;
  if (text == NULL || prefix == NULL) return false;
  while (prefix[i] != '\0') {
    char a = text[i];
    char b = prefix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
    if (a != b) return false;
    i++;
  }
  return true;
}

static int find_bytes(const char *buffer, int length, const char *needle)
{
  size_t needle_length = 0;
  while (needle != NULL && needle[needle_length] != '\0') needle_length++;
  if (needle_length == 0 || buffer == NULL || length < (int)needle_length) return -1;
  for (int i = 0; i <= length - (int)needle_length; i++) {
    size_t j = 0;
    while (j < needle_length && buffer[i + (int)j] == needle[j]) j++;
    if (j == needle_length) return i;
  }
  return -1;
}

static const char *find_char(const char *text, char wanted)
{
  if (text == NULL) return NULL;
  for (size_t i = 0; text[i] != '\0'; i++) if (text[i] == wanted) return text + i;
  return NULL;
}

static const char *find_substring(const char *text, const char *wanted)
{
  if (text == NULL || wanted == NULL || wanted[0] == '\0') return NULL;
  const size_t wanted_length = strlen(wanted);
  for (size_t i = 0; text[i] != '\0'; i++) {
    size_t j = 0;
    while (j < wanted_length && text[i + j] != '\0' && text[i + j] == wanted[j]) j++;
    if (j == wanted_length) return text + i;
  }
  return NULL;
}

static const char *find_last_char(const char *text, char wanted)
{
  const char *found = NULL;
  if (text == NULL) return NULL;
  for (size_t i = 0; text[i] != '\0'; i++) if (text[i] == wanted) found = text + i;
  return found;
}

static const char *find_first_of(const char *text, const char *characters)
{
  if (text == NULL || characters == NULL) return NULL;
  for (size_t i = 0; text[i] != '\0'; i++)
    for (size_t j = 0; characters[j] != '\0'; j++)
      if (text[i] == characters[j]) return text + i;
  return NULL;
}

static bool contains_substring(const char *text, const char *wanted)
{
  return find_substring(text, wanted) != NULL;
}

static bool append_exact(char *destination, size_t capacity, const char *text, size_t length)
{
  if (destination == NULL || text == NULL || capacity == 0) return false;
  const size_t used = strlen(destination);
  if (used + length >= capacity) return false;
  memcpy(destination + used, text, length);
  destination[used + length] = '\0';
  return true;
}

static bool append_text_checked(char *destination, size_t capacity, const char *text)
{
  return text != NULL && append_exact(destination, capacity, text, strlen(text));
}

static int find_ascii_case_insensitive(const char *buffer, int length, const char *needle)
{
  size_t needle_length = 0;
  while (needle != NULL && needle[needle_length] != '\0') needle_length++;
  if (needle_length == 0 || buffer == NULL || length < (int)needle_length) return -1;
  for (int i = 0; i <= length - (int)needle_length; i++) {
    size_t j = 0;
    while (j < needle_length) {
      char a = buffer[i + (int)j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
      if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
      if (a != b) break;
      j++;
    }
    if (j == needle_length) return i;
  }
  return -1;
}

static bool range_ends_with(const char *text, int length, const char *suffix)
{
  size_t suffix_length = 0;
  while (suffix != NULL && suffix[suffix_length] != '\0') suffix_length++;
  if (text == NULL || suffix_length > (size_t)length) return false;
  for (size_t i = 0; i < suffix_length; i++)
    if (text[length - (int)suffix_length + (int)i] != suffix[i]) return false;
  return true;
}

static bool parse_ipv4(const char *text, uint32_t *ip)
{
  if (text == NULL || ip == NULL) return false;
  uint32_t value = 0;
  int octet = 0;
  int digits = 0;
  int position = 0;
  for (position = 0; ; position++) {
    char c = text[position];
    if (c >= '0' && c <= '9') {
      octet = octet * 10 + (c - '0');
      if (octet > 255) return false;
      digits++;
    } else if (c == '.' || c == '\0') {
      if (digits == 0) return false;
      value = (value << 8) | (uint32_t)octet;
      octet = 0;
      digits = 0;
      if (c == '\0') break;
    } else {
      return false;
    }
  }
  if (position == 0) return false;
  /* Four octets are required; the top byte is non-zero for the common LAN
   * address, but zero is valid, so count separators rather than infer it. */
  int dots = 0;
  for (int i = 0; text[i] != '\0'; i++) if (text[i] == '.') dots++;
  if (dots != 3) return false;
  *ip = value;
  return true;
}

static bool archive_host_name_allowed(const char *host)
{
  if (host == NULL) return false;
  const size_t length = strlen(host);
  const size_t suffix = length == 11 ? 0 : length > 12 ? length - 12 : length;
  if (length != 11 && (length <= 12 || host[suffix] != '.')) return false;
  const char *domain = host + (length == 11 ? 0 : suffix + 1);
  for (size_t i = 0; i < 11; i++) {
    char c = domain[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    if (c != "archive.org"[i]) return false;
  }
  return domain[11] == '\0';
}

static bool is_archive_plain_http_url(const char *url)
{
  if (!has_prefix(url, "http://")) return false;
  char host[96];
  size_t used = 0;
  const char *cursor = url + 7;
  while (*cursor != '\0' && *cursor != '/' && *cursor != ':' &&
         *cursor != '?' && *cursor != '#') {
    if (used + 1 >= sizeof(host)) return false;
    char c = *cursor++;
    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    host[used++] = c;
  }
  host[used] = '\0';
  return archive_host_name_allowed(host);
}

static int build_http_request(char *request, int capacity, const char *host, const char *path,
                              const char *method, const char *cookie,
                              const char *authorization, const char *post_body)
{
  if (request == NULL || capacity <= 0 || host == NULL || path == NULL || method == NULL) return -1;
  char content_length[16] = "";
  const size_t body_length = post_body != NULL ? strlen(post_body) : 0;
  append_uint(content_length, sizeof(content_length), (uint32_t)body_length);
  const char *parts[] = {
    method, " ", path,
    " HTTP/1.1\r\nHost: ", host,
    "\r\nAccept: application/json, text/html\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
    authorization != NULL && authorization[0] != '\0' ? "Authorization: " : "",
    authorization != NULL && authorization[0] != '\0' ? authorization : "",
    authorization != NULL && authorization[0] != '\0' ? "\r\n" : "",
    cookie != NULL && cookie[0] != '\0' ? "Cookie: " : "",
    cookie != NULL && cookie[0] != '\0' ? cookie : "",
    cookie != NULL && cookie[0] != '\0' ? "\r\n" : ""
  };
  int written = 0;
  for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); p++) {
    for (int i = 0; parts[p][i] != '\0'; i++) {
      if (written + 1 >= capacity) return -1;
      request[written++] = parts[p][i];
    }
  }
  if (post_body != NULL) {
    const char *post_parts[] = {"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ",
                                content_length, "\r\n"};
    for (size_t p = 0; p < sizeof(post_parts) / sizeof(post_parts[0]); p++) {
      for (int i = 0; post_parts[p][i] != '\0'; i++) {
        if (written + 1 >= capacity) return -1;
        request[written++] = post_parts[p][i];
      }
    }
  }
  if (written + 2 >= capacity) return -1;
  request[written++] = '\r';
  request[written++] = '\n';
  if (post_body != NULL) {
    for (size_t i = 0; i < body_length; i++) {
      if (written + 1 >= capacity) return -1;
      request[written++] = post_body[i];
    }
  }
  request[written] = '\0';
  return written;
}

static void close_http_request_socket(int handle, bool tls)
{
  if (handle < 0 || g_svc == NULL) return;
  if (tls && g_svc->net_tls_close != NULL) g_svc->net_tls_close(handle);
  else if (!tls && g_svc->net_udp_close != NULL) g_svc->net_udp_close(handle);
}

static int fail_http_request(int handle, bool tls, char *request, size_t request_capacity,
                             char *cookie, size_t cookie_capacity)
{
  close_http_request_socket(handle, tls);
  secure_clear(request, request_capacity);
  secure_clear(cookie, cookie_capacity);
  return -1;
}

/* Fetch a small HTML catalog through the loader's networking ABI.  The
 * catalog pages are intentionally requested without compression so the PAX
 * only needs to find ordinary HTML hrefs. */
static int g_http_status_code;

static int http_request_redirects(const char *base_url, char *body, int capacity, int redirects,
                                  const char *method, const char *post_body)
{
  g_http_status_code = 0;
  if (__atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE)) return -1;
  if (g_svc == NULL || base_url == NULL || body == NULL || capacity < 256 ||
      g_svc->net_poll == NULL) return -1;
  bool tls = has_prefix(base_url, "https://");
  const char *host_start = base_url + (tls ? 8 : 7);
  const char *path_start = NULL;
  for (const char *cursor = host_start; *cursor != '\0'; cursor++) {
    if (*cursor == '/') { path_start = cursor; break; }
  }
  if (path_start == NULL) path_start = "/";
  char host[96];
  size_t host_length = (size_t)(path_start - host_start);
  if (host_length >= sizeof(host)) return -1;
  for (size_t i = 0; i < host_length; i++) host[i] = host_start[i];
  host[host_length] = '\0';
  uint16_t port = (uint16_t)(tls ? 443 : 80);
  /* The two configured URLs have no explicit port in the TLS case and the
   * LAN URL uses :8000.  Keep parsing deliberately small and allocation-free. */
  char host_only[96];
  copy_text(host_only, sizeof(host_only), host);
  for (size_t i = 0; host_only[i] != '\0'; i++) {
    if (host_only[i] == ':') {
      int parsed = 0;
      for (size_t j = i + 1; host_only[j] >= '0' && host_only[j] <= '9'; j++)
        parsed = parsed * 10 + (host_only[j] - '0');
      if (parsed > 0 && parsed < 65536) port = (uint16_t)parsed;
      host_only[i] = '\0';
      break;
    }
  }
  char *cookie_buffer = g_http_cookie;
  char *request = g_http_request;
  char authorization[sizeof(g_archive_s3_access) + sizeof(g_archive_s3_secret) + 8] = "";
  cookie_buffer[0] = '\0';
  const char *authorization_header = NULL;
  const bool is_ia_s3_host = strcmp(host_only, "s3.us.archive.org") == 0;
  if (tls && is_ia_s3_host && g_archive_s3_access[0] != '\0' &&
      g_archive_s3_secret[0] != '\0') {
    copy_text(authorization, sizeof(authorization), "LOW ");
    if (append_text_checked(authorization, sizeof(authorization), g_archive_s3_access) &&
        append_text_checked(authorization, sizeof(authorization), ":") &&
        append_text_checked(authorization, sizeof(authorization), g_archive_s3_secret))
      authorization_header = authorization;
    else
      secure_clear(authorization, sizeof(authorization));
  }
  if (tls && !is_ia_s3_host && archive_host_name_allowed(host_only) &&
      g_svc->archive_auth_get != NULL)
    (void)g_svc->archive_auth_get(cookie_buffer, sizeof(g_http_cookie));
  int handle = -1;
  int request_length = build_http_request(request, sizeof(g_http_request), host_only, path_start, method,
                                          cookie_buffer, authorization_header, post_body);
  secure_clear(authorization, sizeof(authorization));
  if (request_length < 0)
    return fail_http_request(handle, tls, request, sizeof(g_http_request),
                             cookie_buffer, sizeof(g_http_cookie));
  if (tls) {
    if (g_svc->net_tls_connect == NULL || g_svc->net_tls_status == NULL ||
        g_svc->net_tls_send == NULL || g_svc->net_tls_recv == NULL ||
        g_svc->net_tls_close == NULL)
      return fail_http_request(handle, tls, request, sizeof(g_http_request),
                               cookie_buffer, sizeof(g_http_cookie));
    handle = g_svc->net_tls_connect(host_only, port);
  } else {
    if (g_svc->net_tcp_connect == NULL || g_svc->net_tcp_send == NULL ||
        g_svc->net_tcp_recv == NULL)
      return fail_http_request(handle, tls, request, sizeof(g_http_request),
                               cookie_buffer, sizeof(g_http_cookie));
    uint32_t ip = 0;
    if (g_svc->net_resolve != NULL) {
      if (!g_svc->net_resolve(host_only, &ip))
        return fail_http_request(handle, tls, request, sizeof(g_http_request),
                                 cookie_buffer, sizeof(g_http_cookie));
    } else if (!parse_ipv4(host_only, &ip)) {
      return fail_http_request(handle, tls, request, sizeof(g_http_request),
                               cookie_buffer, sizeof(g_http_cookie));
    }
    handle = g_svc->net_tcp_connect(ip, port);
  }
  if (handle < 0)
    return fail_http_request(handle, tls, request, sizeof(g_http_request),
                             cookie_buffer, sizeof(g_http_cookie));
  int wait_ms = 0;
  while (wait_ms < 16000) {
    if (__atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE))
      return fail_http_request(handle,tls,request,sizeof(g_http_request),cookie_buffer,sizeof(g_http_cookie));
    int ready = tls ? g_svc->net_tls_status(handle) : g_svc->net_poll(handle);
    if (ready < 0 || (!tls && (ready & 4))) {
      return fail_http_request(handle, tls, request, sizeof(g_http_request),
                               cookie_buffer, sizeof(g_http_cookie));
    }
    if ((tls && ready == 1) || (!tls && (ready & 2))) break;
    g_svc->delay_ms(50);
    wait_ms += 50;
  }
  if (wait_ms >= 16000) {
    return fail_http_request(handle, tls, request, sizeof(g_http_request),
                             cookie_buffer, sizeof(g_http_cookie));
  }
  int sent = 0;
  while (sent < request_length && wait_ms < 18000) {
    if (__atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE))
      return fail_http_request(handle,tls,request,sizeof(g_http_request),cookie_buffer,sizeof(g_http_cookie));
    int n = tls ? g_svc->net_tls_send(handle, request + sent, request_length - sent)
                : g_svc->net_tcp_send(handle, request + sent, request_length - sent);
    if (n < 0) {
      return fail_http_request(handle, tls, request, sizeof(g_http_request),
                               cookie_buffer, sizeof(g_http_cookie));
    }
    if (n > 0) sent += n;
    else { g_svc->delay_ms(20); wait_ms += 20; }
  }
  if (sent != request_length) {
    return fail_http_request(handle, tls, request, sizeof(g_http_request),
                             cookie_buffer, sizeof(g_http_cookie));
  }
  secure_clear(request, sizeof(g_http_request));
  secure_clear(cookie_buffer, sizeof(g_http_cookie));
  int used = 0;
  int idle_ms = 0;
  while (used + 1 < capacity && idle_ms < 12000) {
    if (__atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE))
      return fail_http_request(handle,tls,request,sizeof(g_http_request),cookie_buffer,sizeof(g_http_cookie));
    char *destination = body + used;
    int room = capacity - used - 1;
    int n = tls ? g_svc->net_tls_recv(handle, destination, room)
                : g_svc->net_tcp_recv(handle, destination, room);
    if (n > 0) { used += n; idle_ms = 0; continue; }
    if ((tls && n < 0) || (!tls && n == 0)) break;
    /* TLS returns 0 while the decrypted record is still arriving; plain TCP
     * returns -2 for the same non-blocking condition. */
    if ((tls && n == 0) || (!tls && n == -2)) {
      g_svc->delay_ms(20);
      idle_ms += 20;
    } else {
      break;
    }
  }
  if (tls) g_svc->net_tls_close(handle);
  else if (g_svc->net_udp_close != NULL) g_svc->net_udp_close(handle);
  if (used + 1 >= capacity) return -1;  /* Do not parse a silently truncated page. */
  body[used] = '\0';
  int header_end = find_bytes(body, used, "\r\n\r\n");
  if (header_end < 0) return -1;
  const int status_end = find_bytes(body, header_end, "\r\n");
  if (status_end < 0) return -1;
  g_http_status_code = 0;
  for (int i = status_end - 3; i < status_end; i++) {
    if (i >= 0 && body[i] >= '0' && body[i] <= '9')
      g_http_status_code = g_http_status_code * 10 + (body[i] - '0');
  }
  if (find_bytes(body, status_end, " 200 ") < 0) {
    const bool is_redirect = find_bytes(body, status_end, " 301 ") >= 0 ||
                             find_bytes(body, status_end, " 302 ") >= 0 ||
                             find_bytes(body, status_end, " 303 ") >= 0 ||
                             find_bytes(body, status_end, " 307 ") >= 0 ||
                             find_bytes(body, status_end, " 308 ") >= 0;
    if (!is_redirect || redirects >= 3 || (post_body != NULL && post_body[0] != '\0')) return -1;
    int location = find_ascii_case_insensitive(body, header_end, "location:");
    if (location < 0) return -1;
    location += 9;
    while (location < header_end && (body[location] == ' ' || body[location] == '\t')) location++;
    int end = location;
    while (end < header_end && body[end] != '\r' && body[end] != '\n') end++;
    if (end <= location) return -1;
    char target[PATH_BYTES];
    size_t target_length = (size_t)(end - location);
    if (target_length >= sizeof(target)) return -1;
    memcpy(target, body + location, target_length);
    target[target_length] = '\0';
    if (has_prefix(target, "//")) {
      char absolute[PATH_BYTES] = "";
      if (!append_text_checked(absolute, sizeof(absolute), has_prefix(base_url, "https://") ? "https:" : "http:") ||
          !append_text_checked(absolute, sizeof(absolute), target)) return -1;
      copy_text(target, sizeof(target), absolute);
    } else if (target[0] == '/') {
      const char *scheme_end = find_substring(base_url, "://");
      if (scheme_end == NULL) return -1;
      const char *host_end = find_char(scheme_end + 3, '/');
      const size_t origin_length = host_end == NULL ? strlen(base_url) : (size_t)(host_end - base_url);
      char absolute[PATH_BYTES] = "";
      if (!append_exact(absolute, sizeof(absolute), base_url, origin_length) ||
          !append_text_checked(absolute, sizeof(absolute), target)) return -1;
      copy_text(target, sizeof(target), absolute);
    } else if (!has_prefix(target, "http://") && !has_prefix(target, "https://")) {
      char absolute[PATH_BYTES] = "";
      const char *last_slash = find_last_char(base_url, '/');
      const size_t base_length = last_slash == NULL ? 0 : (size_t)(last_slash + 1 - base_url);
      if (!append_exact(absolute, sizeof(absolute), base_url, base_length) ||
          !append_text_checked(absolute, sizeof(absolute), target)) return -1;
      copy_text(target, sizeof(target), absolute);
    }
    return http_request_redirects(target, body, capacity, redirects + 1, method, post_body);
  }
  header_end += 4;
  int body_length = used - header_end;
  for (int i = 0; i < body_length; i++) body[i] = body[header_end + i];
  body[body_length] = '\0';
  return body_length;
}

typedef struct {const char *url,*method,*form;char *body;int capacity;} HttpJob;
static int http_job(void *argument)
{
  HttpJob *job=argument;
  return http_request_redirects(job->url,job->body,job->capacity,0,job->method,job->form);
}
static int http_fetch(const char *url,char *body,int capacity)
{
  HttpJob job={url,"GET",NULL,body,capacity};return io_run(http_job,&job);
}
static int http_post_form(const char *url,const char *form_body,char *body,int capacity)
{
  HttpJob job={url,"POST",form_body,body,capacity};return io_run(http_job,&job);
}

static void set_fa_glyph(lv_obj_t *label, uint32_t codepoint)
{
  if (label == NULL) return;
  char utf8[5];
  if (codepoint <= 0x7fU) {
    utf8[0] = (char)codepoint;
    utf8[1] = '\0';
  } else if (codepoint <= 0x7ffU) {
    utf8[0] = (char)(0xc0U | (codepoint >> 6));
    utf8[1] = (char)(0x80U | (codepoint & 0x3fU));
    utf8[2] = '\0';
  } else {
    utf8[0] = (char)(0xe0U | (codepoint >> 12));
    utf8[1] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
    utf8[2] = (char)(0x80U | (codepoint & 0x3fU));
    utf8[3] = '\0';
  }
  lv_label_set_text(label, utf8);
}

static lv_obj_t *create_fa_label(lv_obj_t *parent, uint32_t codepoint, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  set_fa_glyph(label, codepoint);
  lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
  lv_obj_set_style_text_font(label, &launcher_fa_icons, LV_PART_MAIN);
  return label;
}

static int parse_pax_links(const char *html, int length,
                            char names[MAX_APPS][NAME_BYTES],
                            char paths[MAX_APPS][PATH_BYTES],
                            const char *base_url)
{
  int count = 0;
  int cursor = 0;
  while (cursor < length && count < MAX_APPS) {
    int start = -1;
    for (int i = cursor; i + 9 < length; i++) {
      if (html[i] == 'h' && html[i + 1] == 'r' && html[i + 2] == 'e' &&
          html[i + 3] == 'f' && html[i + 4] == '=' &&
          (html[i + 5] == '"' || html[i + 5] == '\'')) { start = i + 6; break; }
    }
    if (start < 0) break;
    char quote = html[start - 1];
    int end = start;
    while (end < length && html[end] != quote) end++;
    if (end >= length) break;
    int href_length = end - start;
    if (href_length > 0 && href_length < PATH_BYTES &&
        range_ends_with(html + start, href_length, ".pax")) {
      bool duplicate = false;
      for (int i = 0; i < count; i++) {
        size_t j = 0;
        while (names[i][j] != '\0' && html[start + (int)j] != '\0' &&
               j < NAME_BYTES - 1 && names[i][j] == html[start + (int)j]) j++;
        if (j == (size_t)href_length || (names[i][j] == '\0' && j == (size_t)href_length)) {
          duplicate = true; break;
        }
      }
      if (!duplicate) {
        int name_start = start;
        for (int i = start; i < end; i++) if (html[i] == '/') name_start = i + 1;
        int name_length = end - name_start;
        if (name_length >= NAME_BYTES) name_length = NAME_BYTES - 1;
        for (int i = 0; i < name_length; i++) names[count][i] = html[name_start + i];
        names[count][name_length] = '\0';
        if (base_url != NULL && !has_prefix(html + start, "http://") &&
            !has_prefix(html + start, "https://")) {
          copy_text(paths[count], PATH_BYTES, base_url);
          size_t used = 0;
          while (used + 1 < PATH_BYTES && paths[count][used] != '\0') used++;
          for (int i = 0; i < href_length && used + 1 < PATH_BYTES; i++)
            paths[count][used++] = html[start + i];
          paths[count][used] = '\0';
        } else {
          int copied = href_length < PATH_BYTES - 1 ? href_length : PATH_BYTES - 1;
          for (int i = 0; i < copied; i++) paths[count][i] = html[start + i];
          paths[count][copied] = '\0';
        }
        int tag_start=start-6;while(tag_start>0 && html[tag_start]!='<')tag_start--;
        int tag_end=end;while(tag_end<length && html[tag_end]!='>')tag_end++;
        if(tag_end<length) {
          g_list_stats_valid[count]=pax_network_file_info(html+tag_start,(size_t)(tag_end-tag_start+1),&g_list_stats[count]);
          (void)pax_network_title(html+tag_start,(size_t)(tag_end-tag_start+1),g_titles[count],NAME_BYTES);
        }
        count++;
      }
    }
    cursor = end + 1;
  }
  return count;
}

static const char *display_name_for(int index);

static bool json_string_field(const char *json, int length, const char *key,
                              char *out, size_t capacity)
{
  if (json == NULL || key == NULL || out == NULL || capacity == 0) return false;
  out[0] = '\0';
  char quoted_key[48];
  size_t key_length = 0;
  while (key[key_length] != '\0' && key_length + 3 < sizeof(quoted_key)) {
    quoted_key[key_length + 1] = key[key_length];
    key_length++;
  }
  if (key[key_length] != '\0') return false;
  quoted_key[0] = '"';
  quoted_key[key_length + 1] = '"';
  quoted_key[key_length + 2] = '\0';
  const int found = find_bytes(json, length, quoted_key);
  if (found < 0) return false;
  int cursor = found + (int)key_length + 2;
  while (cursor < length && (json[cursor] == ' ' || json[cursor] == '\t' ||
                             json[cursor] == '\r' || json[cursor] == '\n')) cursor++;
  if (cursor >= length || json[cursor++] != ':') return false;
  while (cursor < length && (json[cursor] == ' ' || json[cursor] == '\t' ||
                             json[cursor] == '\r' || json[cursor] == '\n')) cursor++;
  if (cursor >= length || json[cursor++] != '"') return false;
  size_t used = 0;
  while (cursor < length) {
    char c = json[cursor++];
    if (c == '"') {
      out[used] = '\0';
      return true;
    }
    if (c == '\\') {
      if (cursor >= length) break;
      c = json[cursor++];
      if (c == 'n') c = ' ';
      else if (c == 'r' || c == 't') c = ' ';
      else if (c == 'u') {
        if (cursor + 4 > length) break;
        cursor += 4;
        c = '?';
      }
    }
    if (used + 1 < capacity) out[used++] = c;
  }
  out[used] = '\0';
  return false;
}

static bool json_string_span(const char *json, int length, const char *key,
                             const char **value, int *value_length)
{
  if (json == NULL || key == NULL || value == NULL || value_length == NULL) return false;
  char quoted_key[48];
  size_t key_length = 0;
  while (key[key_length] != '\0' && key_length + 3 < sizeof(quoted_key)) {
    quoted_key[key_length + 1] = key[key_length];
    key_length++;
  }
  if (key[key_length] != '\0') return false;
  quoted_key[0] = '"';
  quoted_key[key_length + 1] = '"';
  quoted_key[key_length + 2] = '\0';
  const int found = find_bytes(json, length, quoted_key);
  if (found < 0) return false;
  int cursor = found + (int)key_length + 2;
  while (cursor < length && json[cursor] != ':') cursor++;
  if (cursor >= length) return false;
  cursor++;
  while (cursor < length && (json[cursor] == ' ' || json[cursor] == '\t' ||
                             json[cursor] == '\r' || json[cursor] == '\n')) cursor++;
  if (cursor >= length || json[cursor++] != '"') return false;
  const int start = cursor;
  bool escaped = false;
  while (cursor < length) {
    const char c = json[cursor];
    if (!escaped && c == '"') {
      *value = json + start;
      *value_length = cursor - start;
      return true;
    }
    if (!escaped && c == '\\') escaped = true;
    else escaped = false;
    cursor++;
  }
  return false;
}

static bool decode_base64_png_to_file(const char *json, int length, const char *path)
{
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_write == NULL ||
      g_svc->file_close == NULL) return false;
  const char *encoded = NULL;
  int encoded_length = 0;
  if (!json_string_span(json, length, "base64", &encoded, &encoded_length) ||
      encoded_length < 16) return false;
  void *file = g_svc->file_open(path, "wb");
  if (file == NULL) return false;
  uint8_t output[384];
  size_t output_used = 0;
  uint32_t accumulator = 0;
  int bits = 0;
  bool valid = true;
  for (int i = 0; i < encoded_length; i++) {
    const unsigned char c = (unsigned char)encoded[i];
    int value = -1;
    if (c >= 'A' && c <= 'Z') value = c - 'A';
    else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
    else if (c >= '0' && c <= '9') value = c - '0' + 52;
    else if (c == '+') value = 62;
    else if (c == '/') value = 63;
    else if (c == '=') break;
    else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    else { valid = false; break; }
    accumulator = (accumulator << 6) | (uint32_t)value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output[output_used++] = (uint8_t)(accumulator >> bits);
      if (output_used == sizeof(output)) {
        if (g_svc->file_write(output, 1, output_used, file) != output_used) {
          valid = false;
          break;
        }
        output_used = 0;
      }
    }
  }
  if (valid && output_used > 0 &&
      g_svc->file_write(output, 1, output_used, file) != output_used) valid = false;
  if (g_svc->file_close(file) != 0) valid = false;
  return valid;
}

static bool json_uint_field(const char *json, int length, const char *key, uint32_t *out)
{
  if (json == NULL || key == NULL || out == NULL) return false;
  char quoted_key[48];
  size_t key_length = 0;
  while (key[key_length] != '\0' && key_length + 3 < sizeof(quoted_key)) {
    quoted_key[key_length + 1] = key[key_length];
    key_length++;
  }
  if (key[key_length] != '\0') return false;
  quoted_key[0] = '"';
  quoted_key[key_length + 1] = '"';
  quoted_key[key_length + 2] = '\0';
  const int found = find_bytes(json, length, quoted_key);
  if (found < 0) return false;
  int cursor = found + (int)key_length + 2;
  while (cursor < length && json[cursor] != ':') cursor++;
  if (cursor >= length) return false;
  cursor++;
  while (cursor < length && (json[cursor] == ' ' || json[cursor] == '\t' ||
                             json[cursor] == '\r' || json[cursor] == '\n')) cursor++;
  if (cursor >= length || json[cursor] < '0' || json[cursor] > '9') return false;
  uint32_t value = 0;
  while (cursor < length && json[cursor] >= '0' && json[cursor] <= '9') {
    const uint32_t digit = (uint32_t)(json[cursor++] - '0');
    if (value > (UINT32_MAX - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  *out = value;
  return true;
}

static bool ends_with(const char *text, const char *suffix);
static int selected_record_index(void);

static bool archive_collection_id_valid(const char *identifier)
{
  if (identifier == NULL || identifier[0] == '\0') return false;
  size_t length = 0;
  for (; identifier[length] != '\0'; length++) {
    const char c = identifier[length];
    if (length >= sizeof(g_archive_collection) - 1 ||
        !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
      return false;
  }
  return length > 0;
}

static bool archive_collection_equal(const char *left, const char *right)
{
  size_t i = 0;
  while (left[i] != '\0' && right[i] != '\0' && left[i] == right[i]) i++;
  return left[i] == '\0' && right[i] == '\0';
}

static void save_archive_collection(void)
{
  if (!archive_collection_id_valid(g_archive_collection) || g_svc == NULL ||
      g_svc->file_open == NULL || g_svc->file_write == NULL || g_svc->file_close == NULL)
    return;
  void *file = g_svc->file_open(ARCHIVE_COLLECTION_FILE, "w");
  if (file == NULL) {
    g_svc->log_printf("launcher_ui: could not save Archive.org collection (%s)\n",
                      ARCHIVE_COLLECTION_FILE);
    return;
  }
  const size_t length = strlen(g_archive_collection);
  (void)g_svc->file_write(g_archive_collection, 1, length, file);
  (void)g_svc->file_write("\n", 1, 1, file);
  (void)g_svc->file_close(file);
}

static void load_archive_collection(void)
{
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_read == NULL ||
      g_svc->file_close == NULL)
    return;
  void *file = g_svc->file_open(ARCHIVE_COLLECTION_FILE, "r");
  if (file == NULL) return;
  char candidate[sizeof(g_archive_collection)];
  size_t used = 0;
  while (used + 1 < sizeof(candidate) && g_svc->file_read(&candidate[used], 1, 1, file) == 1) {
    if (candidate[used] == '\n' || candidate[used] == '\r') break;
    used++;
  }
  candidate[used] = '\0';
  (void)g_svc->file_close(file);
  if (archive_collection_id_valid(candidate))
    copy_text(g_archive_collection, sizeof(g_archive_collection), candidate);
}

static bool archive_credential_field_valid(const char *value, size_t capacity)
{
  if (value == NULL || value[0] == '\0') return false;
  for (size_t i = 0; i < capacity; i++) {
    const unsigned char c = (unsigned char)value[i];
    if (c == '\0') return true;
    /* The simple on-card format uses one field per line. Reject control
     * characters so a credential cannot escape its field in that file. */
    if (c < 0x20 || c == 0x7f) return false;
  }
  return false;
}

static bool save_archive_credentials(void)
{
  if (!archive_credential_field_valid(g_archive_email, sizeof(g_archive_email)) ||
      !archive_credential_field_valid(g_archive_password, sizeof(g_archive_password)) ||
      g_svc == NULL || g_svc->file_open == NULL || g_svc->file_write == NULL ||
      g_svc->file_close == NULL)
    return false;
  void *file = g_svc->file_open(ARCHIVE_LOGIN_FILE, "w");
  if (file == NULL) return false;
  const size_t email_length = strlen(g_archive_email);
  const size_t password_length = strlen(g_archive_password);
  const bool wrote_email = g_svc->file_write(g_archive_email, 1, email_length, file) == email_length;
  const bool wrote_separator = g_svc->file_write("\n", 1, 1, file) == 1;
  const bool wrote_password = g_svc->file_write(g_archive_password, 1, password_length, file) == password_length;
  const bool wrote_end = g_svc->file_write("\n", 1, 1, file) == 1;
  const int close_result = g_svc->file_close(file);
  return wrote_email && wrote_separator && wrote_password && wrote_end && close_result == 0;
}

static void load_archive_credentials(void)
{
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_read == NULL ||
      g_svc->file_close == NULL)
    return;
  void *file = g_svc->file_open(ARCHIVE_LOGIN_FILE, "r");
  if (file == NULL) return;  /* Keep the in-memory values if SD is ejected. */
  char serialized[sizeof(g_archive_email) + sizeof(g_archive_password) + 2] = {};
  size_t used = 0;
  while (used + 1 < sizeof(serialized) &&
         g_svc->file_read(&serialized[used], 1, 1, file) == 1)
    used++;
  (void)g_svc->file_close(file);
  size_t separator = 0;
  while (separator < used && serialized[separator] != '\n' && serialized[separator] != '\r')
    separator++;
  if (separator == 0 || separator >= used) {
    secure_clear(serialized, sizeof(serialized));
    return;
  }
  size_t password_start = separator + 1;
  while (password_start < used &&
         (serialized[password_start] == '\n' || serialized[password_start] == '\r'))
    password_start++;
  size_t password_end = password_start;
  while (password_end < used && serialized[password_end] != '\n' &&
         serialized[password_end] != '\r')
    password_end++;
  const size_t password_length = password_end - password_start;
  if (separator < sizeof(g_archive_email) && password_length > 0 &&
      password_length < sizeof(g_archive_password)) {
    serialized[separator] = '\0';
    serialized[password_end] = '\0';
    if (archive_credential_field_valid(serialized, sizeof(g_archive_email)) &&
        archive_credential_field_valid(serialized + password_start,
                                       sizeof(g_archive_password))) {
      copy_text(g_archive_email, sizeof(g_archive_email), serialized);
      copy_text(g_archive_password, sizeof(g_archive_password), serialized + password_start);
    }
  }
  secure_clear(serialized, sizeof(serialized));
}

static void remove_archive_credentials(void)
{
  if (g_svc != NULL && g_svc->file_remove != NULL)
    (void)g_svc->file_remove(ARCHIVE_LOGIN_FILE);
  secure_clear(g_archive_email, sizeof(g_archive_email));
  secure_clear(g_archive_password, sizeof(g_archive_password));
}

/* IA-S3 credentials are distinct from the browser session cookies above.
 * Keep them in their own masked settings fields and only send them to the
 * exact HTTPS S3 auth-check host. This card file is plaintext (exFAT has no
 * useful per-file permissions), so the settings UI calls that out. */
static bool save_archive_s3_keys(void)
{
  if (g_archive_s3_access[0] == '\0' && g_archive_s3_secret[0] == '\0') {
    const bool removed = g_svc != NULL && g_svc->file_remove != NULL &&
                         g_svc->file_remove(ARCHIVE_S3_KEYS_FILE) == 0;
    copy_text(g_archive_s3_status, sizeof(g_archive_s3_status),
              removed ? "Keys cleared" : "Could not clear saved keys");
    return removed;
  }
  const bool access_valid = g_archive_s3_access[0] == '\0' ||
      archive_credential_field_valid(g_archive_s3_access, sizeof(g_archive_s3_access));
  const bool secret_valid = g_archive_s3_secret[0] == '\0' ||
      archive_credential_field_valid(g_archive_s3_secret, sizeof(g_archive_s3_secret));
  if (!access_valid || !secret_valid) {
    copy_text(g_archive_s3_status, sizeof(g_archive_s3_status), "S3 key contains an invalid character");
    return false;
  }
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_write == NULL ||
      g_svc->file_close == NULL) {
    copy_text(g_archive_s3_status, sizeof(g_archive_s3_status),
              "SD storage unavailable; keys not saved");
    return false;
  }
  void *file = g_svc->file_open(ARCHIVE_S3_KEYS_FILE, "w");
  if (file == NULL) {
    copy_text(g_archive_s3_status, sizeof(g_archive_s3_status), "Could not save S3 keys to SD");
    return false;
  }
  const size_t access_length = strlen(g_archive_s3_access);
  const size_t secret_length = strlen(g_archive_s3_secret);
  const bool wrote_access = g_svc->file_write(g_archive_s3_access, 1, access_length, file) == access_length;
  const bool wrote_separator = g_svc->file_write("\n", 1, 1, file) == 1;
  const bool wrote_secret = g_svc->file_write(g_archive_s3_secret, 1, secret_length, file) == secret_length;
  const bool wrote_end = g_svc->file_write("\n", 1, 1, file) == 1;
  const int close_result = g_svc->file_close(file);
  const bool saved = wrote_access && wrote_separator && wrote_secret && wrote_end && close_result == 0;
  copy_text(g_archive_s3_status, sizeof(g_archive_s3_status),
            saved ? "Saved (plain-text on SD)" : "Could not save S3 keys to SD");
  return saved;
}

static void load_archive_s3_keys(void)
{
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_read == NULL ||
      g_svc->file_close == NULL)
    return;
  void *file = g_svc->file_open(ARCHIVE_S3_KEYS_FILE, "r");
  if (file == NULL) return;
  char serialized[sizeof(g_archive_s3_access) + sizeof(g_archive_s3_secret) + 2] = {};
  size_t used = 0;
  while (used + 1 < sizeof(serialized) &&
         g_svc->file_read(&serialized[used], 1, 1, file) == 1)
    used++;
  (void)g_svc->file_close(file);
  size_t separator = 0;
  while (separator < used && serialized[separator] != '\n' && serialized[separator] != '\r')
    separator++;
  size_t secret_start = separator + 1;
  while (secret_start < used &&
         (serialized[secret_start] == '\n' || serialized[secret_start] == '\r'))
    secret_start++;
  size_t secret_end = secret_start;
  while (secret_end < used && serialized[secret_end] != '\n' && serialized[secret_end] != '\r')
    secret_end++;
  const size_t access_length = separator;
  const size_t secret_length = secret_end - secret_start;
  if (separator < sizeof(g_archive_s3_access) && secret_length < sizeof(g_archive_s3_secret) &&
      (access_length > 0 || secret_length > 0)) {
    serialized[separator] = '\0';
    serialized[secret_end] = '\0';
    const bool access_valid = access_length == 0 ||
        archive_credential_field_valid(serialized, sizeof(g_archive_s3_access));
    const bool secret_valid = secret_length == 0 ||
        archive_credential_field_valid(serialized + secret_start, sizeof(g_archive_s3_secret));
    if (access_valid && secret_valid) {
      copy_text(g_archive_s3_access, sizeof(g_archive_s3_access), serialized);
      copy_text(g_archive_s3_secret, sizeof(g_archive_s3_secret), serialized + secret_start);
      copy_text(g_archive_s3_status, sizeof(g_archive_s3_status), "Saved (plain-text on SD)");
    }
  }
  secure_clear(serialized, sizeof(serialized));
}

static void clear_archive_s3_keys(void)
{
  secure_clear(g_archive_s3_access, sizeof(g_archive_s3_access));
  secure_clear(g_archive_s3_secret, sizeof(g_archive_s3_secret));
  g_archive_s3_access_revealed = false;
  g_archive_s3_secret_revealed = false;
  const bool removed = g_svc != NULL && g_svc->file_remove != NULL &&
                       g_svc->file_remove(ARCHIVE_S3_KEYS_FILE) == 0;
  copy_text(g_archive_s3_status, sizeof(g_archive_s3_status),
            removed ? "Keys cleared" : "Could not clear saved keys");
}

/* A collection can be changed from the keyboard using the familiar IA query
 * form: "collection:open_source_software AND ultimate rom collection". Only
 * the collection token is accepted as configuration; the rest remains a
 * normal text search, so user input can never inject arbitrary Solr syntax. */
static void archive_apply_collection_query(char *phrase, size_t capacity)
{
  if (!has_prefix_ascii_case_insensitive(phrase, "collection:")) return;
  const char *cursor = phrase + 11;
  if (*cursor == '\0') {
    phrase[0] = '\0';
    return;
  }
  while (*cursor == ' ' || *cursor == '\t') cursor++;
  const char *collection_end = cursor + strlen(cursor);
  const char *remainder_start = NULL;
  const size_t phrase_length = strlen(cursor);
  for (size_t i = 0; i + 5 <= phrase_length; i++) {
    const char *p = cursor + i;
    if ((p[0] == ' ' || p[0] == '\t') &&
        (p[1] == 'A' || p[1] == 'a') &&
        (p[2] == 'N' || p[2] == 'n') &&
        (p[3] == 'D' || p[3] == 'd') &&
        (p[4] == ' ' || p[4] == '\t')) {
      collection_end = p;
      remainder_start = p + 5;
      break;
    }
  }
  while (collection_end > cursor &&
         (collection_end[-1] == ' ' || collection_end[-1] == '\t')) collection_end--;
  char candidate[sizeof(g_archive_collection)];
  size_t used = 0;
  bool previous_separator = false;
  for (const char *p = cursor; p < collection_end; p++) {
    char c = *p;
    if (c == ' ' || c == '\t') {
      if (used == 0 || previous_separator) continue;
      c = '_';
      previous_separator = true;
    } else {
      previous_separator = false;
      if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    }
    if (used + 1 >= sizeof(candidate)) return;
    candidate[used++] = c;
  }
  while (used > 0 && candidate[used - 1] == '_') used--;
  candidate[used] = '\0';
  if (!archive_collection_id_valid(candidate)) return;
  if (remainder_start != NULL) {
    cursor = remainder_start;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    char remainder[96];
    copy_text(remainder, sizeof(remainder), cursor);
    copy_text(phrase, capacity, remainder);
  } else {
    phrase[0] = '\0';
  }
  if (!archive_collection_equal(candidate, g_archive_collection)) {
    copy_text(g_archive_collection, sizeof(g_archive_collection), candidate);
    save_archive_collection();
    g_svc->log_printf("launcher_ui Archive.org default collection: %s\n", g_archive_collection);
  }
}

static bool archive_extension_supported(const char *name)
{
  static const char *extensions[] = {
    ".zip", ".7z", ".nes", ".fds", ".sfc", ".smc", ".gb", ".gbc", ".gba",
    ".pce", ".d64", ".t64", ".tap", ".crt", ".md", ".gen", ".sms", ".gg",
    ".a26", ".a78", ".lnx", ".ws", ".wsc", ".ngp", ".ngc", ".rom", ".iso",
    ".cue", ".chd", ".adf", ".dsk", ".woz", ".wad", ".pak", ".grp", ".mix"
  };
  for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
    if (ends_with(name, extensions[i])) return true;
  return false;
}

static bool archive_url_encode(char *out, size_t capacity, const char *value, bool keep_slashes)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t used = 0;
  if (out == NULL || capacity == 0) return false;
  out[0] = '\0';
  if (value == NULL) return true;
  for (size_t i = 0; value[i] != '\0'; i++) {
    const unsigned char c = (unsigned char)value[i];
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                       c == '~' || (keep_slashes && c == '/');
    const size_t needed = plain ? 1 : 3;
    if (used + needed >= capacity) return false;
    if (plain) out[used++] = (char)c;
    else {
      out[used++] = '%';
      out[used++] = hex[c >> 4];
      out[used++] = hex[c & 0x0f];
    }
  }
  out[used] = '\0';
  return true;
}

static void refresh_archive_auth_state(void)
{
  char cookie[1100] = "";
  g_archive_authenticated = g_svc != NULL && g_svc->archive_auth_get != NULL &&
                            g_svc->archive_auth_get(cookie, sizeof(cookie)) > 0;
  copy_text(g_archive_auth_status, sizeof(g_archive_auth_status),
            g_archive_authenticated ? "Signed in (this boot)" : "Not signed in");
  secure_clear(cookie, sizeof(cookie));
}

static void begin_archive_auth_edit(int field)
{
  if (field < 0 || field > 3) return;
  copy_text(g_search_before_settings_edit, sizeof(g_search_before_settings_edit), g_search_text);
  const char *value = field == 0 ? g_archive_email :
                      field == 1 ? g_archive_password :
                      field == 2 ? g_archive_s3_access : g_archive_s3_secret;
  copy_text(g_search_text, sizeof(g_search_text), value);
  g_archive_auth_edit_field = field;
  g_network_edit_field = -1;
  g_keyboard_focus = 0;
  g_keyboard_open = true;
  g_toolbar_focus = 7;
}

static void commit_archive_auth_edit(void)
{
  if (g_archive_auth_edit_field == 0) {
    copy_text(g_archive_email, sizeof(g_archive_email), g_search_text);
  } else if (g_archive_auth_edit_field == 1) {
    copy_text(g_archive_password, sizeof(g_archive_password), g_search_text);
  } else if (g_archive_auth_edit_field == 2) {
    copy_text(g_archive_s3_access, sizeof(g_archive_s3_access), g_search_text);
    g_archive_s3_access_revealed = false;
  } else if (g_archive_auth_edit_field == 3) {
    copy_text(g_archive_s3_secret, sizeof(g_archive_s3_secret), g_search_text);
    g_archive_s3_secret_revealed = false;
  }
  const bool s3_edit = g_archive_auth_edit_field == 2 || g_archive_auth_edit_field == 3;
  secure_clear(g_search_text, sizeof(g_search_text));
  copy_text(g_search_text, sizeof(g_search_text), g_search_before_settings_edit);
  secure_clear(g_search_before_settings_edit, sizeof(g_search_before_settings_edit));
  g_archive_auth_edit_field = -1;
  g_toolbar_focus = -1;
  if (s3_edit) (void)save_archive_s3_keys();
}

static void archive_account_action(void)
{
  if (g_svc == NULL || g_svc->archive_auth_set == NULL || g_svc->archive_auth_get == NULL) {
    copy_text(g_settings_message, sizeof(g_settings_message),
              "Archive.org login needs the latest firmware; update the device first");
    secure_clear(g_archive_password, sizeof(g_archive_password));
    return;
  }
  if (g_archive_authenticated) {
    (void)g_svc->archive_auth_set("", "");
    g_archive_authenticated = false;
    remove_archive_credentials();
    copy_text(g_archive_auth_status, sizeof(g_archive_auth_status), "Signed out");
    copy_text(g_settings_message, sizeof(g_settings_message), "Archive.org session and saved login cleared");
    return;
  }
  if (g_archive_email[0] == '\0' || g_archive_password[0] == '\0') {
    copy_text(g_settings_message, sizeof(g_settings_message), "Enter your Archive.org email and password first");
    return;
  }
  if (!archive_url_encode(g_archive_encoded_email, sizeof(g_archive_encoded_email),
                          g_archive_email, false) ||
      !archive_url_encode(g_archive_encoded_password, sizeof(g_archive_encoded_password),
                          g_archive_password, false)) {
    copy_text(g_settings_message, sizeof(g_settings_message), "Archive.org login details are too long");
    secure_clear(g_archive_password, sizeof(g_archive_password));
    return;
  }
  g_archive_form_body[0] = '\0';
  copy_text(g_archive_form_body, sizeof(g_archive_form_body), "email=");
  append_text(g_archive_form_body, sizeof(g_archive_form_body), g_archive_encoded_email);
  append_text(g_archive_form_body, sizeof(g_archive_form_body), "&password=");
  append_text(g_archive_form_body, sizeof(g_archive_form_body), g_archive_encoded_password);
  const int length = http_post_form("https://archive.org/services/xauthn/?op=login",
                                    g_archive_form_body, g_remote, REMOTE_BYTES);
  secure_clear(g_archive_encoded_email, sizeof(g_archive_encoded_email));
  secure_clear(g_archive_encoded_password, sizeof(g_archive_encoded_password));
  secure_clear(g_archive_form_body, sizeof(g_archive_form_body));
  if (length <= 0) {
    secure_clear(g_archive_password, sizeof(g_archive_password));
    copy_text(g_archive_auth_status, sizeof(g_archive_auth_status), "Sign-in failed");
    if (g_http_status_code == 401 || g_http_status_code == 403)
      copy_text(g_settings_message, sizeof(g_settings_message),
                "Archive.org denied sign-in; check credentials or account access");
    else
      copy_text(g_settings_message, sizeof(g_settings_message), "Archive.org sign-in request failed; check network");
    secure_clear(g_remote, REMOTE_BYTES);
    return;
  }
  const int success_key = find_bytes(g_remote, length, "\"success\"");
  int success_value = success_key < 0 ? -1 : success_key + 9;
  while (success_value >= 0 && success_value < length &&
         (g_remote[success_value] == ' ' || g_remote[success_value] == '\t' ||
          g_remote[success_value] == '\r' || g_remote[success_value] == '\n')) success_value++;
  if (success_value < 0 || success_value >= length || g_remote[success_value] != ':') {
    /* The key scan may land before the colon if whitespace follows the name. */
    success_value = success_key < 0 ? -1 : success_key + 8;
    while (success_value >= 0 && success_value < length && g_remote[success_value] != ':') success_value++;
    if (success_value >= 0 && success_value < length) success_value++;
  } else {
    success_value++;
  }
  while (success_value >= 0 && success_value < length &&
         (g_remote[success_value] == ' ' || g_remote[success_value] == '\t' ||
          g_remote[success_value] == '\r' || g_remote[success_value] == '\n')) success_value++;
  if (success_value < 0 || success_value + 4 > length ||
      memcmp(g_remote + success_value, "true", 4) != 0) {
    secure_clear(g_archive_password, sizeof(g_archive_password));
    copy_text(g_archive_auth_status, sizeof(g_archive_auth_status), "Sign-in failed");
    copy_text(g_settings_message, sizeof(g_settings_message),
              "Archive.org rejected the login; verify the account and try again");
    secure_clear(g_remote, REMOTE_BYTES);
    return;
  }
  g_archive_logged_in_user[0] = '\0';
  g_archive_logged_in_sig[0] = '\0';
  const bool have_cookies = json_string_field(g_remote, length, "logged-in-user",
                                               g_archive_logged_in_user,
                                               sizeof(g_archive_logged_in_user)) &&
                            json_string_field(g_remote, length, "logged-in-sig",
                                               g_archive_logged_in_sig,
                                               sizeof(g_archive_logged_in_sig));
  const bool credentials_saved = save_archive_credentials();
  if (!have_cookies || g_svc->archive_auth_set(g_archive_logged_in_user,
                                                g_archive_logged_in_sig) != 0) {
    copy_text(g_archive_auth_status, sizeof(g_archive_auth_status), "Sign-in could not be saved");
    copy_text(g_settings_message, sizeof(g_settings_message), "Login succeeded but the device could not keep the session");
  } else {
    g_archive_authenticated = true;
    copy_text(g_archive_auth_status, sizeof(g_archive_auth_status), "Signed in (this boot)");
    copy_text(g_settings_message, sizeof(g_settings_message),
              credentials_saved ? "Signed in; login saved on SD card" :
                                  "Signed in; mount SD to save the login");
  }
  secure_clear(g_archive_logged_in_user, sizeof(g_archive_logged_in_user));
  secure_clear(g_archive_logged_in_sig, sizeof(g_archive_logged_in_sig));
  secure_clear(g_archive_password, sizeof(g_archive_password));
  secure_clear(g_remote, REMOTE_BYTES);
}

static void archive_s3_check_action(void)
{
  if (g_archive_s3_access[0] == '\0' || g_archive_s3_secret[0] == '\0') {
    copy_text(g_archive_s3_status, sizeof(g_archive_s3_status), "Enter both S3 keys first");
    copy_text(g_settings_message, sizeof(g_settings_message), "Enter and save both Archive S3 keys first");
    return;
  }
  const int length = http_fetch("https://s3.us.archive.org/?check_auth=1", g_remote, REMOTE_BYTES);
  if (length <= 0) {
    if (g_http_status_code == 401 || g_http_status_code == 403) {
      copy_text(g_archive_s3_status, sizeof(g_archive_s3_status), "S3 keys rejected");
      copy_text(g_settings_message, sizeof(g_settings_message), "Archive S3 rejected the key pair");
    } else {
      copy_text(g_archive_s3_status, sizeof(g_archive_s3_status), "S3 key check failed (network)");
      copy_text(g_settings_message, sizeof(g_settings_message), "Could not reach Archive S3 key-check endpoint");
    }
    secure_clear(g_remote, REMOTE_BYTES);
    return;
  }
  char username[64] = "";
  if (g_http_status_code == 200 &&
      json_string_field(g_remote, length, "username", username, sizeof(username)) &&
      username[0] != '\0') {
    copy_text(g_archive_s3_status, sizeof(g_archive_s3_status), "S3 keys verified");
    copy_text(g_settings_message, sizeof(g_settings_message), "Archive S3 keys verified successfully");
    g_svc->log_printf("launcher_ui: Archive S3 key pair verified\n");
  } else {
    copy_text(g_archive_s3_status, sizeof(g_archive_s3_status), "S3 key check was not accepted");
    copy_text(g_settings_message, sizeof(g_settings_message),
              g_http_status_code == 200 ? "Archive S3 response did not include an account" :
                                           "Archive S3 key check failed; review network/status");
  }
  secure_clear(username, sizeof(username));
  secure_clear(g_remote, REMOTE_BYTES);
}

static void archive_url_decode(char *out, size_t capacity, const char *value, int length)
{
  size_t used = 0;
  if (capacity == 0) return;
  for (int i = 0; i < length && used + 1 < capacity; i++) {
    char c = value[i];
    if (c == '+' ) c = ' ';
    else if (c == '%' && i + 2 < length) {
      int hi = value[i + 1] >= '0' && value[i + 1] <= '9' ? value[i + 1] - '0' :
               value[i + 1] >= 'a' && value[i + 1] <= 'f' ? value[i + 1] - 'a' + 10 :
               value[i + 1] >= 'A' && value[i + 1] <= 'F' ? value[i + 1] - 'A' + 10 : -1;
      int lo = value[i + 2] >= '0' && value[i + 2] <= '9' ? value[i + 2] - '0' :
               value[i + 2] >= 'a' && value[i + 2] <= 'f' ? value[i + 2] - 'a' + 10 :
               value[i + 2] >= 'A' && value[i + 2] <= 'F' ? value[i + 2] - 'A' + 10 : -1;
      if (hi >= 0 && lo >= 0) { c = (char)((hi << 4) | lo); i += 2; }
    }
    if (c == '\0' || c == '/' || c == '\\' || c == ':') {
      /* A member filename is a single safe path component. */
      if (c == '/' || c == '\\' || c == ':') c = '_';
      else continue;
    }
    out[used++] = c;
  }
  out[used] = '\0';
}

static int json_array_start(const char *json, int length, const char *key)
{
  char quoted[48];
  size_t key_length = 0;
  while (key[key_length] != '\0' && key_length + 3 < sizeof(quoted)) {
    quoted[key_length + 1] = key[key_length];
    key_length++;
  }
  if (key[key_length] != '\0') return -1;
  quoted[0] = '"'; quoted[key_length + 1] = '"'; quoted[key_length + 2] = '\0';
  int cursor = find_bytes(json, length, quoted);
  if (cursor < 0) return -1;
  cursor += (int)key_length + 2;
  while (cursor < length && json[cursor] != ':') cursor++;
  if (cursor >= length) return -1;
  cursor++;
  while (cursor < length && (json[cursor] == ' ' || json[cursor] == '\t' ||
                             json[cursor] == '\r' || json[cursor] == '\n')) cursor++;
  return cursor < length && json[cursor] == '[' ? cursor : -1;
}

static void json_string_array_field(const char *json, int length, const char *key,
                                    char *out, size_t capacity)
{
  if (out == NULL || capacity == 0) return;
  out[0] = '\0';
  int cursor = json_array_start(json, length, key);
  if (cursor < 0) return;
  cursor++;
  while (cursor < length && json[cursor] != ']') {
    while (cursor < length && (json[cursor] == ' ' || json[cursor] == '\t' ||
                               json[cursor] == '\r' || json[cursor] == '\n' ||
                               json[cursor] == ',')) cursor++;
    if (cursor >= length || json[cursor] == ']') break;
    if (json[cursor++] != '"') {
      while (cursor < length && json[cursor] != ',' && json[cursor] != ']') cursor++;
      continue;
    }
    char item[96];
    size_t used = 0;
    while (cursor < length && json[cursor] != '"') {
      char c = json[cursor++];
      if (c == '\\' && cursor < length) {
        c = json[cursor++];
        if (c == 'n' || c == 'r' || c == 't') c = ' ';
      }
      if (used + 1 < sizeof(item)) item[used++] = c;
    }
    if (cursor < length && json[cursor] == '"') cursor++;
    item[used] = '\0';
    if (used == 0) continue;
    /* Keep control bindings one per line in the information view. The long
     * pipe-separated form was hard to scan and often made the JSON detail
     * page look like a single clipped line instead of a scrollable document. */
    if (out[0] != '\0') append_text(out, capacity, "\n  - ");
    append_text(out, capacity, item);
  }
}

static int json_object_end(const char *json, int length, int start)
{
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (int i = start; i < length; i++) {
    const char c = json[i];
    if (in_string) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '{') depth++;
    else if (c == '}' && --depth == 0) return i + 1;
  }
  return -1;
}

static bool json_bool_field(const char *json, int length, const char *key)
{
  char quoted[48];
  size_t key_length = 0;
  while (key[key_length] != '\0' && key_length + 3 < sizeof(quoted)) {
    quoted[key_length + 1] = key[key_length];
    key_length++;
  }
  if (key[key_length] != '\0') return false;
  quoted[0] = '"'; quoted[key_length + 1] = '"'; quoted[key_length + 2] = '\0';
  int cursor = find_bytes(json, length, quoted);
  if (cursor < 0) return false;
  cursor += (int)key_length + 2;
  while (cursor < length && json[cursor] != ':') cursor++;
  if (cursor >= length) return false;
  cursor++;
  while (cursor < length && (json[cursor] == ' ' || json[cursor] == '\t' ||
                             json[cursor] == '\r' || json[cursor] == '\n')) cursor++;
  return cursor + 4 <= length && memcmp(json + cursor, "true", 4) == 0;
}

static bool detail_glob_match(const char *pattern, const char *name)
{
  const char *star = NULL;
  const char *resume = NULL;
  while (*name != '\0') {
    char p = *pattern, n = *name;
    if (p >= 'A' && p <= 'Z') p = (char)(p + ('a' - 'A'));
    if (n >= 'A' && n <= 'Z') n = (char)(n + ('a' - 'A'));
    if (*pattern == '*') {
      star = pattern++;
      resume = name;
    } else if (*pattern == '?' || (p != '\0' && p == n)) {
      pattern++;
      name++;
    } else if (star != NULL) {
      pattern = star + 1;
      name = ++resume;
    } else {
      return false;
    }
  }
  while (*pattern == '*') pattern++;
  return *pattern == '\0';
}

static bool detail_directory_has_match(const char *folder, const char *pattern)
{
  if (g_svc == NULL || g_svc->file_list_dir == NULL) return false;
  memset(g_listing, 0, sizeof(g_listing));
  const int bytes = g_svc->file_list_dir(folder, g_listing, sizeof(g_listing));
  if (bytes < 0) return false;
  const int limit = bytes < (int)sizeof(g_listing) ? bytes : (int)sizeof(g_listing);
  int cursor = 0;
  while (cursor < limit) {
    const char *name = g_listing + cursor;
    size_t length = 0;
    while (cursor + (int)length < limit && name[length] != '\0') length++;
    if (cursor + (int)length >= limit) break;
    cursor += (int)length + 1;
    if (length == 0 || name[length - 1] == '/') continue;
    if (detail_glob_match(pattern, name)) return true;
  }
  return false;
}

static bool detail_file_exists(const char *path)
{
  if (path == NULL || path[0] == '\0' || g_svc == NULL) return false;
  const size_t length = strlen(path);
  if (length == 0 || length >= PATH_BYTES) return false;

  if (path[length - 1] == '/') {
    if (g_svc->file_list_dir == NULL) return false;
    memset(g_listing, 0, sizeof(g_listing));
    return g_svc->file_list_dir(path, g_listing, sizeof(g_listing)) >= 0;
  }

  const char *leaf = NULL;
  bool has_pattern = false;
  for (const char *p = path; *p != '\0'; p++) {
    if (*p == '/') leaf = p;
  }
  if (leaf != NULL) {
    for (const char *p = leaf + 1; *p != '\0'; p++)
      if (*p == '*' || *p == '?') has_pattern = true;
  }
  if (leaf != NULL && has_pattern) {
    char folder[PATH_BYTES];
    size_t folder_length = (size_t)(leaf - path) + 1;
    if (folder_length >= sizeof(folder)) return false;
    memcpy(folder, path, folder_length);
    folder[folder_length] = '\0';
    return detail_directory_has_match(folder, leaf + 1);
  }

  if (g_svc->file_open == NULL || g_svc->file_close == NULL) return false;
  void *file = g_svc->file_open(path, "rb");
  if (file == NULL) return false;
  (void)g_svc->file_close(file);
  return true;
}

static void detail_add_required_file(const char *path, const char *note,
                                     bool optional, bool download)
{
  if (path == NULL || path[0] == '\0') return;
  for (int i = 0; i < g_detail_file_count; i++)
    if (strcmp(g_detail_files[i].path, path) == 0) return;
  if (g_detail_file_count >= MAX_DETAIL_FILES) return;
  detail_file_t *file = &g_detail_files[g_detail_file_count++];
  memset(file, 0, sizeof(*file));
  copy_text(file->path, sizeof(file->path), path);
  copy_text(file->note, sizeof(file->note), note != NULL ? note : "");
  file->optional = optional;
  file->download = download;
  file->found = detail_file_exists(path);
}

static void detail_parse_required_array(const char *json, int length,
                                        const char *path_key, bool data_files)
{
  int cursor = json_array_start(json, length, data_files ? "files" : "requires");
  if (cursor < 0) return;
  cursor++;
  while (cursor < length && json[cursor] != ']') {
    while (cursor < length && (json[cursor] == ' ' || json[cursor] == '\t' ||
                               json[cursor] == '\r' || json[cursor] == '\n' ||
                               json[cursor] == ',')) cursor++;
    if (cursor >= length || json[cursor] == ']') break;
    if (json[cursor] != '{') {
      while (cursor < length && json[cursor] != ',' && json[cursor] != ']') cursor++;
      continue;
    }
    const int end = json_object_end(json, length, cursor);
    if (end <= cursor) break;
    char path[PATH_BYTES];
    char note[128];
    path[0] = note[0] = '\0';
    (void)json_string_field(json + cursor, end - cursor, path_key, path, sizeof(path));
    (void)json_string_field(json + cursor, end - cursor, "note", note, sizeof(note));
    if (data_files && path[0] != '\0') {
      char target[PATH_BYTES];
      copy_text(target, sizeof(target), path);
      copy_text(path, sizeof(path), "/sd/");
      append_text(path, sizeof(path), target);
    }
    if (path[0] != '\0')
      detail_add_required_file(path, note,
                               json_bool_field(json + cursor, end - cursor, "optional"),
                               data_files || json_bool_field(json + cursor, end - cursor, "download"));
    cursor = end;
  }
}

static void detail_collect_required_files(const char *json, int length)
{
  g_detail_file_count = 0;
  memset(g_detail_files, 0, sizeof(g_detail_files));
  detail_parse_required_array(json, length, "path", false);

  /* The previous launcher also included data.files as store-managed files.
   * Scope the lookup to the data object so an unrelated top-level "files"
   * field can never be mistaken for requirements. */
  int data = find_bytes(json, length, "\"data\"");
  if (data >= 0) {
    while (data < length && json[data] != ':') data++;
    while (data < length && json[data] != '{' && json[data] != '[') data++;
    if (data < length && json[data] == '{') {
      const int end = json_object_end(json, length, data);
      if (end > data) detail_parse_required_array(json + data, end - data, "target", true);
    }
  }
}

static bool extract_relative_path_valid(const char *path)
{
  if (path == NULL || path[0] == '\0' || path[0] == '/' || path[0] == '\\') return false;
  size_t segment = 0;
  bool dot = true;
  bool dotdot = true;
  for (const char *p = path; ; ++p) {
    const char c = *p;
    if (c == '\\') return false;
    if (c == '/' || c == '\0') {
      if (segment == 0 || dot || dotdot) return false;
      if (c == '\0') return true;
      segment = 0;
      dot = true;
      dotdot = true;
      continue;
    }
    if (segment >= 127) return false;
    if (segment == 0) {
      dot = c == '.';
      dotdot = c == '.';
    } else if (segment == 1) {
      dot = false;
      dotdot = dotdot && c == '.';
    } else {
      dot = false;
      dotdot = false;
    }
    segment++;
  }
}

static bool extract_target_path_valid(const char *path)
{
  if (path == NULL ||
      !(has_prefix(path, "/sd/roms/") || has_prefix(path, "/usb0/roms/")))
    return false;
  for (const char *p = path; *p; ++p) {
    if (*p == '\\') return false;
    if (p[0] == '.' && p[1] == '.' && (p == path || p[-1] == '/') &&
        (p[2] == '\0' || p[2] == '/')) return false;
    if (p[0] == '/' && p[1] == '/') return false;
  }
  return path[strlen(path) - 1] != '/';
}

static bool extract_archive_dir_valid(const char *path)
{
  if (path == NULL ||
      !(has_prefix(path, "/sd/roms/") || has_prefix(path, "/usb0/roms/")))
    return false;
  for (const char *p = path; *p; ++p) {
    if (*p == '\\') return false;
    if (p[0] == '.' && p[1] == '.' && (p == path || p[-1] == '/') &&
        (p[2] == '\0' || p[2] == '/')) return false;
    if (p[0] == '/' && p[1] == '/') return false;
  }
  return path[strlen(path) - 1] != '/';
}

static bool extract_recipe_parse_object(const char *json, int length,
                                        extract_recipe_t *recipe)
{
  memset(recipe, 0, sizeof(*recipe));
  int cursor = find_bytes(json, length, "\"extract\"");
  if (cursor < 0) return false;
  cursor += 9;
  while (cursor < length && json[cursor] != ':') cursor++;
  if (cursor >= length) return false;
  cursor++;
  while (cursor < length && (json[cursor] == ' ' || json[cursor] == '\t' ||
                             json[cursor] == '\r' || json[cursor] == '\n')) cursor++;
  if (cursor >= length || json[cursor] != '{') return false;
  const int extract_start = cursor;
  const int extract_end = json_object_end(json, length, extract_start);
  if (extract_end <= extract_start) return false;
  const char *extract_json = json + extract_start;
  const int extract_length = extract_end - extract_start;
  uint32_t version = 0;
  if (!json_uint_field(extract_json, extract_length, "version", &version) || (version != 1 && version != 2) ||
      !json_string_field(extract_json, extract_length, "archive_dir",
                         recipe->archive_dir, sizeof(recipe->archive_dir)) ||
      !extract_archive_dir_valid(recipe->archive_dir)) return false;

  /* Version 2 setup and ZIP recipes are interpreted by the separate recipe app.
   * The launcher keeps only enough state to expose its handoff action. */
  if (version == 2) {
    /* Entry-task parsing is serial. Keep this validation scratch in the
     * PAX's PSRAM data instead of consuming half of pax_main's 8 KiB stack. */
    static char instructions[4096];
    if (!json_string_field(extract_json, extract_length, "instructions", instructions, sizeof(instructions))) return false;
    recipe->valid = true;
    return true;
  }
  cursor = json_array_start(extract_json, extract_length, "media");
  if (cursor < 0) return false;
  cursor++;
  while (cursor < extract_length && extract_json[cursor] != ']') {
    while (cursor < extract_length && (extract_json[cursor] == ' ' ||
           extract_json[cursor] == '\t' || extract_json[cursor] == '\r' ||
           extract_json[cursor] == '\n' || extract_json[cursor] == ',')) cursor++;
    if (cursor >= extract_length || extract_json[cursor] == ']') break;
    if (extract_json[cursor] != '{') return false;
    const int media_end = json_object_end(extract_json, extract_length, cursor);
    if (media_end <= cursor || recipe->media_count >= MAX_EXTRACT_MEDIA) return false;
    extract_media_t *media = &recipe->media[recipe->media_count];
    const char *media_json = extract_json + cursor;
    const int media_length = media_end - cursor;
    if (!json_string_field(media_json, media_length, "format",
                           media->format, sizeof(media->format))) return false;
    media->auto_entry = json_bool_field(media_json, media_length, "auto_entry");
    const bool has_entry = json_string_field(media_json, media_length, "entry",
                                              media->entry, sizeof(media->entry));
    if (media->auto_entry) {
      if (strcmp(media->format, "zip-iso9660") != 0 || has_entry) return false;
    } else if (!has_entry || !extract_relative_path_valid(media->entry)) {
      return false;
    }
    (void)json_string_field(media_json, media_length, "zip_name",
                            media->zip_name, sizeof(media->zip_name));
    if (media->zip_name[0] != '\0' &&
        (extract_has_char(media->zip_name, '/') ||
         extract_has_char(media->zip_name, '\\') ||
         !extract_ends_with_ci(media->zip_name, ".zip"))) return false;
    const bool iso = strcmp(media->format, "zip-iso9660") == 0 &&
                     (media->auto_entry || ends_with(media->entry, ".iso"));
    const bool cue = strcmp(media->format, "zip-cue-bin") == 0 &&
                     ends_with(media->entry, ".cue");
    if (!iso && !cue) return false;

    int files_cursor = json_array_start(media_json, media_length, "files");
    if (files_cursor < 0) return false;
    files_cursor++;
    media->first_file = recipe->file_count;
    while (files_cursor < media_length && media_json[files_cursor] != ']') {
      while (files_cursor < media_length && (media_json[files_cursor] == ' ' ||
             media_json[files_cursor] == '\t' || media_json[files_cursor] == '\r' ||
             media_json[files_cursor] == '\n' || media_json[files_cursor] == ','))
        files_cursor++;
      if (files_cursor >= media_length || media_json[files_cursor] == ']') break;
      if (media_json[files_cursor] != '{' || recipe->file_count >= MAX_EXTRACT_FILES)
        return false;
      const int file_end = json_object_end(media_json, media_length, files_cursor);
      if (file_end <= files_cursor) return false;
      extract_file_t *file = &recipe->files[recipe->file_count];
      const char *file_json = media_json + files_cursor;
      const int file_length = file_end - files_cursor;
      if (!json_string_field(file_json, file_length, "source",
                             file->source, sizeof(file->source)) ||
          !json_string_field(file_json, file_length, "target",
                             file->target, sizeof(file->target)) ||
          !extract_relative_path_valid(file->source) ||
          !extract_target_path_valid(file->target)) return false;
      file->optional = json_bool_field(file_json, file_length, "optional");
      for (int previous = 0; previous < recipe->file_count; previous++)
        if (strcmp(recipe->files[previous].target, file->target) == 0) return false;

      bool listed = false;
      for (int required = 0; required < g_detail_file_count; required++)
        if (strcmp(g_detail_files[required].path, file->target) == 0) listed = true;
      if (!listed) {
        if (g_detail_file_count >= MAX_DETAIL_FILES) return false;
        detail_add_required_file(file->target, "Extracted from game installation media",
                                 file->optional, false);
      }
      recipe->file_count++;
      media->file_count++;
      files_cursor = file_end;
    }
    if (media->file_count == 0) return false;
    recipe->media_count++;
    cursor = media_end;
  }
  if (recipe->media_count == 0 || recipe->file_count == 0) return false;
  recipe->valid = true;
  return true;
}

static void detail_load_extract_recipe(const char *json, int length)
{
  memset(&g_detail_extract_recipe, 0, sizeof(g_detail_extract_recipe));
  (void)extract_recipe_parse_object(json, length, &g_detail_extract_recipe);
  /* Existing cards may still have the first Red Alert recipe, which only
   * mapped Allied MAIN.MIX.  Keep that card's detail page useful by exposing
   * the missing base REDALERT.MIX target until its JSON sidecar is refreshed. */
  if (g_detail_extract_recipe.valid &&
      ends_with(g_detail_recipe_path, "/redalert.json")) {
    const char *base_target = "/sd/roms/redalert/REDALERT.MIX";
    bool mapped = false;
    for (int i = 0; i < g_detail_extract_recipe.file_count; ++i)
      if (strcmp(g_detail_extract_recipe.files[i].target, base_target) == 0)
        mapped = true;
    if (!mapped && g_detail_extract_recipe.file_count < MAX_EXTRACT_FILES) {
      for (int i = 0; i < g_detail_extract_recipe.media_count; ++i) {
        extract_media_t *media = &g_detail_extract_recipe.media[i];
        if (strcmp(media->format, "zip-iso9660") != 0 || !media->auto_entry ||
            strcmp(media->zip_name, "cncredalertalliedcd1.zip") != 0 ||
            media->first_file + media->file_count != g_detail_extract_recipe.file_count)
          continue;
        extract_file_t *file =
            &g_detail_extract_recipe.files[g_detail_extract_recipe.file_count++];
        memset(file, 0, sizeof(*file));
        copy_text(file->source, sizeof(file->source), "INSTALL/REDALERT.MIX");
        copy_text(file->target, sizeof(file->target), base_target);
        detail_add_required_file(base_target, "Required Red Alert base game data",
                                 false, false);
        media->file_count++;
        if (g_svc != NULL && g_svc->log_printf != NULL)
          g_svc->log_printf("launcher_ui: added legacy Red Alert extraction mapping for %s\n",
                            base_target);
        break;
      }
    }
  }
  /* The separate installer accepts a local JSON path, not remote JSON bytes.
   * Keep the extraction action hidden unless its recipe can be reopened from
   * storage after the launcher unloads. */
  if (g_detail_recipe_path[0] == '\0')
    g_detail_extract_recipe.valid = false;
  g_detail_action_count = g_detail_extract_recipe.valid ? DETAIL_ACTION_COUNT :
                                                          DETAIL_ACTIONS_DEFAULT;
}

static bool extract_char_equal(char a, char b)
{
  if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
  if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
  return a == b;
}

static bool extract_ends_with_ci(const char *text, const char *suffix)
{
  if (!text || !suffix) return false;
  const size_t text_len = strlen(text);
  const size_t suffix_len = strlen(suffix);
  if (text_len < suffix_len) return false;
  for (size_t i = 0; i < suffix_len; ++i)
    if (!extract_char_equal(text[text_len - suffix_len + i], suffix[i])) return false;
  return true;
}

static const char *extract_find_last_char(const char *text, char wanted)
{
  const char *found = NULL;
  if (text == NULL) return NULL;
  for (const char *p = text; *p != '\0'; ++p)
    if (*p == wanted) found = p;
  return found;
}

static bool extract_has_char(const char *text, char wanted)
{
  if (text == NULL) return false;
  for (const char *p = text; *p != '\0'; ++p)
    if (*p == wanted) return true;
  return false;
}

static bool extract_path_join(char *out, size_t capacity,
                              const char *directory, const char *name)
{
  if (!out || !directory || !name || name[0] == '\0') return false;
  copy_text(out, capacity, directory);
  if (!append_text_checked(out, capacity, "/") ||
      !append_text_checked(out, capacity, name)) return false;
  return true;
}

static void extract_make_temp_name(const extract_media_t *media, int media_index,
                                   const char *extension, char *out, size_t capacity)
{
  copy_text(out, capacity, g_detail_extract_recipe.archive_dir);
  append_text(out, capacity, "/.esphomebrew-");
  for (size_t i = 0; g_detail_name[i] != '\0'; ++i) {
    if (out[0] == '\0' || strlen(out) + 2 >= capacity) break;
    const char c = g_detail_name[i];
    const char safe = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
    const size_t used = strlen(out);
    out[used] = safe;
    out[used + 1] = '\0';
  }
  append_text(out, capacity, "-");
  append_uint(out, capacity, (uint32_t)media_index);
  append_text(out, capacity, ".");
  append_text(out, capacity, extension);
  (void)media;
}

static bool extract_is_mounted_path(const char *path)
{
  if (g_svc == NULL || g_svc->file_stat == NULL) return false;
  pax_file_stat_t info = {0};
  return g_svc->file_stat(path, &info) == 0 && info.is_dir;
}

static bool extract_create_directory_tree(const char *path)
{
  if (g_svc == NULL || g_svc->file_mkdir == NULL ||
      !(extract_archive_dir_valid(path) || extract_target_path_valid(path)))
    return false;
  char directory[PATH_BYTES];
  copy_text(directory, sizeof(directory), path);
  for (char *p = directory + 1; ; ++p) {
    if (*p != '/' && *p != '\0') continue;
    const char saved = *p;
    *p = '\0';
    if (g_svc->file_mkdir(directory) != 0 && !extract_is_mounted_path(directory)) return false;
    *p = saved;
    if (saved == '\0') break;
  }
  return true;
}

static bool extract_create_parent_dirs(const char *path)
{
  if (!extract_target_path_valid(path)) return false;
  char parent[PATH_BYTES];
  copy_text(parent, sizeof(parent), path);
  char *last_slash = (char *)extract_find_last_char(parent, '/');
  if (last_slash == NULL || last_slash == parent) return false;
  *last_slash = '\0';
  return extract_create_directory_tree(parent);
}

static bool extract_file_found(const char *path)
{
  if (g_svc == NULL || g_svc->file_stat == NULL) return false;
  pax_file_stat_t info = {0};
  return g_svc->file_stat(path, &info) == 0 && !info.is_dir;
}

static bool extract_parse_decimal(const char **cursor, unsigned max_value,
                                  unsigned *out)
{
  if (!cursor || !*cursor || !out || **cursor < '0' || **cursor > '9') return false;
  unsigned value = 0;
  while (**cursor >= '0' && **cursor <= '9') {
    const unsigned digit = (unsigned)(*(*cursor)++ - '0');
    if (value > (max_value - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  *out = value;
  return true;
}

static bool extract_cue_layout(const char *cue_path, const char *cue_entry,
                               char *bin_entry, size_t bin_entry_size,
                               iso9660_layout_t *layout)
{
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_read == NULL ||
      g_svc->file_close == NULL || !cue_path || !cue_entry || !bin_entry || !layout)
    return false;
  void *file = g_svc->file_open(cue_path, "rb");
  if (file == NULL) return false;
  char cue[8192] = {0};
  size_t bytes = g_svc->file_read(cue, 1, sizeof(cue) - 1, file);
  char extra;
  const bool too_large = g_svc->file_read(&extra, 1, 1, file) != 0;
  (void)g_svc->file_close(file);
  if (too_large || bytes == 0) return false;
  cue[bytes] = '\0';

  char current_file[192] = "";
  char selected_file[192] = "";
  bool selected_track = false;
  uint16_t selected_stride = 0;
  uint16_t selected_payload = 0;
  uint64_t selected_offset = 0;
  bool cue_index_found = false;
  char *line = cue;
  while (*line != '\0') {
    char *next = line;
    while (*next != '\0' && *next != '\n' && *next != '\r') next++;
    const char saved = *next;
    *next = '\0';
    while (*line == ' ' || *line == '\t') line++;
    const char *value = line;
    if (extract_char_equal(value[0], 'F') && extract_char_equal(value[1], 'I') &&
        extract_char_equal(value[2], 'L') && extract_char_equal(value[3], 'E') &&
        (value[4] == ' ' || value[4] == '\t')) {
      value += 5;
      while (*value == ' ' || *value == '\t') value++;
      char name[192];
      size_t used = 0;
      if (*value == '"') {
        value++;
        while (*value != '\0' && *value != '"' && used + 1 < sizeof(name))
          name[used++] = *value++;
        if (*value != '"') used = 0;
      } else {
        while (*value != '\0' && *value != ' ' && *value != '\t' &&
               used + 1 < sizeof(name)) name[used++] = *value++;
      }
      name[used] = '\0';
      while (*value == ' ' || *value == '\t') value++;
      if (used > 0 && extract_char_equal(value[0], 'B') &&
          extract_char_equal(value[1], 'I') && extract_char_equal(value[2], 'N') &&
          extract_char_equal(value[3], 'A') && extract_char_equal(value[4], 'R') &&
          extract_char_equal(value[5], 'Y') && extract_relative_path_valid(name))
        copy_text(current_file, sizeof(current_file), name);
      else
        current_file[0] = '\0';
      selected_track = false;
    } else if (extract_char_equal(value[0], 'T') && extract_char_equal(value[1], 'R') &&
               extract_char_equal(value[2], 'A') && extract_char_equal(value[3], 'C') &&
               extract_char_equal(value[4], 'K') && (value[5] == ' ' || value[5] == '\t')) {
      value += 5;
      while (*value == ' ' || *value == '\t') value++;
      unsigned track_number = 0;
      if (extract_parse_decimal(&value, 99, &track_number) && track_number > 0) {
        while (*value == ' ' || *value == '\t') value++;
        if (extract_char_equal(value[0], 'M') && extract_char_equal(value[1], 'O') &&
            extract_char_equal(value[2], 'D') && extract_char_equal(value[3], 'E') &&
            extract_char_equal(value[4], '1') && value[5] == '/') {
          value += 6;
          unsigned stride = 0;
          if (extract_parse_decimal(&value, 2352, &stride) &&
              (stride == 2048 || stride == 2352) && current_file[0] != '\0') {
            selected_track = true;
            selected_stride = (uint16_t)stride;
            selected_payload = stride == 2352 ? 16 : 0;
            copy_text(selected_file, sizeof(selected_file), current_file);
          } else selected_track = false;
        } else selected_track = false;
      } else selected_track = false;
    } else if (selected_track && extract_char_equal(value[0], 'I') &&
               extract_char_equal(value[1], 'N') && extract_char_equal(value[2], 'D') &&
               extract_char_equal(value[3], 'E') && extract_char_equal(value[4], 'X') &&
               (value[5] == ' ' || value[5] == '\t')) {
      value += 5;
      while (*value == ' ' || *value == '\t') value++;
      unsigned index_number = 0, minutes = 0, seconds = 0, frames = 0;
      if (extract_parse_decimal(&value, 99, &index_number) && index_number == 1 &&
          (*value == ' ' || *value == '\t')) {
        while (*value == ' ' || *value == '\t') value++;
        if (extract_parse_decimal(&value, 999, &minutes) && *value++ == ':' &&
            extract_parse_decimal(&value, 59, &seconds) && *value++ == ':' &&
            extract_parse_decimal(&value, 74, &frames)) {
          const uint64_t cue_frames = ((uint64_t)minutes * 60U + seconds) * 75U + frames;
          selected_offset = cue_frames * selected_stride;
          cue_index_found = true;
          break;
        }
        selected_track = false;
      }
    }
    *next = saved;
    if (saved == '\0') break;
    line = next + 1;
    if (saved == '\r' && *line == '\n') line++;
  }
  if (!cue_index_found || selected_file[0] == '\0' || selected_stride == 0 ||
      selected_payload > 16) return false;

  char bin_name[192];
  if (!extract_relative_path_valid(selected_file)) return false;
  const char *cue_slash = extract_find_last_char(cue_entry, '/');
  if (cue_slash != NULL) {
    const size_t directory_len = (size_t)(cue_slash - cue_entry + 1);
    if (directory_len + strlen(selected_file) + 1 > sizeof(bin_name)) return false;
    memcpy(bin_name, cue_entry, directory_len);
    copy_text(bin_name + directory_len, sizeof(bin_name) - directory_len, selected_file);
  } else {
    copy_text(bin_name, sizeof(bin_name), selected_file);
  }
  if (!extract_relative_path_valid(bin_name) || strlen(bin_name) >= bin_entry_size) return false;
  copy_text(bin_entry, bin_entry_size, bin_name);
  layout->track_offset = selected_offset;
  layout->sector_size = selected_stride;
  layout->user_data_offset = selected_payload;
  return true;
}

typedef struct {
  const app_services_t *svc;
  void *stream;
} extract_io_context_t;

static int extract_io_seek(void *context, long offset, int origin)
{
  extract_io_context_t *io = (extract_io_context_t *)context;
  return io->svc->file_seek(io->stream, offset, origin);
}

static long extract_io_tell(void *context)
{
  extract_io_context_t *io = (extract_io_context_t *)context;
  return io->svc->file_tell(io->stream);
}

static size_t extract_io_read(void *buffer, size_t size, size_t count, void *context)
{
  extract_io_context_t *io = (extract_io_context_t *)context;
  return io->svc->file_read(buffer, size, count, io->stream);
}

static size_t extract_io_write(const void *buffer, size_t size, size_t count,
                               void *context)
{
  extract_io_context_t *io = (extract_io_context_t *)context;
  return io->svc->file_write(buffer, size, count, io->stream);
}

static int detail_extract_ui_on_host(void *unused)
{
  (void)unused;
  if (g_detail_status_label != NULL)
    lv_label_set_text(g_detail_status_label, g_install_status);
  if (g_detail_progress != NULL) {
    const int percent = g_install_total == 0 ? 0 :
        (int)(((uint64_t)g_install_done * 100U) / g_install_total);
    lv_bar_set_value(g_detail_progress, percent > 100 ? 100 : percent, LV_ANIM_OFF);
  }
  for (int i = 0; i < g_detail_file_count; ++i) {
    detail_file_t *file = &g_detail_files[i];
    lv_obj_t *label = g_detail_file_state_labels[i];
    if (label != NULL) {
      const char *state = file->found ? "FOUND" :
                          file->download ? "NOT DOWNLOADED" :
                          file->optional ? "OPTIONAL - NOT FOUND" : "MISSING REQUIRED FILE";
      const uint32_t color = file->found ? COL_OK :
                             (file->optional || file->download) ? COL_WARN : COL_ERROR;
      lv_label_set_text(label, state);
      lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    }
  }
  if (g_detail_action_count > 3 && g_detail_buttons[3] != NULL) {
    lv_obj_clear_state(g_detail_buttons[3], LV_STATE_DISABLED);
    if (g_detail_button_labels[3] != NULL)
      lv_label_set_text(g_detail_button_labels[3], detail_action_text(3));
  }
  return 0;
}

static void detail_extract_report(const char *status, uint32_t done, uint32_t total)
{
  copy_text(g_install_status, sizeof(g_install_status), status);
  g_install_done = done;
  g_install_total = total;
  if (g_svc != NULL && g_svc->ui_call_sync != NULL)
    (void)g_svc->ui_call_sync(detail_extract_ui_on_host, NULL, 1500);
}

static bool detail_extract_find_zip(const extract_media_t *media, int media_index,
                                    char *archive_out, size_t archive_capacity,
                                    char *image_out, size_t image_capacity,
                                    iso9660_layout_t *layout_out)
{
  if (g_svc == NULL || g_svc->dir_open == NULL || g_svc->dir_read == NULL ||
      g_svc->dir_close == NULL || g_svc->file_zip_extract_named == NULL ||
      g_svc->file_remove == NULL || !media || !archive_out || !image_out ||
      !layout_out) return false;
  const bool is_iso = strcmp(media->format, "zip-iso9660") == 0;
  char first_temp[PATH_BYTES];
  char second_temp[PATH_BYTES];
  extract_make_temp_name(media, media_index, is_iso ? "iso" : "cue",
                         first_temp, sizeof(first_temp));
  extract_make_temp_name(media, media_index, "bin", second_temp, sizeof(second_temp));
  if (strlen(first_temp) >= sizeof(first_temp) - 1 ||
      strlen(second_temp) >= sizeof(second_temp) - 1) return false;

  if (!extract_create_directory_tree(g_detail_extract_recipe.archive_dir)) return false;
  void *directory = g_svc->dir_open(g_detail_extract_recipe.archive_dir);
  if (directory == NULL) return false;
  bool found = false;
  char name[PATH_BYTES];
  int is_dir = 0;
  unsigned entries = 0;
  while (entries++ < 512 && g_svc->dir_read(directory, name, sizeof(name), &is_dir) > 0) {
    if (is_dir || !extract_ends_with_ci(name, ".zip")) continue;
    if (media->zip_name[0] != '\0') {
      size_t i = 0;
      while (media->zip_name[i] != '\0' && name[i] != '\0' &&
             extract_char_equal(media->zip_name[i], name[i])) i++;
      if (media->zip_name[i] != '\0' || name[i] != '\0') continue;
    }
    if (!extract_path_join(archive_out, archive_capacity,
                           g_detail_extract_recipe.archive_dir, name)) continue;
    (void)g_svc->file_remove(first_temp);
    (void)g_svc->file_remove(second_temp);
    uint32_t extracted = 0;
    if (is_iso) {
      if (g_svc->file_zip_extract_named(archive_out, media->entry, first_temp,
                                        EXTRACT_MAX_IMAGE_BYTES, &extracted) != 0)
        continue;
      copy_text(image_out, image_capacity, first_temp);
      *layout_out = (iso9660_layout_t){.track_offset = 0,
                                       .sector_size = 2048,
                                       .user_data_offset = 0};
      found = true;
      break;
    }

    if (g_svc->file_zip_extract_named(archive_out, media->entry, first_temp,
                                      64U * 1024U, &extracted) != 0)
      continue;
    char bin_entry[192];
    iso9660_layout_t cue_layout = {0};
    if (!extract_cue_layout(first_temp, media->entry, bin_entry, sizeof(bin_entry),
                            &cue_layout) ||
        g_svc->file_zip_extract_named(archive_out, bin_entry, second_temp,
                                      EXTRACT_MAX_IMAGE_BYTES, &extracted) != 0) {
      (void)g_svc->file_remove(first_temp);
      (void)g_svc->file_remove(second_temp);
      continue;
    }
    copy_text(image_out, image_capacity, second_temp);
    *layout_out = cue_layout;
    found = true;
    break;
  }
  (void)g_svc->dir_close(directory);
  if (!found) {
    (void)g_svc->file_remove(first_temp);
    (void)g_svc->file_remove(second_temp);
  }
  return found;
}

static int detail_extract_one_file(const char *image_path, const char *source,
                                   const char *target,
                                   const iso9660_layout_t *layout)
{
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_close == NULL ||
      g_svc->file_seek == NULL || g_svc->file_tell == NULL ||
      g_svc->file_read == NULL || g_svc->file_write == NULL ||
      g_svc->file_rename == NULL || g_svc->file_remove == NULL ||
      !extract_target_path_valid(target)) return -1;
  if (extract_file_found(target)) return 1;
  if (!extract_create_parent_dirs(target)) return -2;

  char temporary[PATH_BYTES];
  copy_text(temporary, sizeof(temporary), target);
  if (!append_text_checked(temporary, sizeof(temporary), ".esphomebrew-tmp")) return -2;
  (void)g_svc->file_remove(temporary);
  void *input = g_svc->file_open(image_path, "rb");
  if (input == NULL) return -3;
  if (g_svc->file_seek(input, 0, 2) != 0) {
    (void)g_svc->file_close(input);
    return -3;
  }
  const long image_size = g_svc->file_tell(input);
  if (image_size <= 0 || g_svc->file_seek(input, 0, 0) != 0) {
    (void)g_svc->file_close(input);
    return -3;
  }
  void *output = g_svc->file_open(temporary, "wb");
  if (output == NULL) {
    (void)g_svc->file_close(input);
    return -4;
  }
  extract_io_context_t input_context = {.svc = g_svc, .stream = input};
  extract_io_context_t output_context = {.svc = g_svc, .stream = output};
  const iso9660_io_t input_io = {
    .context = &input_context, .seek = extract_io_seek, .tell = extract_io_tell,
    .read = extract_io_read, .write = extract_io_write,
  };
  const iso9660_io_t output_io = {
    .context = &output_context, .seek = extract_io_seek, .tell = extract_io_tell,
    .read = extract_io_read, .write = extract_io_write,
  };
  uint64_t copied = 0;
  const int result = iso9660_copy_file(&input_io, &output_io, source, layout, image_size,
                                       EXTRACT_MAX_FILE_BYTES, &copied);
  const int output_close = g_svc->file_close(output);
  (void)g_svc->file_close(input);
  if (result != 0 || output_close != 0 || copied == 0) {
    (void)g_svc->file_remove(temporary);
    return result == -3 ? -5 : -6;
  }
  /* Never replace a game file that appeared while the extraction was running. */
  if (extract_file_found(target) || g_svc->file_rename(temporary, target) != 0) {
    (void)g_svc->file_remove(temporary);
    return extract_file_found(target) ? 1 : -7;
  }
  return 0;
}

static bool detail_extract_recipe_run(void)
{
  if (!g_detail_extract_recipe.valid || g_svc == NULL ||
      g_svc->file_zip_extract_named == NULL) {
    copy_text(g_install_status, sizeof(g_install_status),
              "No supported ZIP/ISO or ZIP/CUE recipe is available.");
    return false;
  }
  int missing_required = 0;
  for (int i = 0; i < g_detail_file_count; ++i) {
    if (g_detail_files[i].found) continue;
    bool mapped = false;
    for (int j = 0; j < g_detail_extract_recipe.file_count; ++j)
      if (strcmp(g_detail_files[i].path,
                 g_detail_extract_recipe.files[j].target) == 0) mapped = true;
    if (!g_detail_files[i].optional && !mapped) missing_required++;
  }
  detail_extract_report("Looking for installation ZIPs...", 0,
                        (uint32_t)g_detail_extract_recipe.file_count);
  uint32_t completed = 0;
  for (int media_index = 0; media_index < g_detail_extract_recipe.media_count; ++media_index) {
    const extract_media_t *media = &g_detail_extract_recipe.media[media_index];
    bool media_needed = false;
    for (int i = 0; i < media->file_count; ++i) {
      const extract_file_t *mapping =
          &g_detail_extract_recipe.files[media->first_file + i];
      if (!extract_file_found(mapping->target)) media_needed = true;
    }
    if (!media_needed) {
      completed += (uint32_t)media->file_count;
      detail_extract_report("All mapped files for this disc are already present.",
                            completed, (uint32_t)g_detail_extract_recipe.file_count);
      continue;
    }

    char archive[PATH_BYTES] = "";
    char image_path[PATH_BYTES] = "";
    iso9660_layout_t layout = {0};
    char status[96];
    copy_text(status, sizeof(status), "Staging a disc image from ZIP...");
    detail_extract_report(status, completed,
                          (uint32_t)g_detail_extract_recipe.file_count);
    if (!detail_extract_find_zip(media, media_index, archive, sizeof(archive),
                                 image_path, sizeof(image_path), &layout)) {
      copy_text(status, sizeof(status), "No matching ZIP/CUE image found in recipe folder.");
      detail_extract_report(status, completed,
                            (uint32_t)g_detail_extract_recipe.file_count);
      return false;
    }

    for (int i = 0; i < media->file_count; ++i) {
      const extract_file_t *mapping =
          &g_detail_extract_recipe.files[media->first_file + i];
      if (extract_file_found(mapping->target)) {
        for (int required = 0; required < g_detail_file_count; ++required)
          if (strcmp(g_detail_files[required].path, mapping->target) == 0)
            g_detail_files[required].found = true;
        completed++;
        detail_extract_report("Keeping existing game files; no overwrite performed.",
                              completed, (uint32_t)g_detail_extract_recipe.file_count);
        continue;
      }
      copy_text(status, sizeof(status), "Extracting required file ");
      append_uint(status, sizeof(status), completed + 1U);
      append_text(status, sizeof(status), " of ");
      append_uint(status, sizeof(status), (uint32_t)g_detail_extract_recipe.file_count);
      append_text(status, sizeof(status), "...");
      detail_extract_report(status, completed,
                            (uint32_t)g_detail_extract_recipe.file_count);
      const int result = detail_extract_one_file(image_path, mapping->source,
                                                 mapping->target, &layout);
      if (result < 0) {
        copy_text(status, sizeof(status), "Extraction failed for ");
        append_text(status, sizeof(status), mapping->source);
        append_text(status, sizeof(status), " (error ");
        append_uint(status, sizeof(status), (uint32_t)(-result));
        append_text(status, sizeof(status), ").");
        detail_extract_report(status, completed,
                              (uint32_t)g_detail_extract_recipe.file_count);
        (void)g_svc->file_remove(image_path);
        return false;
      }
      for (int required = 0; required < g_detail_file_count; ++required)
        if (strcmp(g_detail_files[required].path, mapping->target) == 0)
          g_detail_files[required].found = true;
      completed++;
      detail_extract_report("Extracted file safely; checking the next item...", completed,
                            (uint32_t)g_detail_extract_recipe.file_count);
    }
    (void)g_svc->file_remove(image_path);
  }

  for (int i = 0; i < g_detail_file_count; ++i)
    g_detail_files[i].found = extract_file_found(g_detail_files[i].path);
  if (missing_required == 0)
    copy_text(g_install_status, sizeof(g_install_status),
              "Recipe files are ready; existing files were left unchanged.");
  else {
    copy_text(g_install_status, sizeof(g_install_status), "Extracted mapped files; ");
    append_uint(g_install_status, sizeof(g_install_status), (uint32_t)missing_required);
    append_text(g_install_status, sizeof(g_install_status),
                " other required file(s) still missing.");
  }
  g_install_done = (uint32_t)g_detail_extract_recipe.file_count;
  g_install_total = (uint32_t)g_detail_extract_recipe.file_count;
  if (g_svc->ui_call_sync != NULL)
    (void)g_svc->ui_call_sync(detail_extract_ui_on_host, NULL, 1500);
  return true;
}

static int archive_add_record(const char *name, const char *path, const char *filename, int type)
{
  if (g_app_count >= MAX_APPS || name == NULL || path == NULL) return -1;
  const int slot = g_app_count++;
  copy_text(g_names[slot], NAME_BYTES, name);
  copy_text(g_paths[slot], PATH_BYTES, path);
  copy_text(g_archive_filenames[slot], sizeof(g_archive_filenames[slot]), filename);
  g_item_source[slot] = SOURCE_ARCHIVE;
  g_archive_record_type[slot] = type;
  return slot;
}

static void archive_search_load(void)
{
  g_app_count = 0;
  g_view_count = 0;
  g_catalog_failed = false;
  copy_text(g_archive_status, sizeof(g_archive_status), "Searching collection: ");
  append_text(g_archive_status, sizeof(g_archive_status), g_archive_collection);
  append_text(g_archive_status, sizeof(g_archive_status), "...");
  char phrase[96];
  copy_text(phrase, sizeof(phrase), g_search_text);
  archive_apply_collection_query(phrase, sizeof(phrase));
  char query[SEARCH_QUERY_BYTES] = {0};
  if (phrase[0] == '\0') {
    if (!append_text_checked(query, sizeof(query), "collection:") ||
        !append_text_checked(query, sizeof(query), g_archive_collection)) {
      g_catalog_failed = true;
      copy_text(g_archive_status, sizeof(g_archive_status), "Archive collection query is too long");
      return;
    }
  } else {
    char clean[96];
    copy_text(clean, sizeof(clean), phrase);
    for (size_t i = 0; clean[i] != '\0'; i++)
      if (clean[i] == '"' || clean[i] == '\\') clean[i] = ' ';
    bool query_ok = append_text_checked(query, sizeof(query), "collection:") &&
                    append_text_checked(query, sizeof(query), g_archive_collection) &&
                    append_text_checked(query, sizeof(query), " AND (identifier:");
    bool slug_started = false;
    for (size_t i = 0; clean[i] != '\0'; i++) {
      char c = clean[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_') {
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        char character[2] = {c, '\0'};
        query_ok = query_ok && append_text_checked(query, sizeof(query), character);
        slug_started = true;
      } else if (slug_started && clean[i] == ' ') {
        query_ok = query_ok && append_text_checked(query, sizeof(query), "-");
        slug_started = false;
      }
    }
    query_ok = query_ok && append_text_checked(query, sizeof(query), " OR title:\"") &&
               append_text_checked(query, sizeof(query), clean) &&
               append_text_checked(query, sizeof(query), "\" OR description:\"") &&
               append_text_checked(query, sizeof(query), clean) &&
               append_text_checked(query, sizeof(query), "\")");
    if (!query_ok) {
      g_catalog_failed = true;
      copy_text(g_archive_status, sizeof(g_archive_status), "Archive search query is too long");
      return;
    }
  }
  char encoded[SEARCH_ENCODED_BYTES];
  if (!archive_url_encode(encoded, sizeof(encoded), query, false)) {
    g_catalog_failed = true;
    copy_text(g_archive_status, sizeof(g_archive_status), "Archive search encoding is too long");
    return;
  }
  /* Archive.org query URLs grow quickly because the search phrase is
   * percent-encoded and the API fields are appended after it.  PATH_BYTES is
   * intended for on-card paths; keep the network URL in its own larger buffer. */
  char url[SEARCH_URL_BYTES] = "https://archive.org/advancedsearch.php?q=";
  if (!append_text_checked(url, sizeof(url), encoded) ||
      !append_text_checked(url, sizeof(url), "&fl%5B%5D=identifier&fl%5B%5D=title&rows=40&page=1&output=json")) {
    g_catalog_failed = true;
    copy_text(g_archive_status, sizeof(g_archive_status), "Search URL too long; try a shorter query");
    return;
  }
  g_svc->log_printf("launcher_ui Archive.org search request: %u URL bytes, collection='%s'\n",
                    (unsigned)strlen(url), g_archive_collection);
  const int length = http_fetch(url, g_remote, REMOTE_BYTES);
  if (length < 0) {
    g_catalog_failed = true;
    copy_text(g_archive_status, sizeof(g_archive_status), "Archive.org search unavailable");
    return;
  }
  const int docs = json_array_start(g_remote, length, "docs");
  if (docs < 0) {
    g_catalog_failed = true;
    copy_text(g_archive_status, sizeof(g_archive_status), "Archive.org response was not valid search JSON");
    return;
  }
  int cursor = docs + 1;
  while (cursor < length && g_app_count < 40) {
    if (g_remote[cursor] == ']') break;
    if (g_remote[cursor] != '{') { cursor++; continue; }
    const int end = json_object_end(g_remote, length, cursor);
    if (end < 0) break;
    char identifier[128], title[NAME_BYTES];
    if (json_string_field(g_remote + cursor, end - cursor, "identifier", identifier, sizeof(identifier)) &&
        identifier[0] != '\0') {
      (void)json_string_field(g_remote + cursor, end - cursor, "title", title, sizeof(title));
      if (title[0] == '\0') copy_text(title, sizeof(title), identifier);
      (void)archive_add_record(title, identifier, "", ARCHIVE_RECORD_ITEM);
    }
    cursor = end;
  }
  if (g_app_count == 0) {
    copy_text(g_archive_status, sizeof(g_archive_status), "No matching items in ");
    append_text(g_archive_status, sizeof(g_archive_status), g_archive_collection);
  }
  else {
    copy_text(g_archive_status, sizeof(g_archive_status), "Collection: ");
    append_text(g_archive_status, sizeof(g_archive_status), g_archive_collection);
    append_text(g_archive_status, sizeof(g_archive_status), "  |  A OPEN  |  B BACK");
    g_svc->log_printf("launcher_ui Archive.org search: %d results for '%s'\n", g_app_count, phrase);
  }
}

static bool archive_item_url(char *out, size_t capacity, const char *identifier, const char *file)
{
  char safe_identifier[128];
  char safe_file[256];
  archive_url_encode(safe_identifier, sizeof(safe_identifier), identifier, true);
  archive_url_encode(safe_file, sizeof(safe_file), file, true);
  copy_text(out, capacity, "https://archive.org/download/");
  return append_text_checked(out, capacity, safe_identifier) && append_text_checked(out, capacity, "/") &&
         append_text_checked(out, capacity, safe_file);
}

static void archive_metadata_load(void)
{
  g_app_count = 0;
  g_view_count = 0;
  g_catalog_failed = false;
  copy_text(g_archive_status, sizeof(g_archive_status), "Loading item files...");
  char safe_identifier[128], url[PATH_BYTES] = "https://archive.org/metadata/";
  archive_url_encode(safe_identifier, sizeof(safe_identifier), g_archive_identifier, true);
  if (!append_text_checked(url, sizeof(url), safe_identifier)) {
    g_catalog_failed = true;
    copy_text(g_archive_status, sizeof(g_archive_status), "Item identifier is too long");
    return;
  }
  const int length = http_fetch(url, g_remote, REMOTE_BYTES);
  if (length < 0) {
    g_catalog_failed = true;
    copy_text(g_archive_status, sizeof(g_archive_status), "Archive.org item files unavailable");
    return;
  }
  const int files = json_array_start(g_remote, length, "files");
  if (files < 0) {
    g_catalog_failed = true;
    copy_text(g_archive_status, sizeof(g_archive_status), "Item response has no file list");
    return;
  }
  int cursor = files + 1;
  while (cursor < length && g_app_count < MAX_APPS) {
    if (g_remote[cursor] == ']') break;
    if (g_remote[cursor] != '{') { cursor++; continue; }
    const int end = json_object_end(g_remote, length, cursor);
    if (end < 0) break;
    char filename[128];
    if (json_string_field(g_remote + cursor, end - cursor, "name", filename, sizeof(filename)) &&
        filename[0] != '\0' && archive_extension_supported(filename)) {
      char item_url[PATH_BYTES];
      if (archive_item_url(item_url, sizeof(item_url), g_archive_identifier, filename)) {
        const int type = ends_with(filename, ".zip") ? ARCHIVE_RECORD_ZIP : ARCHIVE_RECORD_FILE;
        (void)archive_add_record(filename, item_url, filename, type);
      }
    }
    cursor = end;
  }
  if (g_app_count == 0) copy_text(g_archive_status, sizeof(g_archive_status), "No supported ROM/archive files listed");
  else {
    char status[128];
    copy_text(status, sizeof(status), g_archive_title);
    append_text(status, sizeof(status), "  |  A OPEN  |  B BACK");
    copy_text(g_archive_status, sizeof(g_archive_status), status);
  }
}

static bool archive_query_value(const char *href, const char *key, char *out, size_t capacity)
{
  const char *query = find_char(href, '?');
  if (query == NULL) return false;
  query++;
  const size_t key_length = strlen(key);
  while (*query != '\0') {
    const char *end = find_char(query, '&');
    size_t length = end == NULL ? strlen(query) : (size_t)(end - query);
    if (length > key_length && memcmp(query, key, key_length) == 0 && query[key_length] == '=') {
      archive_url_decode(out, capacity, query + key_length + 1, (int)(length - key_length - 1));
      return out[0] != '\0';
    }
    if (end == NULL) break;
    query = end + 1;
  }
  return false;
}

static bool archive_resolve_href(const char *base, const char *href, char *label, size_t label_capacity,
                                 char *url, size_t url_capacity)
{
  char clean_href[PATH_BYTES];
  copy_text(clean_href, sizeof(clean_href), href);
  for (size_t i = 0; clean_href[i] != '\0'; i++) {
    if (clean_href[i] == '#') { clean_href[i] = '\0'; break; }
    if (clean_href[i] == '&' && clean_href[i + 1] == 'a' && clean_href[i + 2] == 'm' &&
        clean_href[i + 3] == 'p' && clean_href[i + 4] == ';') {
      clean_href[i] = '&';
      size_t j = i + 1;
      while (clean_href[j + 4] != '\0') { clean_href[j] = clean_href[j + 4]; j++; }
      clean_href[j] = '\0';
    }
  }
  char archive[PATH_BYTES], member[PATH_BYTES];
  if (has_prefix(clean_href, "view_archive.php") &&
      archive_query_value(clean_href, "archive", archive, sizeof(archive)) &&
      archive_query_value(clean_href, "file", member, sizeof(member))) {
    const char *items = find_substring(archive, "/items/");
    if (items == NULL) return false;
    items += 7;
    char encoded_archive[PATH_BYTES], encoded_member[PATH_BYTES];
    archive_url_encode(encoded_archive, sizeof(encoded_archive), items, true);
    archive_url_encode(encoded_member, sizeof(encoded_member), member, true);
    copy_text(url, url_capacity, "https://archive.org/download/");
    const bool url_ok = append_text_checked(url, url_capacity, encoded_archive) &&
                        append_text_checked(url, url_capacity, "/") &&
                        append_text_checked(url, url_capacity, encoded_member);
    archive_url_decode(label, label_capacity, member, (int)strlen(member));
    return url_ok;
  }
  const char *plain_end = find_first_of(clean_href, "?&");
  if (plain_end != NULL) clean_href[(size_t)(plain_end - clean_href)] = '\0';
  size_t href_length = strlen(clean_href);
  if (href_length == 0 || clean_href[href_length - 1] == '/' ||
      has_prefix(clean_href, "../") || has_prefix(clean_href, "javascript:")) return false;
  const char *last = find_last_char(clean_href, '/');
  const char *raw_name = last != NULL ? last + 1 : clean_href;
  if (*raw_name == '\0') return false;
  archive_url_decode(label, label_capacity, raw_name, (int)strlen(raw_name));
  if (!archive_extension_supported(label)) return false;
  url[0] = '\0';
  if (is_archive_plain_http_url(clean_href)) {
    copy_text(url, url_capacity, "https://");
    return append_text_checked(url, url_capacity, clean_href + 7);
  }
  if (has_prefix(clean_href, "http://") || has_prefix(clean_href, "https://"))
    return append_text_checked(url, url_capacity, clean_href);
  if (clean_href[0] == '/')
    return append_text_checked(url, url_capacity, "https://archive.org") &&
           append_text_checked(url, url_capacity, clean_href);
  return append_text_checked(url, url_capacity, base) && append_text_checked(url, url_capacity, clean_href);
}

static void archive_zip_load(void)
{
  g_app_count = 0;
  g_view_count = 0;
  g_catalog_failed = false;
  copy_text(g_archive_status, sizeof(g_archive_status), "Loading ZIP contents...");
  char listing_url[PATH_BYTES];
  copy_text(listing_url, sizeof(listing_url), g_archive_zip_url);
  size_t used = strlen(listing_url);
  if (used + 2 < sizeof(listing_url) && listing_url[used - 1] != '/') {
    listing_url[used++] = '/'; listing_url[used] = '\0';
  }
  const int length = http_fetch(listing_url, g_remote, REMOTE_BYTES);
  if (length < 0) {
    g_catalog_failed = true;
    copy_text(g_archive_status, sizeof(g_archive_status), "ZIP contents unavailable; check connection");
    return;
  }
  int cursor = 0;
  while (cursor < length && g_app_count < MAX_APPS) {
    int href = -1;
    for (int i = cursor; i + 5 < length; i++) {
      if ((g_remote[i] == 'h' || g_remote[i] == 'H') &&
          (g_remote[i + 1] == 'r' || g_remote[i + 1] == 'R') &&
          (g_remote[i + 2] == 'e' || g_remote[i + 2] == 'E') &&
          (g_remote[i + 3] == 'f' || g_remote[i + 3] == 'F')) { href = i + 4; break; }
    }
    if (href < 0) break;
    while (href < length && (g_remote[href] == ' ' || g_remote[href] == '\t')) href++;
    if (href >= length || g_remote[href++] != '=') { cursor = href; continue; }
    while (href < length && (g_remote[href] == ' ' || g_remote[href] == '\t')) href++;
    if (href >= length || (g_remote[href] != '\'' && g_remote[href] != '"')) { cursor = href; continue; }
    const char quote = g_remote[href++];
    int end = href;
    while (end < length && g_remote[end] != quote) end++;
    if (end >= length) break;
    char href_value[PATH_BYTES];
    const int href_length = end - href < PATH_BYTES - 1 ? end - href : PATH_BYTES - 1;
    memcpy(href_value, g_remote + href, (size_t)href_length);
    href_value[href_length] = '\0';
    char label[128], url[PATH_BYTES];
    if (archive_resolve_href(listing_url, href_value, label, sizeof(label), url, sizeof(url)))
      (void)archive_add_record(label, url, label, ARCHIVE_RECORD_MEMBER);
    cursor = end + 1;
  }
  if (g_app_count == 0) copy_text(g_archive_status, sizeof(g_archive_status), "No supported files in this ZIP");
  else copy_text(g_archive_status, sizeof(g_archive_status), "ZIP contents  |  A DOWNLOAD  |  B BACK");
}

static void archive_load_current(void)
{
  if (g_archive_level == ARCHIVE_LEVEL_ITEM) archive_metadata_load();
  else if (g_archive_level == ARCHIVE_LEVEL_ZIP) archive_zip_load();
  else archive_search_load();
}

static const char *archive_destination(const char *filename)
{
  if (ends_with(filename, ".nes") || ends_with(filename, ".fds")) return "/sd/roms/nes/";
  if (ends_with(filename, ".sfc") || ends_with(filename, ".smc")) return "/sd/roms/snes/";
  if (ends_with(filename, ".gb") || ends_with(filename, ".gbc")) return "/sd/roms/gb/";
  if (ends_with(filename, ".gba")) return "/sd/roms/gba/";
  if (ends_with(filename, ".pce")) return "/sd/roms/pce/";
  if (ends_with(filename, ".d64") || ends_with(filename, ".t64") || ends_with(filename, ".tap") ||
      ends_with(filename, ".crt")) return "/sd/roms/c64/";
  if (ends_with(filename, ".md") || ends_with(filename, ".gen")) return "/sd/roms/genesis/";
  if (ends_with(filename, ".sms") || ends_with(filename, ".gg")) return "/sd/roms/sms/";
  if (ends_with(filename, ".a26")) return "/sd/roms/stella/";
  if (ends_with(filename, ".a78")) return "/sd/roms/prosystem/";
  if (ends_with(filename, ".lnx")) return "/sd/roms/handy/";
  if (ends_with(filename, ".ws") || ends_with(filename, ".wsc")) return "/sd/roms/ws/";
  if (ends_with(filename, ".ngp") || ends_with(filename, ".ngc")) return "/sd/roms/ngp/";
  if (ends_with(filename, ".wad")) return "/sd/roms/doom/";
  if (ends_with(filename, ".pak")) return "/sd/roms/quake/id1/";
  if (ends_with(filename, ".grp")) return "/sd/roms/duke3d/";
  if (ends_with(filename, ".mix")) return "/sd/roms/redalert/";
  return "/sd/roms/archive/";
}

static void archive_start_download(int index)
{
  if (index < 0 || index >= g_app_count || g_svc->archive_download_start == NULL) {
    copy_text(g_archive_status, sizeof(g_archive_status), "Archive download is not supported by this firmware");
    return;
  }
  const char *filename = g_archive_filenames[index];
  if (filename[0] == '\0') filename = g_names[index];
  char safe_name[128];
  copy_text(safe_name, sizeof(safe_name), filename);
  for (size_t i = 0; safe_name[i] != '\0'; i++) {
    const char c = safe_name[i];
    if (c == '/' || c == '\\' || c == ':' || c == '\0') safe_name[i] = '_';
    else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ' ')) safe_name[i] = '_';
  }
  if (safe_name[0] == '\0' || contains_substring(safe_name, "..")) {
    copy_text(g_archive_status, sizeof(g_archive_status), "Unsafe filename; refusing download");
    return;
  }
  char destination[PATH_BYTES];
  copy_text(destination, sizeof(destination), archive_destination(filename));
  if (!append_text_checked(destination, sizeof(destination), safe_name)) {
    copy_text(g_archive_status, sizeof(g_archive_status), "Download path is too long");
    return;
  }
  const int result = g_svc->archive_download_start(g_paths[index], destination);
  if (result == 0) {
    g_archive_download_active = true;
    g_install_done = 0;
    g_install_total = 0;
    copy_text(g_archive_status, sizeof(g_archive_status), "Download starting...");
    g_svc->log_printf("launcher_ui Archive.org download: %s -> %s\n", g_paths[index], destination);
  } else {
    copy_text(g_archive_status, sizeof(g_archive_status), result == -2 ?
              "A download is already running" : "Could not start download; check SD/network");
  }
}

static void archive_open_selected(void)
{
  const int record = selected_record_index();
  if (record < 0 || record >= g_app_count || g_item_source[record] != SOURCE_ARCHIVE) return;
  const int type = g_archive_record_type[record];
  if (type == ARCHIVE_RECORD_ITEM) {
    copy_text(g_archive_identifier, sizeof(g_archive_identifier), g_paths[record]);
    copy_text(g_archive_title, sizeof(g_archive_title), g_names[record]);
    g_archive_level = ARCHIVE_LEVEL_ITEM;
    g_selected = 0;
    copy_text(g_archive_status, sizeof(g_archive_status), "Loading item files...");
  } else if (type == ARCHIVE_RECORD_ZIP) {
    copy_text(g_archive_zip_url, sizeof(g_archive_zip_url), g_paths[record]);
    g_archive_level = ARCHIVE_LEVEL_ZIP;
    g_selected = 0;
    copy_text(g_archive_status, sizeof(g_archive_status), "Loading ZIP contents...");
  } else {
    archive_start_download(record);
    return;
  }
  g_open_index = -3;
}

static void release_detail_cover(void)
{
  if (g_detail_cover_pixels != NULL && g_svc != NULL && g_svc->mem_free != NULL)
    g_svc->mem_free(g_detail_cover_pixels);
  g_detail_cover_pixels = NULL;
  g_detail_cover_width = 0;
  g_detail_cover_height = 0;
}

/* png_load_rgb565() also supports the bare-panel renderer and returns its
 * byte/channel-swizzled pixels. LVGL image descriptors need ordinary
 * little-endian RGB565, so undo that display-specific transform here. */
static void restore_lvgl_rgb565(uint16_t *pixels, size_t count)
{
  if (pixels == NULL) return;
  for (size_t i = 0; i < count; i++) {
    if((i&4095U)==0 && __atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE))return;
    const uint16_t panel_pixel = pixels[i];
    const uint16_t rgb_swapped =
        (uint16_t)(((panel_pixel & 0x001FU) << 11) |
                   (panel_pixel & 0x07E0U) |
                   ((panel_pixel >> 11) & 0x001FU));
    pixels[i] = (uint16_t)((rgb_swapped >> 8) | (rgb_swapped << 8));
  }
}

static bool replace_package_suffix(const char *path, char *out, size_t capacity,
                                   const char *suffix)
{
  if (path == NULL || out == NULL || capacity == 0) return false;
  copy_text(out, capacity, path);
  size_t length = strlen(out);
  if (length < 4 || !ends_with(out, ".pax")) return false;
  length -= 4;
  if (length + strlen(suffix) + 1 > capacity) return false;
  memcpy(out + length, suffix, strlen(suffix) + 1);
  return true;
}

typedef struct {const char *path;char *buffer;int capacity;} FileJob;
static int read_file_job(void *argument)
{
  FileJob *job=argument;
  if(!g_svc->file_open || !g_svc->file_read || !g_svc->file_close || job->capacity<2)return -1;
  void *file=g_svc->file_open(job->path,"rb");if(!file)return -1;
  int bytes=(int)g_svc->file_read(job->buffer,1,job->capacity-1,file);
  (void)g_svc->file_close(file);job->buffer[bytes]=0;return bytes ? bytes : -1;
}
static int read_json_file(const char *path)
{
  FileJob job={path,g_remote,sizeof(g_remote)};return io_run(read_file_job,&job);
}
static int list_directory_job(void *argument)
{
  FileJob *job=argument;return g_svc->file_list_dir(job->path,job->buffer,job->capacity);
}
static int list_directory_async(const char *path,char *buffer,int capacity)
{
  FileJob job={path,buffer,capacity};return io_run(list_directory_job,&job);
}
typedef struct {const char *path;pax_file_stat_t *out;} StatJob;
static int stat_job(void *argument){StatJob *job=argument;return g_svc->file_stat(job->path,job->out);}
static int stat_async(const char *path,pax_file_stat_t *out){StatJob job={path,out};return io_run(stat_job,&job);}

static bool detail_package_present(const char *name, char *path, size_t capacity)
{
  if (g_svc == NULL || g_svc->file_stat == NULL || name == NULL || name[0] == '\0') return false;
  char safe_name[NAME_BYTES];
  size_t used = 0;
  for (size_t i = 0; name[i] != '\0' && used + 1 < sizeof(safe_name); i++) {
    const char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')
      safe_name[used++] = c;
  }
  safe_name[used] = '\0';
  if (used == 0) return false;
  const char *roots[] = {"/sd/roms/pax/", "/usb0/roms/pax/"};
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    char candidate[PATH_BYTES];
    copy_text(candidate, sizeof(candidate), roots[i]);
    if (!append_text_checked(candidate, sizeof(candidate), safe_name) ||
        !append_text_checked(candidate, sizeof(candidate), ".pax")) continue;
    pax_file_stat_t info = {0};
    if (stat_async(candidate, &info) == 0 && !info.is_dir) {
      copy_text(path, capacity, candidate);
      return true;
    }
  }
  return false;
}

/* Compare the three numeric components used by our PAX package versions.
 * A zero result also means the strings were equal or not parseable; only a
 * strictly newer remote version is advertised as an update. */
static int compare_package_versions(const char *local, const char *remote)
{
  if (local == NULL || remote == NULL || local[0] == '\0' || remote[0] == '\0') return 0;
  for (int part = 0; part < 3; part++) {
    while (*local == 'v' || *local == 'V' || *local == ' ') local++;
    while (*remote == 'v' || *remote == 'V' || *remote == ' ') remote++;
    if (*local < '0' || *local > '9' || *remote < '0' || *remote > '9') return 0;
    uint32_t a = 0, b = 0;
    while (*local >= '0' && *local <= '9') {
      a = a * 10U + (uint32_t)(*local++ - '0');
      if (a > 1000000U) return 0;
    }
    while (*remote >= '0' && *remote <= '9') {
      b = b * 10U + (uint32_t)(*remote++ - '0');
      if (b > 1000000U) return 0;
    }
    if (a != b) return a < b ? -1 : 1;
    if (part < 2) {
      if (*local == '.') local++;
      else if (*local != '\0' && *local != '-') return 0;
      if (*remote == '.') remote++;
      else if (*remote != '\0' && *remote != '-') return 0;
    }
  }
  return 0;
}

static bool remove_detail_package(void)
{
  if (g_svc == NULL || g_svc->file_remove == NULL || g_detail_local_path[0] == '\0')
    return false;
  if (g_svc->file_remove(g_detail_local_path) != 0) return false;
  char sidecar[PATH_BYTES];
  if (replace_package_suffix(g_detail_local_path, sidecar, sizeof(sidecar), ".json"))
    (void)g_svc->file_remove(sidecar);
  for (int i = 0; i < g_favorite_count; i++) {
    if (strcmp(g_favorite_paths[i], g_detail_local_path) != 0) continue;
    for (int next = i + 1; next < g_favorite_count; next++)
      copy_text(g_favorite_paths[next - 1], PATH_BYTES, g_favorite_paths[next]);
    g_favorite_count--;
    save_favorites();
    break;
  }
  g_detail_local_path[0] = '\0';
  g_detail_recipe_path[0] = '\0';
  g_detail_is_installed = false;
  g_detail_uninstall_confirm = false;
  copy_text(g_install_status, sizeof(g_install_status), "App removed; its ROMs and save files were left untouched.");
  return true;
}

typedef struct {uint16_t *pixels;uint16_t width,height;} ImageResult;
static void image_release(ImageResult *image)
{
  if(image->pixels)g_svc->mem_free(image->pixels);
  *image=(ImageResult){0};
}
static lv_image_dsc_t image_descriptor(const ImageResult *image)
{
  return (lv_image_dsc_t){
    .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,
      .w=image->width,.h=image->height,.stride=(uint16_t)(image->width*2)},
    .data_size=(uint32_t)image->width*image->height*2,.data=(const uint8_t *)image->pixels
  };
}
static const char *g_work_text;
static int update_work_on_host(void *unused)
{
  (void)unused;
  if(!g_display)return 0;
  if(!g_work_label){
    g_work_label=lv_label_create(lv_display_get_screen_active(g_display));
    lv_obj_set_style_text_font(g_work_label,&lv_font_montserrat_14,LV_PART_MAIN);
    lv_obj_set_style_text_color(g_work_label,lv_color_hex(COL_ACCENT),LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_work_label,lv_color_hex(COL_PANEL),LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_work_label,LV_OPA_COVER,LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_work_label,ui_px(2),LV_PART_MAIN);
    lv_obj_clear_flag(g_work_label,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(g_work_label,LV_ALIGN_BOTTOM_RIGHT,-ui_px(8),-ui_px(2));
  }
  if(g_work_text){
    lv_label_set_text(g_work_label,g_work_text);lv_obj_clear_flag(g_work_label,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_work_label);
  }else lv_obj_add_flag(g_work_label,LV_OBJ_FLAG_HIDDEN);
  return 0;
}
static void work_status(const char *text)
{
  g_work_text=text ? text : (g_transfer_busy ? g_transfer_message : NULL);
  if(g_display)(void)g_svc->ui_call_sync(update_work_on_host,NULL,1000);
}

typedef struct {int storage,mounted;} MountJob;
static int mount_job(void *argument)
{
  const MountJob *job=argument;
  return g_svc->storage_set_mounted ? g_svc->storage_set_mounted(job->storage,job->mounted) : -1;
}
static int network_apply_job(void *argument)
{
  return g_svc->network_config_set ? g_svc->network_config_set(argument) : -1;
}
static int wifi_apply_job(void *argument)
{
  if(!g_svc->file_open || !g_svc->file_write || !g_svc->file_close)return -1;
  void *file=g_svc->file_open(PAX_WIFI_SETTINGS_FILE,"wb");
  if(!file)return -1;
  bool accepted=g_svc->file_write(argument,1,sizeof(pax_wifi_request_t),file)==sizeof(pax_wifi_request_t);
  return g_svc->file_close(file)==0 && accepted ? 0 : -2;
}
static int run_action(int (*function)(void *),void *argument,const char *message)
{
  if(mutations_busy())return -4;
  g_action_busy=true;
  /* Clear the action that started this job. A new action now represents a
   * distinct intent; keep its dispatch after this non-cancellable mutation. */
  g_open_index=-1;
  copy_text(g_settings_message,sizeof(g_settings_message),message);
  (void)g_svc->ui_call_sync(update_selection_on_host,NULL,1000);
  work_status(message);
  int result=io_run_committed(function,argument);
  g_action_busy=false;work_status(NULL);
  g_svc->log_printf("launcher_ui action finished: result=%d navigation=%d\n",result,g_discard_io);
  return result;
}

typedef struct {
  char url[PATH_BYTES],name[65],title[97],version[33],sha256[65];uint32_t size;
} InstallJob;
static int install_start_job(void *argument)
{
  const InstallJob *job=argument;
  return g_svc->store_install_start(job->url,job->name,job->title,job->version,job->size,job->sha256);
}

typedef struct {const char *json,*path;int length;ImageResult image;} CoverJob;
static int cover_job(void *argument)
{
  CoverJob *job=argument;
  const char *temporary_paths[2] = {DETAIL_COVER_TEMP_SD, DETAIL_COVER_TEMP_USB};
  if (job->path != NULL && has_prefix(job->path, "/usb0/")) {
    temporary_paths[0] = DETAIL_COVER_TEMP_USB;
    temporary_paths[1] = DETAIL_COVER_TEMP_SD;
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    if(__atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE))break;
    const char *temporary = temporary_paths[attempt];
    if (!decode_base64_png_to_file(job->json,job->length,temporary)) {
      if (g_svc->file_remove != NULL) (void)g_svc->file_remove(temporary);
      continue;
    }
    /* PNGdec takes its composite background as 0x00BBGGRR. Blend the icon's
     * real alpha against the detail panel color so transparent artwork does
     * not reveal hidden magenta matte pixels. Keep the legacy decoder as a
     * compatibility fallback for older firmware. */
    const uint32_t panel_rgb = COL_BG;
    const uint32_t panel_bgr = ((panel_rgb & 0x0000FFU) << 16) |
                               (panel_rgb & 0x00FF00U) |
                               ((panel_rgb >> 16) & 0x0000FFU);
    if (g_svc->png_load_rgb565_blended != NULL)
      job->image.pixels = g_svc->png_load_rgb565_blended(
          temporary, &job->image.width, &job->image.height, panel_bgr);
    else if (g_svc->png_load_rgb565 != NULL)
      job->image.pixels = g_svc->png_load_rgb565(temporary, &job->image.width,
                                                      &job->image.height);
    if (g_svc->file_remove != NULL) (void)g_svc->file_remove(temporary);
    if (job->image.pixels != NULL && job->image.width > 0 &&
        job->image.height > 0) {
      restore_lvgl_rgb565(job->image.pixels,
                          (size_t)job->image.width * job->image.height);
      if(__atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE)){image_release(&job->image);return -2;}
      /* Detail art is a small UI thumbnail, not a full-screen texture. Shrink
       * it once in-place to its final pixel dimensions so LVGL doesn't scale
       * a large image around its center (which made the art appear displaced
       * down and inward on the native 1024x600 canvas). */
      const uint16_t max_dim = (uint16_t)ui_px(28);
      const uint16_t src_w = job->image.width;
      const uint16_t src_h = job->image.height;
      const uint16_t largest = src_w > src_h ? src_w : src_h;
      if (largest > max_dim) {
        uint16_t dst_w = (uint16_t)(((uint32_t)src_w * max_dim) / largest);
        uint16_t dst_h = (uint16_t)(((uint32_t)src_h * max_dim) / largest);
        if (dst_w == 0) dst_w = 1;
        if (dst_h == 0) dst_h = 1;
        for (uint16_t y = 0; y < dst_h; y++) {
          const uint16_t sy = (uint16_t)(((uint32_t)y * src_h) / dst_h);
          for (uint16_t x = 0; x < dst_w; x++) {
            const uint16_t sx = (uint16_t)(((uint32_t)x * src_w) / dst_w);
            job->image.pixels[(size_t)y * dst_w + x] =
                job->image.pixels[(size_t)sy * src_w + sx];
          }
        }
        job->image.width = dst_w;
        job->image.height = dst_h;
      }
      return 0;
    }
    image_release(&job->image);
  }
  return -1;
}

static bool load_detail_cover(const char *json,int length,const char *json_path)
{
  release_detail_cover();
  if(!g_svc->png_load_rgb565 && !g_svc->png_load_rgb565_blended)return false;
  CoverJob job={.json=json,.path=json_path,.length=length};
  work_status("Loading artwork...");uint64_t start=timing_now();
  int result=io_run(cover_job,&job);
  timing_record(&g_image_time,timing_now()-start);work_status(NULL);
  if(result!=0){image_release(&job.image);return false;}
  g_detail_cover_pixels=job.image.pixels;g_detail_cover_width=job.image.width;g_detail_cover_height=job.image.height;
  g_detail_cover_descriptor=image_descriptor(&job.image);
  return true;
}

static bool fetch_app_detail(int index)
{
  if (index < 0 || index >= g_app_count) return false;
  release_detail_cover();
  const int source = g_item_source[index];
  g_detail_is_remote = source == SOURCE_NETWORK || source == SOURCE_STORE;
  g_detail_is_installed = false;
  g_detail_uninstall_confirm = false;
  g_detail_local_path[0] = '\0';
  g_detail_name[0] = '\0';
  g_detail_title[0] = '\0';
  g_detail_version[0] = '\0';
  g_detail_local_version[0] = '\0';
  g_detail_description[0] = '\0';
  g_detail_author[0] = '\0';
  g_detail_category[0] = '\0';
  g_detail_license[0] = '\0';
  g_detail_controls[0] = '\0';
  g_detail_file_count = 0;
  memset(&g_detail_extract_recipe, 0, sizeof(g_detail_extract_recipe));
  g_detail_action_count = DETAIL_ACTIONS_DEFAULT;
  g_detail_sha256[0] = '\0';
  g_detail_size = 0;
  g_detail_install_valid = false;
  g_detail_update_available = false;
  g_install_state = 0;
  g_install_done = 0;
  g_install_total = 0;
  copy_text(g_install_status, sizeof(g_install_status), "Reading app information...");

  const char *base = g_paths[index];
  for (const char *p = g_paths[index]; *p != '\0'; p++) if (*p == '/') base = p + 1;
  copy_text(g_detail_name, sizeof(g_detail_name), base);
  if (ends_with(g_detail_name, ".pax")) g_detail_name[strlen(g_detail_name) - 4] = '\0';
  copy_text(g_detail_title, sizeof(g_detail_title), display_name_for(index));
  if (!g_titles[index][0] && ends_with(g_detail_title, ".pax")) g_detail_title[strlen(g_detail_title) - 4] = '\0';

  if (g_detail_is_remote) {
    g_detail_is_installed = detail_package_present(g_detail_name, g_detail_local_path,
                                                    sizeof(g_detail_local_path));
    if (g_detail_is_installed) {
      char local_json[PATH_BYTES];
      if (replace_package_suffix(g_detail_local_path, local_json, sizeof(local_json), ".json")) {
        const int local_length = read_json_file(local_json);
        if (local_length > 0)
          (void)json_string_field(g_remote, local_length, "version", g_detail_local_version,
                                  sizeof(g_detail_local_version));
      }
    }
  } else {
    copy_text(g_detail_local_path, sizeof(g_detail_local_path), g_paths[index]);
    g_detail_is_installed = true;
  }

  copy_text(g_detail_file_info,sizeof(g_detail_file_info),"File: ");
  append_text(g_detail_file_info,sizeof(g_detail_file_info),g_paths[index]);
  pax_file_stat_t actual={0};
  bool have_stat=g_detail_local_path[0] && g_svc->file_stat &&
                 stat_async(g_detail_local_path,&actual)==0 && !actual.is_dir;
  if(have_stat){
    if(g_detail_is_remote){append_text(g_detail_file_info,sizeof(g_detail_file_info),"\nInstalled file: ");append_text(g_detail_file_info,sizeof(g_detail_file_info),g_detail_local_path);}
    append_text(g_detail_file_info,sizeof(g_detail_file_info),"\nSize: ");pax_info_number(g_detail_file_info,sizeof(g_detail_file_info),actual.size,1);
    append_text(g_detail_file_info,sizeof(g_detail_file_info)," bytes\nModified: ");pax_info_date(g_detail_file_info,sizeof(g_detail_file_info),actual.mtime);
  }else if(g_list_stats_valid[index]){
    append_text(g_detail_file_info,sizeof(g_detail_file_info),"\nServer size: ");
    pax_info_number(g_detail_file_info,sizeof(g_detail_file_info),g_list_stats[index].size,1);
    append_text(g_detail_file_info,sizeof(g_detail_file_info)," bytes\nServer modified: ");
    pax_info_date(g_detail_file_info,sizeof(g_detail_file_info),g_list_stats[index].mtime);
  }else append_text(g_detail_file_info,sizeof(g_detail_file_info),"\nModified: Unavailable");

  char info_path[PATH_BYTES];
  if (!replace_package_suffix(g_paths[index], info_path, sizeof(info_path), ".json")) {
    copy_text(g_install_status, sizeof(g_install_status), "No JSON sidecar for this entry; launch remains available.");
    return false;
  }
  if (g_discard_io) return false;
  if (!g_detail_is_remote) {
    copy_text(g_detail_recipe_path, sizeof(g_detail_recipe_path), info_path);
  }
  int length = g_detail_is_remote ? http_fetch(info_path, g_remote, REMOTE_BYTES) :
                                   read_json_file(info_path);
  if (length <= 0) {
    copy_text(g_install_status, sizeof(g_install_status),
              "JSON unavailable; this app can still be launched.");
    return false;
  }

  char value[DETAIL_TEXT_BYTES];
  if (json_string_field(g_remote, length, "name", value, sizeof(value)) && value[0] != '\0')
    copy_text(g_detail_name, sizeof(g_detail_name), value);
  if (json_string_field(g_remote, length, "title", value, sizeof(value)) && value[0] != '\0')
    copy_text(g_detail_title, sizeof(g_detail_title), value);
  if (!json_string_field(g_remote, length, "version", g_detail_version,
                         sizeof(g_detail_version))) g_detail_version[0] = '\0';
  if (!json_string_field(g_remote, length, "description", g_detail_description,
                         sizeof(g_detail_description)) || g_detail_description[0] == '\0')
    (void)json_string_field(g_remote, length, "about", g_detail_description,
                            sizeof(g_detail_description));
  (void)json_string_field(g_remote, length, "author", g_detail_author, sizeof(g_detail_author));
  (void)json_string_field(g_remote, length, "category", g_detail_category, sizeof(g_detail_category));
  (void)json_string_field(g_remote, length, "license", g_detail_license, sizeof(g_detail_license));
  json_string_array_field(g_remote, length, "controls", g_detail_controls,
                          sizeof(g_detail_controls));
  detail_collect_required_files(g_remote, length);
  detail_load_extract_recipe(g_remote, length);
  (void)json_string_field(g_remote, length, "sha256", g_detail_sha256, sizeof(g_detail_sha256));
  (void)json_uint_field(g_remote, length, "size", &g_detail_size);
  if (!g_detail_is_remote)
    copy_text(g_detail_local_version, sizeof(g_detail_local_version), g_detail_version);
  else {
    if (!g_detail_is_installed)
      g_detail_is_installed = detail_package_present(g_detail_name, g_detail_local_path,
                                                      sizeof(g_detail_local_path));
    if (g_detail_is_installed && g_detail_local_version[0] != '\0' &&
        g_detail_version[0] != '\0')
      g_detail_update_available = compare_package_versions(g_detail_local_version,
                                                           g_detail_version) < 0;
  }
  g_detail_install_valid = g_detail_is_remote && g_detail_size > 0 &&
                           g_detail_sha256[0] != '\0' && strlen(g_detail_sha256) == 64 &&
                           g_detail_version[0] != '\0' && g_detail_name[0] != '\0' &&
                           g_svc->store_install_start != NULL;
  (void)load_detail_cover(g_remote, length, info_path);
  if (!g_detail_is_remote) {
    copy_text(g_install_status, sizeof(g_install_status),
              "Local app information loaded; CHECK STORE for the latest version.");
  } else if (g_detail_install_valid) {
    if (!g_detail_is_installed)
      copy_text(g_install_status, sizeof(g_install_status), "Verified network package; choose INSTALL to save it.");
    else if (g_detail_update_available)
      copy_text(g_install_status, sizeof(g_install_status), "A newer network version is available to install.");
    else if (g_detail_local_version[0] != '\0')
      copy_text(g_install_status, sizeof(g_install_status), "Installed app is already at this version.");
    else
      copy_text(g_install_status, sizeof(g_install_status), "Installed package found; version comparison is unavailable.");
  } else {
    copy_text(g_install_status, sizeof(g_install_status), "Listing loaded; install metadata is incomplete.");
  }
  return true;
}

static const char *detail_action_text(int action)
{
  if(action==1 && g_detail_is_remote && (g_action_busy || g_transfer_busy))return "INSTALLING...";
  if (action == 0) return "LAUNCH";
  if (action == 1)
    return g_detail_is_remote ? (g_detail_is_installed ?
           (g_detail_update_available ? "UPDATE" : "REINSTALL") : "INSTALL") : "CHECK STORE";
  if (action == 2) {
    if (!g_detail_is_installed) return "REMOVE";
    return g_detail_uninstall_confirm ? "CONFIRM REMOVE" : "UNINSTALL";
  }
  if (action == 3)
    return g_detail_extract_recipe.valid ? "GET GAME DATA" : "BACK";
  return "BACK";
}

static const char *display_name_for(int index)
{
  if (index < 0 || index >= MAX_APPS) return "PAX";
  if (g_titles[index][0] != '\0') return g_titles[index];
  if (g_names[index][0] != '\0') return g_names[index];

  /* Older loader builds and a partially populated directory buffer can leave
   * the short name empty even though the full path is valid.  The path is
   * authoritative, so derive the visible filename from it rather than
   * exposing an unhelpful placeholder. */
  const char *path = g_paths[index];
  const char *base = path;
  for (const char *cursor = path; cursor != NULL && *cursor != '\0'; cursor++) {
    if (*cursor == '/') base = cursor + 1;
  }
  if (*base != '\0') return base;
  return "PAX";
}

static bool path_is_favorite(const char *path)
{
  for (int i = 0; i < g_favorite_count; i++) {
    size_t j = 0;
    while (g_favorite_paths[i][j] != '\0' && path[j] != '\0' &&
           g_favorite_paths[i][j] == path[j]) j++;
    if (g_favorite_paths[i][j] == '\0' && path[j] == '\0') return true;
  }
  return false;
}

static void save_favorites(void)
{
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_write == NULL ||
      g_svc->file_close == NULL) return;
  void *file = g_svc->file_open(FAVORITES_FILE, "w");
  if (file == NULL) {
    g_svc->log_printf("launcher_ui: could not save favorites (%s)\n", FAVORITES_FILE);
    return;
  }
  for (int i = 0; i < g_favorite_count; i++) {
    const size_t length = strlen(g_favorite_paths[i]);
    if (length > 0) (void)g_svc->file_write(g_favorite_paths[i], 1, length, file);
    (void)g_svc->file_write("\n", 1, 1, file);
  }
  (void)g_svc->file_close(file);
}

static void load_favorites(void)
{
  g_favorite_count = 0;
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_read == NULL ||
      g_svc->file_close == NULL) return;
  void *file = g_svc->file_open(FAVORITES_FILE, "r");
  if (file == NULL) return;
  char path[PATH_BYTES];
  size_t used = 0;
  while (g_favorite_count < MAX_APPS) {
    char c = '\0';
    if (g_svc->file_read(&c, 1, 1, file) != 1) break;
    if (c == '\n' || c == '\r') {
      if (used > 0) {
        path[used] = '\0';
        if (ends_with(path, ".pax") ||
            ((has_prefix(path, "http://archive.org/download/") ||
              has_prefix(path, "https://archive.org/download/")) && archive_extension_supported(path))) {
          copy_text(g_favorite_paths[g_favorite_count], PATH_BYTES, path);
          g_favorite_count++;
        }
        used = 0;
      }
    } else if (used + 1 < sizeof(path)) {
      path[used++] = c;
    }
  }
  if (used > 0 && g_favorite_count < MAX_APPS) {
    path[used] = '\0';
    if (ends_with(path, ".pax") ||
        ((has_prefix(path, "http://archive.org/download/") ||
          has_prefix(path, "https://archive.org/download/")) && archive_extension_supported(path))) {
      copy_text(g_favorite_paths[g_favorite_count], PATH_BYTES, path);
      g_favorite_count++;
    }
  }
  (void)g_svc->file_close(file);
}

static int source_for_path(const char *path)
{
  if (has_prefix(path, "http://archive.org/download/") ||
      has_prefix(path, "https://archive.org/download/")) return SOURCE_ARCHIVE;
  if (has_prefix(path, "https://")) return SOURCE_STORE;
  if (has_prefix(path, "http://")) return SOURCE_NETWORK;
  return SOURCE_STORAGE;
}

static void load_favorites_as_catalog(void)
{
  g_app_count = 0;
  load_favorites();
  for (int i = 0; i < g_favorite_count && i < MAX_APPS; i++) {
    copy_text(g_paths[i], PATH_BYTES, g_favorite_paths[i]);
    g_item_source[i] = source_for_path(g_paths[i]);
    const char *base = g_paths[i];
    for (const char *p = g_paths[i]; *p != '\0'; p++)
      if (*p == '/') base = p + 1;
    if (g_item_source[i] == SOURCE_ARCHIVE) {
      archive_url_decode(g_archive_filenames[i], sizeof(g_archive_filenames[i]), base, (int)strlen(base));
      copy_text(g_names[i], NAME_BYTES, g_archive_filenames[i]);
      g_archive_record_type[i] = ARCHIVE_RECORD_MEMBER;
    } else {
      copy_text(g_names[i], NAME_BYTES, base);
      g_archive_filenames[i][0] = '\0';
      g_archive_record_type[i] = 0;
    }
    g_app_count++;
  }
}

static bool contains_case_insensitive(const char *text, const char *query)
{
  if (query == NULL || query[0] == '\0') return true;
  if (text == NULL) return false;
  for (size_t start = 0; text[start] != '\0'; start++) {
    size_t i = 0;
    while (query[i] != '\0' && text[start + i] != '\0') {
      char a = text[start + i];
      char b = query[i];
      if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
      if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
      if (a != b) break;
      i++;
    }
    if (query[i] == '\0') return true;
  }
  return false;
}

static void apply_filter(void)
{
  g_view_count = 0;
  /* The Archive query has already been applied server-side; filtering those
   * results again locally would hide matches for the full IA query syntax. */
  const bool apply_search = g_source != SOURCE_ARCHIVE;
  for (int i = 0; i < g_app_count; i++) {
    if (g_favorites_mode && !path_is_favorite(g_paths[i])) continue;
    if (apply_search && !contains_case_insensitive(display_name_for(i), g_search_text) &&
        !contains_case_insensitive(g_names[i], g_search_text) &&
        !contains_case_insensitive(g_paths[i], g_search_text)) continue;
    g_visible_indices[g_view_count++] = i;
  }
  /* Stable insertion sort is bounded by MAX_APPS and keeps all record arrays
   * untouched, including filenames, paths, metadata and Archive hierarchy. */
  if(g_sort_mode!=PAX_SORT_SOURCE)for(int i=1;i<g_view_count;i++){
    int record=g_visible_indices[i],j=i;
    while(j>0){
      int previous=g_visible_indices[j-1];
      if(pax_sort_compare(g_sort_mode,display_name_for(record),g_paths[record],g_list_stats_valid[record] ? &g_list_stats[record] : NULL,
                         display_name_for(previous),g_paths[previous],g_list_stats_valid[previous] ? &g_list_stats[previous] : NULL)>=0)break;
      g_visible_indices[j]=previous;j--;
    }
    g_visible_indices[j]=record;
  }
  if (g_selected >= g_view_count) g_selected = g_view_count > 0 ? g_view_count - 1 : 0;
  if (g_selected < 0) g_selected = 0;
  g_first_visible = g_selected >= VISIBLE_ROWS ? g_selected - VISIBLE_ROWS + 1 : 0;
  const int max_first = g_view_count > VISIBLE_ROWS ? g_view_count - VISIBLE_ROWS : 0;
  if (g_first_visible > max_first) g_first_visible = max_first;
}

static int selected_record_index(void)
{
  if (g_selected < 0 || g_selected >= g_view_count) return -1;
  return g_visible_indices[g_selected];
}

static bool favorite_allowed(int record)
{
  if (record < 0 || record >= g_app_count) return false;
  return g_item_source[record] != SOURCE_ARCHIVE ||
         g_archive_record_type[record] == ARCHIVE_RECORD_FILE ||
         g_archive_record_type[record] == ARCHIVE_RECORD_MEMBER;
}

static void toggle_selected_favorite(void)
{
  const int record = selected_record_index();
  if (record < 0 || record >= g_app_count) return;
  if (g_item_source[record] == SOURCE_ARCHIVE &&
      g_archive_record_type[record] != ARCHIVE_RECORD_FILE &&
      g_archive_record_type[record] != ARCHIVE_RECORD_MEMBER)
    return;
  const char *path = g_paths[record];
  for (int i = 0; i < g_favorite_count; i++) {
    size_t j = 0;
    while (g_favorite_paths[i][j] != '\0' && path[j] != '\0' &&
           g_favorite_paths[i][j] == path[j]) j++;
    if (g_favorite_paths[i][j] == '\0' && path[j] == '\0') {
      for (int next = i + 1; next < g_favorite_count; next++)
        copy_text(g_favorite_paths[next - 1], PATH_BYTES, g_favorite_paths[next]);
      g_favorite_count--;
      save_favorites();
      apply_filter();
      return;
    }
  }
  if (g_favorite_count < MAX_APPS) {
    copy_text(g_favorite_paths[g_favorite_count++], PATH_BYTES, path);
    save_favorites();
  }
  apply_filter();
}

static bool read_small_file(const char *path, char *out, size_t capacity)
{
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_read == NULL ||
      g_svc->file_close == NULL || out == NULL || capacity < 2) return false;
  void *file = g_svc->file_open(path, "r");
  if (file == NULL) return false;
  size_t used = 0;
  while (used + 1 < capacity && g_svc->file_read(&out[used], 1, 1, file) == 1) {
    if (out[used] == '\n' || out[used] == '\r') break;
    used++;
  }
  out[used] = '\0';
  (void)g_svc->file_close(file);
  return used > 0;
}

static void load_app_details_preference(void)
{
  char enabled[12];
  /* This is a new option, so a missing preference intentionally keeps the
   * improved details-first behavior enabled by default. */
  g_app_details_enabled = !read_small_file(APP_DETAILS_PREF_FILE, enabled, sizeof(enabled)) ||
                          (enabled[0] != '0' && enabled[0] != 'n' && enabled[0] != 'N');
}

static bool save_app_details_preference(void)
{
  if (g_svc == NULL || g_svc->file_open == NULL || g_svc->file_write == NULL ||
      g_svc->file_close == NULL) return false;
  void *file = g_svc->file_open(APP_DETAILS_PREF_FILE, "w");
  if (file == NULL) return false;
  const char *value = g_app_details_enabled ? "1\n" : "0\n";
  const bool saved = g_svc->file_write(value, 1, 2, file) == 2;
  return g_svc->file_close(file) == 0 && saved;
}

static void load_tooltips_preference(void)
{
  char value[12];
  g_tooltips_enabled = !read_small_file(TOOLTIPS_PREF_FILE,value,sizeof(value)) || value[0]!='0';
}

static bool save_tooltips_preference(void)
{
  if(!g_svc || !g_svc->file_open || !g_svc->file_write || !g_svc->file_close)return false;
  void *file=g_svc->file_open(TOOLTIPS_PREF_FILE,"w");if(!file)return false;
  const char *value=g_tooltips_enabled ? "1\n" : "0\n";
  bool saved=g_svc->file_write(value,1,2,file)==2;
  return g_svc->file_close(file)==0 && saved;
}

static bool normalize_network_url(const char *input, char *out, size_t capacity)
{
  if(!input || !out || capacity<2)return false;
  size_t n=strlen(input);
  while(n && (input[n-1]=='\n' || input[n-1]=='\r' || input[n-1]==' '))n--;
  const int prefix=has_prefix(input,"http://") ? 7 : has_prefix(input,"https://") ? 8 : 0;
  if(!prefix || n<=(size_t)prefix || n+2>capacity)return false;
  for(size_t i=0;i<n;i++)if((unsigned char)input[i]<=32 || (unsigned char)input[i]>126 || input[i]=='[' || input[i]==']' || input[i]=='@' || input[i]=='#' || input[i]=='?')return false;
  size_t authority_end=(size_t)prefix;
  while(authority_end<n && input[authority_end]!='/')authority_end++;
  size_t host_end=(size_t)prefix;
  while(host_end<authority_end && input[host_end]!=':')host_end++;
  if(host_end==(size_t)prefix)return false;
  for(size_t i=(size_t)prefix;i<host_end;i++){
    const char ch=input[i];
    if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='.'||ch=='-'))return false;
  }
  if(host_end<authority_end){
    unsigned port=0;
    if(host_end+1==authority_end)return false;
    for(size_t i=host_end+1;i<authority_end;i++){
      if(input[i]<'0'||input[i]>'9')return false;
      port=port*10u+(unsigned)(input[i]-'0');if(port>65535u)return false;
    }
    if(!port)return false;
  }
  memcpy(out,input,n);if(out[n-1]!='/')out[n++]='/';out[n]=0;return true;
}
static void load_network_url(void)
{
  char value[PATH_BYTES],mode[16];
  if(read_small_file(PAX_SUPPLY_AUTO_FILE,mode,sizeof(mode)))g_network_auto=strncmp(mode,"manual",6)!=0;
  if(read_small_file("/sd/roms/pax/.launcher_ui_network_url.txt",value,sizeof(value)))
    (void)normalize_network_url(value,g_network_url,sizeof(g_network_url));
}
static bool save_network_url(const char *value)
{
  char normalized[PATH_BYTES];
  if(!normalize_network_url(value,normalized,sizeof(normalized)))return false;
  if(!g_svc->file_open || !g_svc->file_write || !g_svc->file_close)return false;
  void *f=g_svc->file_open("/sd/roms/pax/.launcher_ui_network_url.txt","wb");if(!f)return false;
  size_t n=strlen(normalized);bool saved=g_svc->file_write(normalized,1,n,f)==n;
  saved=g_svc->file_close(f)==0 && saved;
  if(saved)copy_text(g_network_url,sizeof(g_network_url),normalized);
  return saved;
}
static bool save_network_mode(bool automatic)
{
  if(!g_svc->file_open || !g_svc->file_write || !g_svc->file_close)return false;
  void *file=g_svc->file_open(PAX_SUPPLY_AUTO_FILE,"wb");if(!file)return false;
  const char *mode=automatic ? "auto" : "manual";size_t length=strlen(mode);
  bool ok=g_svc->file_write(mode,1,length,file)==length;
  ok=g_svc->file_close(file)==0 && ok;
  if(ok)g_network_auto=automatic;return ok;
}
static void discover_supply_server(void)
{
  if(!g_network_auto)return;
  PaxSupplyResult result={0};work_status("Finding PAX Supply on this network...");
  int status=io_run(pax_supply_discovery_job,&result);work_status(NULL);
  if(status==0 && !g_discard_io){
    // Cache the address for older/non-advertising servers and temporary packet loss.
    if(strcmp(g_network_url,result.url)!=0){
      (void)save_network_url(result.url);copy_text(g_network_url,sizeof(g_network_url),result.url);
    }
  }
}
static void refresh_c6_status(void) {
  g_c6_available=false;
  void *f=g_svc->file_open ? g_svc->file_open(PAX_C6_UPDATE_FILE,"rb") : NULL;
  if(f){pax_c6_status_t state={0};size_t n=g_svc->file_read(&state,1,sizeof(state),f);g_svc->file_close(f);
    if(n==sizeof(state) && state.magic==PAX_C6_UPDATE_MAGIC){state.message[sizeof(state.message)-1]=0;g_c6_status=state;g_c6_available=true;}
  }
}
static void request_c6_update(uint32_t command) {
  if(mutations_busy()){copy_text(g_settings_message,sizeof(g_settings_message),"Wait for the active transfer before updating C6");return;}
  pax_c6_request_t request={0};request.magic=PAX_C6_UPDATE_MAGIC;request.command=command;
  memcpy(request.source,g_c6_source,sizeof(request.source));
  void *f=g_svc->file_open ? g_svc->file_open(PAX_C6_UPDATE_FILE,"wb") : NULL;
  bool ok=false;
  if(f){ok=g_svc->file_write(&request,1,sizeof(request),f)==sizeof(request);ok=g_svc->file_close(f)==0 && ok;}
  if(!ok)copy_text(g_settings_message,sizeof(g_settings_message),"C6 request rejected; check firmware first, or update P4 firmware");
  if(ok && command==PAX_C6_PREPARE)copy_text(g_c6_checked_source,sizeof(g_c6_checked_source),g_c6_source);
  refresh_c6_status();
}

static bool consume_c6_action(void)
{
  if(g_open_index!=-71 && g_open_index!=-72)return false;
  uint32_t command=g_open_index==-71 ? PAX_C6_PREPARE : PAX_C6_INSTALL;
  g_open_index=-1;
  request_c6_update(command);
  return true;
}

static void refresh_firmware_status(void)
{
  g_firmware_available=false;
  void *f=g_svc->file_open ? g_svc->file_open(PAX_FIRMWARE_FILE,"rb") : NULL;
  if(f){pax_firmware_status_t state={0};size_t n=g_svc->file_read(&state,1,sizeof(state),f);g_svc->file_close(f);
    if(n==sizeof(state) && state.magic==PAX_FIRMWARE_MAGIC){
      state.message[sizeof(state.message)-1]=0;state.profile[sizeof(state.profile)-1]=0;
      g_firmware_status=state;g_firmware_available=true;
    }
  }
  if(!g_firmware_available || g_firmware_status.state!=PAX_FIRMWARE_AVAILABLE ||
     g_firmware_confirm_build!=g_firmware_status.latest_build)g_firmware_confirm=false;
}
static bool consume_firmware_action(void)
{
  if(g_open_index!=-73 && g_open_index!=-74)return false;
  uint32_t command=g_open_index==-73 ? PAX_FIRMWARE_CHECK : PAX_FIRMWARE_INSTALL;
  g_open_index=-1;
  refresh_firmware_status();
  if(mutations_busy() || (g_c6_available && (g_c6_status.state==PAX_C6_BUSY || g_c6_status.state==PAX_C6_REBOOTING))){
    copy_text(g_settings_message,sizeof(g_settings_message),"Wait for the active transfer before updating firmware");return true;
  }
  if(command==PAX_FIRMWARE_INSTALL && !g_firmware_confirm){
    g_firmware_confirm=g_firmware_available && g_firmware_status.state==PAX_FIRMWARE_AVAILABLE;
    g_firmware_confirm_build=g_firmware_status.latest_build;
    copy_text(g_settings_message,sizeof(g_settings_message),"Tap CONFIRM & RESTART to install. Settings and SD files are kept. Keep power connected.");
    return true;
  }
  pax_firmware_request_t request={0};request.magic=PAX_FIRMWARE_MAGIC;request.command=command;
  request.expected_build=g_firmware_confirm_build;
  void *f=g_svc->file_open ? g_svc->file_open(PAX_FIRMWARE_FILE,"wb") : NULL;
  bool ok=false;
  if(f){ok=g_svc->file_write(&request,1,sizeof(request),f)==sizeof(request);ok=g_svc->file_close(f)==0 && ok;}
  g_firmware_confirm=false;
  if(!ok)copy_text(g_settings_message,sizeof(g_settings_message),"Firmware request rejected. Check for updates first; older firmware needs a one-time upgrade.");
  refresh_firmware_status();return true;
}

static void refresh_wifi_settings(bool load)
{
  g_wifi_available=false;
  if(g_svc->file_open && g_svc->file_read && g_svc->file_close){
    void *f=g_svc->file_open(PAX_WIFI_SETTINGS_FILE,"rb");
    if(f){pax_wifi_status_t status={0};size_t n=g_svc->file_read(&status,1,sizeof(status),f);g_svc->file_close(f);
      if(n>=PAX_WIFI_STATUS_BASE_BYTES && status.magic==PAX_WIFI_SETTINGS_MAGIC && status.ssid[32]==0){status.security[sizeof(status.security)-1]=0;g_wifi_status=status;g_wifi_available=true;}
    }
  }
  if(load){copy_text(g_wifi_ssid,sizeof(g_wifi_ssid),g_wifi_available ? g_wifi_status.ssid : "");secure_clear(g_wifi_password,sizeof(g_wifi_password));g_wifi_password_changed=false;}
}
static void apply_wifi_settings(void)
{
  if(g_wifi_scan.state==1){copy_text(g_settings_message,sizeof(g_settings_message),"Wait for the Wi-Fi scan to finish");return;}
  if(g_wifi_needs_password && !g_wifi_password[0]){copy_text(g_settings_message,sizeof(g_settings_message),"Enter the selected network's Wi-Fi password first");return;}
  pax_wifi_request_t request={0};request.magic=PAX_WIFI_SETTINGS_MAGIC;request.replace_password=g_wifi_password_changed;
  copy_text(request.ssid,sizeof(request.ssid),g_wifi_ssid);copy_text(request.password,sizeof(request.password),g_wifi_password);
  if(!pax_wifi_request_valid(&request)){copy_text(g_settings_message,sizeof(g_settings_message),"SSID required; password must be 8-63 characters, 64 hex digits, or blank for open Wi-Fi");secure_clear(&request,sizeof(request));return;}
  int result=run_action(wifi_apply_job,&request,"Applying Wi-Fi... Please wait");
  bool accepted=result==0;secure_clear(&request,sizeof(request));
  if(accepted){secure_clear(g_wifi_password,sizeof(g_wifi_password));g_wifi_password_changed=false;g_wifi_needs_password=false;}
  copy_text(g_settings_message,sizeof(g_settings_message),accepted ? "Connecting; the previous Wi-Fi is restored only if it was connected" : "Wi-Fi request rejected; another connection may be in progress");
}

/* The scan endpoint is a bounded memory snapshot, never an SD read. Poll on
 * the entry task so Wi-Fi discovery cannot block or rebuild Settings widgets. */
static bool read_wifi_scan(void)
{
  if(!g_svc->file_open || !g_svc->file_read || !g_svc->file_close)return false;
  void *file=g_svc->file_open(PAX_WIFI_SCAN_FILE,"rb");if(!file)return false;
  pax_wifi_scan_status_t result={0};size_t n=g_svc->file_read(&result,1,sizeof(result),file);
  g_svc->file_close(file);
  if(n!=sizeof(result) || !pax_wifi_scan_valid(&result))return false;
  bool changed=!g_wifi_scan_available || result.generation!=g_wifi_scan.generation || result.state!=g_wifi_scan.state;
  g_wifi_scan=result;g_wifi_scan_available=true;return changed;
}
static int wifi_scan_start_job(void *unused)
{
  (void)unused;pax_wifi_scan_request_t request={PAX_WIFI_SCAN_MAGIC};
  void *file=g_svc->file_open(PAX_WIFI_SCAN_FILE,"wb");if(!file)return -1;
  bool ok=g_svc->file_write(&request,1,sizeof(request),file)==sizeof(request);
  return g_svc->file_close(file)==0 && ok ? 0 : -1;
}
static void request_wifi_scan(void)
{
  if(mutations_busy() || g_wifi_scan.state==1)return;
  if(!g_svc->file_open || !g_svc->file_write || !g_svc->file_close)return;
  g_wifi_scan_started_ms=(uint32_t)(g_svc->get_time_us()/1000U);
  if(run_action(wifi_scan_start_job,NULL,"Starting Wi-Fi scan...")==0){
    g_wifi_scan.state=1;g_wifi_scan.count=0;g_wifi_choice=0;
    g_wifi_scan_started_ms=(uint32_t)(g_svc->get_time_us()/1000U);
    copy_text(g_wifi_scan.message,sizeof(g_wifi_scan.message),"Scanning nearby networks...");
  }else {
    g_wifi_scan.state=-1;g_wifi_scan.count=0;
    copy_text(g_wifi_scan.message,sizeof(g_wifi_scan.message),"Scan unavailable; enter manually or update firmware");
  }
  copy_text(g_settings_message,sizeof(g_settings_message),g_wifi_scan.message);
  if(!g_discard_io)(void)g_svc->ui_call_sync(update_wifi_picker_on_host,NULL,1000);
}
static void poll_wifi_scan(void)
{
  if(!g_settings_mode)return;
  uint32_t now=(uint32_t)(g_svc->get_time_us()/1000U);
  if((uint32_t)(now-g_wifi_scan_poll_ms)<500U)return;
  g_wifi_scan_poll_ms=now;
  bool changed=read_wifi_scan();
  if(g_wifi_scan.state==1 && (uint32_t)(now-g_wifi_scan_started_ms)>25000U){
    g_wifi_scan.state=-1;copy_text(g_wifi_scan.message,sizeof(g_wifi_scan.message),"Scan timed out; retry or enter manually");changed=true;
  }
  if(changed && g_display){copy_text(g_settings_message,sizeof(g_settings_message),g_wifi_scan.message);(void)g_svc->ui_call_sync(update_wifi_picker_on_host,NULL,1000);}
}
static bool select_wifi_network(int index)
{
  if(g_wifi_scan.state!=2 || index<0 || (uint32_t)index>=g_wifi_scan.count)return false;
  const pax_wifi_scan_ap_t *ap=&g_wifi_scan.aps[index];
  bool current=g_wifi_available && !strcmp(ap->ssid,g_wifi_status.ssid);
  copy_text(g_wifi_ssid,sizeof(g_wifi_ssid),ap->ssid);
  g_wifi_needs_password=ap->secured && !current;
  if(!current || !ap->secured){secure_clear(g_wifi_password,sizeof(g_wifi_password));g_wifi_password_changed=true;}
  copy_text(g_settings_message,sizeof(g_settings_message),g_wifi_needs_password ?
    "Network selected; enter its password, then APPLY WI-FI" : "Network selected; choose APPLY WI-FI when ready");
  return true;
}

static void load_themes(void)
{
  g_theme_count=PAX_THEME_PRESETS;g_theme=pax_theme_presets[0];g_theme_index=0;
  memcpy(g_themes,pax_theme_presets,sizeof(pax_theme_presets));
  if(g_svc->file_mkdir)(void)g_svc->file_mkdir(PAX_THEME_FOLDER);
  if(g_svc->file_list_dir){
    int entries=g_svc->file_list_dir(PAX_THEME_FOLDER,g_listing,sizeof(g_listing));
    char *entry=g_listing;
    for(int i=0;i<entries && g_theme_count<MAX_THEMES;i++){
      size_t remaining=sizeof(g_listing)-(size_t)(entry-g_listing),n=0;
      while(n<remaining && entry[n])n++;if(n==remaining)break;
      char path[PATH_BYTES];copy_text(path,sizeof(path),PAX_THEME_FOLDER);append_text(path,sizeof(path),"/");append_text(path,sizeof(path),entry);
      if(n && !strchr(entry,'/') && !strchr(entry,'\\') && ends_with(entry,".json")){
        int length=read_json_file(path);pax_theme_profile_t candidate;
        if(length>0 && length<=2048 && pax_theme_parse(g_remote,&candidate)){
          bool duplicate=false;
          for(int j=0;j<g_theme_count;j++)if(strcmp(candidate.name,g_themes[j].name)==0 && memcmp(candidate.colors,g_themes[j].colors,sizeof(candidate.colors))==0){duplicate=true;break;}
          if(!duplicate)g_themes[g_theme_count++]=candidate;
        }
      }entry+=n+1;
    }
  }
  int length=read_json_file(PAX_THEME_FILE);
  if(length>0 && length<=2048)(void)pax_theme_parse(g_remote,&g_theme);
  bool found=false;
  for(int i=0;i<g_theme_count;i++)if(strcmp(g_themes[i].name,g_theme.name)==0 && memcmp(g_themes[i].colors,g_theme.colors,sizeof(g_theme.colors))==0){g_theme_index=i;found=true;break;}
  if(!found && g_theme_count<MAX_THEMES){g_theme_index=g_theme_count;g_themes[g_theme_count++]=g_theme;}
}
static bool save_theme(void)
{
  char json[768];if(!pax_theme_serialize(&g_theme,json,sizeof(json)) || !g_svc->file_open || !g_svc->file_write || !g_svc->file_close)return false;
  void *f=g_svc->file_open(PAX_THEME_FILE,"w");if(!f)return false;
  size_t n=strlen(json);bool saved=g_svc->file_write(json,1,n,f)==n;return g_svc->file_close(f)==0 && saved;
}

static void load_list_preferences(void)
{
  char value[12];g_hide_pax_extension=false;g_list_file_details=false;g_sort_mode=PAX_SORT_SOURCE;g_size_unit=PAX_SIZE_BYTES;
  if(read_small_file(LIST_PREF_FILE,value,sizeof(value))){
    g_hide_pax_extension=value[0]=='1';g_list_file_details=strlen(value)>1 && value[1]=='1';
    if(strlen(value)>3 && value[3]>='0' && value[3]<'0'+PAX_SIZE_COUNT)g_size_unit=value[3]-'0';
    if(strlen(value)>2 && value[2]>='0' && value[2]<'0'+PAX_SORT_COUNT)g_sort_mode=value[2]-'0';
  }
}
static bool save_list_preferences(void)
{
  if(!g_svc || !g_svc->file_open || !g_svc->file_write || !g_svc->file_close)return false;
  void *file=g_svc->file_open(LIST_PREF_FILE,"w");if(!file)return false;
  char value[5]={g_hide_pax_extension?'1':'0',g_list_file_details?'1':'0',(char)('0'+g_sort_mode),(char)('0'+g_size_unit),'\n'};
  bool saved=g_svc->file_write(value,1,sizeof(value),file)==sizeof(value);return g_svc->file_close(file)==0 && saved;
}

static void save_background_preference(void)
{
  if (g_svc == NULL || g_svc->file_open == NULL ||
      g_svc->file_write == NULL || g_svc->file_close == NULL) return;
  void *file = g_svc->file_open(BACKGROUND_PREF_FILE, "w");
  if (file == NULL) return;
  const char *path = (g_background_index >= 0 && g_background_index < g_background_count) ?
                     g_background_paths[g_background_index] : "none";
  (void)g_svc->file_write(path, 1, strlen(path), file);
  (void)g_svc->file_write("\n", 1, 1, file);
  (void)g_svc->file_close(file);
}

static void add_background_folder(const char *folder)
{
  if (g_background_count >= MAX_BACKGROUNDS || g_svc->file_list_dir == NULL) return;
  for (int i = 0; i < LIST_BYTES; i++) g_listing[i] = '\0';
  const int written = g_svc->file_list_dir(folder, g_listing, LIST_BYTES);
  if (written < 0 || g_discard_io) return;
  int offset = 0;
  while (offset < LIST_BYTES && g_background_count < MAX_BACKGROUNDS) {
    const char *name = g_listing + offset;
    size_t length = 0;
    while (offset + (int)length < LIST_BYTES && name[length] != '\0') length++;
    if (offset + (int)length >= LIST_BYTES) break;
    offset += (int)length + 1;
    if (length == 0 || name[length - 1] == '/' || !ends_with(name, ".png")) continue;
    char path[PATH_BYTES];
    copy_text(path, sizeof(path), folder);
    const size_t folder_length = strlen(path);
    if (folder_length > 0 && path[folder_length - 1] != '/' && name[0] != '/')
      append_text(path, sizeof(path), "/");
    append_text(path, sizeof(path), name);
    bool duplicate = false;
    for (int i = 0; i < g_background_count; i++) {
      size_t j = 0;
      while (g_background_paths[i][j] != '\0' && path[j] != '\0' &&
             g_background_paths[i][j] == path[j]) j++;
      if (g_background_paths[i][j] == '\0' && path[j] == '\0') duplicate = true;
    }
    if (!duplicate) copy_text(g_background_paths[g_background_count++], PATH_BYTES, path);
  }
}

static void load_backgrounds(void)
{
  g_background_count = 0;
  g_background_index = -1;
  for (int i = 0; i < MAX_BACKGROUNDS; i++) g_background_paths[i][0] = '\0';
  add_background_folder("/sd/backgrounds/");
  add_background_folder("/usb0/backgrounds/");
  char preferred[PATH_BYTES];
  bool preferred_none = false;
  if (read_small_file(BACKGROUND_PREF_FILE, preferred, sizeof(preferred))) {
    if (strcmp(preferred, "none") == 0) {
      g_background_index = -1;
      preferred_none = true;
    } else {
      for (int i = 0; i < g_background_count; i++) {
        size_t j = 0;
        while (g_background_paths[i][j] != '\0' && preferred[j] != '\0' &&
               g_background_paths[i][j] == preferred[j]) j++;
        if (g_background_paths[i][j] == '\0' && preferred[j] == '\0') {
          g_background_index = i;
          break;
        }
      }
    }
  }
  if (!preferred_none && g_background_index < 0 && g_background_count > 0)
    g_background_index = 0;
  g_background_choice = g_background_index + 1;
}

static void build_background_options(void)
{
  size_t used = 0;
  copy_text(g_background_options, sizeof(g_background_options), "None");
  used = strlen(g_background_options);
  for (int i = 0; i < g_background_count; i++) {
    const char *name = g_background_paths[i];
    for (const char *p = name; *p != '\0'; p++) if (*p == '/') name = p + 1;
    const size_t length = strlen(name);
    if (used + length + 2 > sizeof(g_background_options)) break;
    g_background_options[used++] = '\n';
    memcpy(g_background_options + used, name, length);
    used += length;
    g_background_options[used] = '\0';
  }
}

static void ipv4_to_text(uint32_t address, char *out, size_t capacity)
{
  if (out == NULL || capacity == 0) return;
  out[0] = '\0';
  append_uint(out, capacity, (address >> 24) & 0xffU); append_text(out, capacity, ".");
  append_uint(out, capacity, (address >> 16) & 0xffU); append_text(out, capacity, ".");
  append_uint(out, capacity, (address >> 8) & 0xffU); append_text(out, capacity, ".");
  append_uint(out, capacity, address & 0xffU);
}

static bool ipv4_from_text(const char *text, uint32_t *out)
{
  if (text == NULL || out == NULL) return false;
  uint32_t address = 0;
  const char *cursor = text;
  for (int octet = 0; octet < 4; octet++) {
    if (*cursor < '0' || *cursor > '9') return false;
    unsigned value = 0;
    int digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
      value = value * 10U + (unsigned)(*cursor++ - '0');
      if (value > 255U || ++digits > 3) return false;
    }
    address = (address << 8) | value;
    if (octet < 3) {
      if (*cursor++ != '.') return false;
    } else if (*cursor != '\0') {
      return false;
    }
  }
  *out = address;
  return true;
}

static int update_selection_on_host(void *arg);
typedef struct {
  pax_storage_status_t status[PAX_STORAGE_COUNT];
  int count[PAX_STORAGE_COUNT];
  bool inferred[PAX_STORAGE_COUNT],accessible[PAX_STORAGE_COUNT];
} StorageSnapshot;
static int storage_scan_job(void *argument)
{
  StorageSnapshot *job=argument;
  static char listing[LIST_BYTES]; /* private to the sole I/O worker */
  static const char *const app_dirs[PAX_STORAGE_COUNT] = {
    "/sd/roms/pax/", "/usb0/roms/pax/"
  };
  static const char *const root_dirs[PAX_STORAGE_COUNT] = {"/sd/", "/usb0/"};
  for (int storage = 0; storage < PAX_STORAGE_COUNT; storage++) {
    if(__atomic_load_n(&g_io_cancel,__ATOMIC_ACQUIRE))return -2;
    job->count[storage] = 0;
    job->inferred[storage] = false;
    job->accessible[storage] = false;
    const bool have_status_service = g_svc->storage_status_get != NULL &&
        g_svc->storage_status_get(storage, &job->status[storage]) == 0;
    if (!have_status_service) {
      memset(&job->status[storage], 0, sizeof(job->status[storage]));
      job->inferred[storage] = true;
    }
    if (g_svc->file_list_dir == NULL) continue;
    memset(listing, 0, sizeof(listing));
    int bytes = g_svc->file_list_dir(app_dirs[storage], listing, sizeof(listing));
    if (bytes < 0) {
      /* The mount may be live even when the status service is absent or
       * stale, and a freshly formatted volume may not have roms/pax yet.
       * Probe the root before reporting it as unmounted. */
      memset(listing, 0, sizeof(listing));
      bytes = g_svc->file_list_dir(root_dirs[storage], listing, sizeof(listing));
      if (bytes >= 0) {
        job->accessible[storage] = true;
        job->inferred[storage] = true;
        job->status[storage].available = 1;
        job->status[storage].mounted = 1;
        copy_text(job->status[storage].mount_path,
                  sizeof(job->status[storage].mount_path), root_dirs[storage]);
      }
      continue;
    }
    if (bytes < 0) continue;
    job->accessible[storage] = true;
    if (job->inferred[storage] || !job->status[storage].mounted) {
      job->inferred[storage] = true;
      job->status[storage].available = 1;
      job->status[storage].mounted = 1;
      copy_text(job->status[storage].mount_path,
                sizeof(job->status[storage].mount_path), root_dirs[storage]);
    }
    if (!job->status[storage].available || !job->status[storage].mounted) continue;
    int offset = 0;
    const int limit=sizeof(listing); /* file_list_dir returns an entry count. */
    while (offset < limit) {
      const char *name = listing + offset;
      size_t length = 0;
      while (offset + (int)length < limit && name[length] != '\0') length++;
      if (!length || offset + (int)length >= limit) break;
      offset += (int)length + 1;
      if (length >= 4 && name[length - 1] != '/' && ends_with(name, ".pax"))
        job->count[storage]++;
    }
  }
  return 0;
}

/* Keep the live lease separate from editable network values. A DHCP renewal
 * may update the page, but must never replace an address being typed/staged. */
static void refresh_network_live(bool load)
{
  if(!g_svc->network_config_get)return;
  pax_network_config_t current={0};
  if(g_svc->network_config_get(&current)!=0)return;
  g_network_live=current;
  bool editing_address=g_network_edit_field>=0 && g_network_edit_field<4;
  if(load || (!g_network_config_dirty && !editing_address))g_network_config=current;
  if(load)g_network_config_dirty=false;
}
static void poll_settings_connection(void)
{
  if(!g_settings_mode)return;
  uint32_t now=(uint32_t)(g_svc->get_time_us()/1000U);
  if((uint32_t)(now-g_connection_poll_ms)<1000U)return;
  g_connection_poll_ms=now;
  pax_network_config_t previous_live=g_network_live,previous_config=g_network_config;
  pax_wifi_status_t previous_wifi=g_wifi_status;bool previous_available=g_wifi_available;
  refresh_network_live(false);
  refresh_wifi_settings(false);
  refresh_c6_status();
  bool changed=memcmp(&previous_live,&g_network_live,sizeof(g_network_live)) ||
    memcmp(&previous_config,&g_network_config,sizeof(g_network_config)) ||
    memcmp(&previous_wifi,&g_wifi_status,sizeof(g_wifi_status)) || previous_available!=g_wifi_available;
  if(changed && !g_discard_io)(void)g_svc->ui_call_sync(update_selection_on_host,NULL,1000);
}

static void refresh_settings_snapshot(bool load_network_config)
{
  refresh_wifi_settings(load_network_config);
  refresh_network_live(load_network_config);
  StorageSnapshot snapshot={0};g_settings_scan_busy=true;
  (void)g_svc->ui_call_sync(update_selection_on_host,NULL,1000);
  work_status("Checking storage...");uint64_t start=timing_now();
  int result=io_run(storage_scan_job,&snapshot);
  timing_record(&g_scan_time,timing_now()-start);work_status(NULL);g_settings_scan_busy=false;
  if(result!=0)g_settings_scan_pending=true;
  if(result==0){
    memcpy(g_storage_status,snapshot.status,sizeof(g_storage_status));
    memcpy(g_storage_app_count,snapshot.count,sizeof(g_storage_app_count));
    memcpy(g_storage_status_inferred,snapshot.inferred,sizeof(g_storage_status_inferred));
    memcpy(g_storage_path_accessible,snapshot.accessible,sizeof(g_storage_path_accessible));
  }
  (void)g_svc->ui_call_sync(update_selection_on_host,NULL,1000);
}

static void refresh_pending_settings_scan(void)
{
  if(!g_settings_mode || !g_settings_scan_pending || g_settings_scan_busy ||
     mutations_busy() || g_discard_io || __atomic_load_n(&g_io_state,__ATOMIC_ACQUIRE)!=0)return;
  g_settings_scan_pending=false;
  refresh_settings_snapshot(false);
}

typedef struct {char path[PATH_BYTES];ImageResult image;} BackgroundJob;
static int background_job(void *argument)
{
  BackgroundJob *job=argument;
  job->image.pixels=g_svc->png_load_rgb565(job->path,&job->image.width,&job->image.height);
  return job->image.pixels && job->image.width && job->image.height ? 0 : -1;
}
/* Swap the image on the UI owner and invalidate caches before releasing the
 * previous allocation. A failed/cancelled job never touches the live image. */
static int publish_background_on_host(void *argument)
{
  BackgroundJob *job=argument;
  lv_image_cache_drop(&g_background_descriptor);
  lv_image_header_cache_drop(&g_background_descriptor);
  uint16_t *previous=g_background_pixels;
  g_background_pixels=job->image.pixels;g_background_width=job->image.width;g_background_height=job->image.height;
  g_background_descriptor=image_descriptor(&job->image);
  if(g_background_widget){
    if(g_background_pixels){
      lv_image_set_src(g_background_widget,&g_background_descriptor);
      lv_image_set_scale_x(g_background_widget,(uint32_t)(((uint64_t)g_canvas_w*256U)/g_background_width));
      lv_image_set_scale_y(g_background_widget,(uint32_t)(((uint64_t)g_canvas_h*256U)/g_background_height));
      lv_obj_center(g_background_widget);lv_obj_clear_flag(g_background_widget,LV_OBJ_FLAG_HIDDEN);
    }else lv_obj_add_flag(g_background_widget,LV_OBJ_FLAG_HIDDEN);
  }
  job->image=(ImageResult){0};
  if(previous)g_svc->mem_free(previous);
  lv_obj_invalidate(lv_display_get_screen_active(g_display));
  return 0;
}
static void load_background_image(void)
{
  static char loaded_path[PATH_BYTES];
  const char *wanted=g_background_count && g_background_index>=0 && g_background_index<g_background_count ? g_background_paths[g_background_index] : "";
  if(!g_display || !strcmp(wanted,loaded_path) || g_discard_io)return;
  BackgroundJob job={0};copy_text(job.path,sizeof(job.path),wanted);
  if(wanted[0]){
    if(!g_svc->png_load_rgb565)return;
    work_status("Loading background...");uint64_t start=timing_now();
    int result=io_run(background_job,&job);
    timing_record(&g_image_time,timing_now()-start);work_status(NULL);
    if(result!=0){image_release(&job.image);return;}
  }
  if(g_svc->ui_call_sync(publish_background_on_host,&job,2000)==0)copy_text(loaded_path,sizeof(loaded_path),wanted);
  image_release(&job.image);
}

static bool ends_with(const char *text, const char *suffix)
{
  size_t text_len = 0, suffix_len = 0;
  while (text != NULL && text[text_len] != '\0') text_len++;
  while (suffix != NULL && suffix[suffix_len] != '\0') suffix_len++;
  if (suffix_len > text_len) return false;
  for (size_t i = 0; i < suffix_len; i++) {
    char a = text[text_len - suffix_len + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return true;
}

static bool already_listed(const char *name)
{
  for (int i = 0; i < g_app_count; i++) {
    size_t j = 0;
    while (g_names[i][j] != '\0' && name[j] != '\0' && g_names[i][j] == name[j]) j++;
    if (g_names[i][j] == '\0' && name[j] == '\0') return true;
  }
  return false;
}

static void add_folder_entries(const char *folder)
{
  if (g_svc->file_list_dir == NULL || g_app_count >= MAX_APPS) return;
  /* Keep the directory buffer in the PAX data image.  The loader writes the
   * result from its ESPHome task; using a PAX-owned buffer keeps the pointer
   * valid across that service boundary on every ESP-IDF heap configuration. */
  char *listing = g_listing;
  for (int i = 0; i < LIST_BYTES; i++) listing[i] = '\0';
  int written = list_directory_async(folder, listing, LIST_BYTES);
  if (written < 0) return;
  g_svc->log_printf("launcher_ui dir '%s': entries=%d bytes=%u,%u,%u,%u\n",
                    folder, written, (unsigned)listing[0], (unsigned)listing[1],
                    (unsigned)listing[2], (unsigned)listing[3]);
  int offset = 0;
  while (offset < LIST_BYTES && g_app_count < MAX_APPS) {
    const char *name = listing + offset;
    size_t length = 0;
    while (offset + (int)length < LIST_BYTES && name[length] != '\0') length++;
    if (offset + (int)length >= LIST_BYTES) break;
    offset += (int)length + 1;
    if (length == 0 || name[length - 1] == '/' || !ends_with(name, ".pax")) continue;
    if (already_listed(name)) continue;
    const int slot = g_app_count;
    size_t name_bytes = length < NAME_BYTES - 1 ? length : NAME_BYTES - 1;
    for (size_t j = 0; j < name_bytes; j++) g_names[slot][j] = name[j];
    g_names[slot][name_bytes] = '\0';
    size_t used = 0;
    while (used + 1 < PATH_BYTES && folder[used] != '\0') {
      g_paths[slot][used] = folder[used];
      used++;
    }
    g_paths[slot][used] = '\0';
    if (used > 0 && g_paths[g_app_count][used - 1] != '/' && used + 1 < PATH_BYTES)
      g_paths[slot][used++] = '/';
    size_t path_bytes = length;
    if (path_bytes > PATH_BYTES - used - 1) path_bytes = PATH_BYTES - used - 1;
    for (size_t j = 0; j < path_bytes; j++) g_paths[slot][used + j] = name[j];
    g_paths[slot][used + path_bytes] = '\0';
    g_svc->log_printf("launcher_ui item %d: name='%s' path='%s'\n",
                      g_app_count, g_names[g_app_count], g_paths[g_app_count]);
    g_item_source[g_app_count] = SOURCE_STORAGE;
    g_app_count++;
  }
}

typedef struct {char path[PATH_BYTES],title[NAME_BYTES];pax_file_stat_t stat;uint32_t expires;bool local,used;} TitleCache;
static TitleCache g_title_cache[MAX_APPS];
static unsigned g_title_cache_next;
static bool g_title_attempted[MAX_APPS];
static uint32_t g_catalog_generation;

static void load_list_title(int index)
{
  if(g_title_attempted[index])return;
  g_title_attempted[index]=true;
  if(g_titles[index][0] || !ends_with(g_paths[index],".pax"))return;
  char sidecar[PATH_BYTES];if(!replace_package_suffix(g_paths[index],sidecar,sizeof(sidecar),".json"))return;
  const bool remote=has_prefix(sidecar,"http://") || has_prefix(sidecar,"https://");
  pax_file_stat_t info={0};
  const bool local_ok=!remote && g_svc->file_stat && stat_async(sidecar,&info)==0;
  if(g_discard_io)return;
  uint32_t now=(uint32_t)(g_svc->get_time_us()/1000U);
  for(int i=0;i<MAX_APPS;i++)if(g_title_cache[i].used && !strcmp(g_title_cache[i].path,sidecar)){
    TitleCache *cache=&g_title_cache[i];
    if((remote && (int32_t)(cache->expires-now)>0) ||
       (local_ok && cache->local && cache->stat.size==info.size && cache->stat.mtime==info.mtime && (int32_t)(cache->expires-now)>0)){
      copy_text(g_titles[index],NAME_BYTES,cache->title);return;
    }
    break;
  }
  int length=remote ? http_fetch(sidecar,g_remote,REMOTE_BYTES) : read_json_file(sidecar);
  if(g_discard_io)return;
  if(length>0 && (!json_string_field(g_remote,length,"title",g_titles[index],NAME_BYTES) || !pax_list_title_valid(g_titles[index])))g_titles[index][0]=0;
  if(length>0){
    TitleCache *cache=&g_title_cache[g_title_cache_next++%MAX_APPS];
    copy_text(cache->path,sizeof(cache->path),sidecar);copy_text(cache->title,sizeof(cache->title),g_titles[index]);
    cache->stat=info;cache->local=local_ok;cache->expires=now+30000;cache->used=true;
  }
}

/* Prefer visible rows, then finish the rest so title searches cover the
 * whole catalog without delaying the first usable list. */
static bool g_metadata_pending;
static uint32_t g_metadata_last_flush;
static bool load_visible_metadata(void)
{
  if(g_catalog_loading || g_settings_mode || g_detail_index>=0 || g_source==SOURCE_ARCHIVE)return false;
  int index=-1;
  for(int row=0;row<VISIBLE_ROWS;row++){
    int view=g_first_visible+row;if(view>=g_view_count)break;
    int candidate=g_visible_indices[view];
    if(!g_title_attempted[candidate]){index=candidate;break;}
  }
  if(index<0)for(int i=0;i<g_app_count;i++)if(!g_title_attempted[i]){index=i;break;}
  if(index<0)return false;
  if(g_item_source[index]==SOURCE_STORAGE && g_svc->file_stat)
    g_list_stats_valid[index]=stat_async(g_paths[index],&g_list_stats[index])==0 && !g_list_stats[index].is_dir;
  if(g_discard_io)return false;
  load_list_title(index);
  if(g_discard_io)return false;
  return true;
}

static void load_apps(void)
{
  g_metadata_pending=false;
  memset(g_list_stats_valid,0,sizeof(g_list_stats_valid));
  memset(g_title_attempted,0,sizeof(g_title_attempted));
  g_catalog_generation=__atomic_load_n(&g_navigation_generation,__ATOMIC_ACQUIRE);
  g_app_count = 0;
  g_catalog_failed = false;
  for (int i = 0; i < MAX_APPS; i++) {
    g_names[i][0] = '\0';
    g_titles[i][0] = '\0';
    g_paths[i][0] = '\0';
    g_archive_filenames[i][0] = '\0';
    g_archive_record_type[i] = 0;
  }
  if (g_favorites_mode) {
    load_favorites_as_catalog();
  } else if (g_source == SOURCE_STORAGE) {
    add_folder_entries("/sd/roms/pax/");
    add_folder_entries("/usb0/roms/pax/");
  } else if (g_source == SOURCE_ARCHIVE) {
    archive_load_current();
  } else {
    if(g_source==SOURCE_NETWORK)discover_supply_server();
    if(g_discard_io)return;
    const char *url = source_url(g_source);
    int length = url[0] ? http_fetch(url, g_remote, REMOTE_BYTES) : -1;
    if (length >= 0)
      g_app_count = parse_pax_links(g_remote, length, g_names, g_paths, url);
    else
      g_catalog_failed = true;
    g_svc->log_printf("launcher_ui %s catalog: bytes=%d apps=%d\n",
                      source_label(g_source), length, g_app_count);
    if (g_app_count == MAX_APPS)
      g_svc->log_printf("launcher_ui %s catalog reached the %d-entry safety cap\n",
                        source_label(g_source), MAX_APPS);
    for (int i = 0; i < g_app_count; i++) g_item_source[i] = g_source;
  }
  if(g_discard_io || g_catalog_generation!=__atomic_load_n(&g_navigation_generation,__ATOMIC_ACQUIRE)){g_app_count=0;g_view_count=0;return;}
  /* Publish filenames first. File stats and titles arrive progressively. */
  apply_filter();
}

static uint32_t tick_cb(void)
{
  return (uint32_t)(g_svc->get_time_us() / 1000);
}

static void disp_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
  if (g_flush_count < 3) {
    g_svc->log_printf("launcher_ui LVGL flush %d: %dx%d\n", g_flush_count + 1,
                      lv_area_get_width(area), lv_area_get_height(area));
    g_flush_count++;
  }
  /* LVGL supplies tightly packed dirty rectangles in PARTIAL mode. The
   * host retains the complete canvas for screenshots and presents small
   * updates without rotating/copying the whole native-resolution screen. */
  uint64_t flush_start=timing_now();
  g_svc->display_write_rect(area->x1, area->y1,
                           lv_area_get_width(area), lv_area_get_height(area),
                           (const uint16_t *)px_map);
  if (lv_display_flush_is_last(display)){
    g_svc->display_flush();
    if(g_action_pending_us){timing_record(&g_action_time,timing_now()-g_action_pending_us);g_action_pending_us=0;}
  }
  timing_record(&g_flush_time,timing_now()-flush_start);
  lv_display_flush_ready(display);
}

static bool g_syncing_search;
static unsigned g_ui_builds, g_row_updates, g_label_updates, g_button_updates;
static unsigned g_keyboard_transitions, g_metadata_flushes;
static lv_obj_t *g_footer_label;
static int g_keyboard_layout=-1, g_last_settings_focus=-1;
typedef struct {lv_obj_t *object;bool selected;} ButtonAppearance;
static ButtonAppearance g_button_appearance[128];
static unsigned g_button_appearance_count;

static void label_text_changed(lv_obj_t *label, const char *text)
{
  if(label && (!lv_label_get_text(label) || strcmp(lv_label_get_text(label),text)!=0)){
    lv_label_set_text(label,text);g_label_updates++;
  }
}
static void label_number_changed(lv_obj_t *label, int number, const char *suffix)
{
  char text[40];lv_snprintf(text,sizeof(text),"%d%s",number,suffix);
  label_text_changed(label,text);
}

static void style_button(lv_obj_t *button, bool selected)
{
  /* Reset with the display: object addresses must never survive deletion. */
  unsigned index=0;
  while(index<g_button_appearance_count && g_button_appearance[index].object!=button)index++;
  if(index<g_button_appearance_count && g_button_appearance[index].selected==selected)return;
  if(index<128){
    g_button_appearance[index]=(ButtonAppearance){button,selected};
    if(index==g_button_appearance_count)g_button_appearance_count++;
  }
  g_button_updates++;
  lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(selected ? COL_SELECTED : COL_PANEL), LV_PART_MAIN);
  lv_obj_set_style_border_color(button, lv_color_hex(selected ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
  lv_obj_set_style_border_width(button, selected ? 2 : 1, LV_PART_MAIN);
  /* Let a little of the selected launcher background show through the
   * controls without sacrificing the contrast of their labels and borders. */
  lv_obj_set_style_bg_opa(button, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(COL_SELECTED), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, lv_color_hex(COL_ACCENT), LV_PART_MAIN | LV_STATE_FOCUSED);
}

#include "tooltips.h"

static void rom_picker_button_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const intptr_t row = (intptr_t)lv_event_get_user_data(event);
  if (row < 0) {
    g_open_index = -61;
    return;
  }
  if (g_rom_prepare_busy) return;
  const int index = g_rom_picker_first + (int)row;
  if (index >= 0 && index < g_rom_picker_count) {
    g_rom_picker_selected = index;
    g_open_index = -60;
  }
}

static int update_rom_picker_on_host(void *unused)
{
  (void)unused;
  char host_names[ROM_PICKER_VISIBLE_ROWS][NAME_BYTES];
  for (int row = 0; row < ROM_PICKER_VISIBLE_ROWS; row++) {
    const int index = g_rom_picker_first + row;
    if (row < g_rom_picker_rows_shown && index >= 0 && index < g_rom_picker_count)
      copy_text(host_names[row], sizeof(host_names[row]), g_rom_picker_names[index]);
    else
      host_names[row][0] = '\0';
  }
  for (int row = 0; row < ROM_PICKER_VISIBLE_ROWS; row++) {
    if (g_rom_picker_rows[row] == NULL || g_rom_picker_row_labels[row] == NULL) continue;
    if (host_names[row][0] == '\0') {
      lv_obj_add_flag(g_rom_picker_rows[row], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(g_rom_picker_rows[row], LV_OBJ_FLAG_HIDDEN);
    if (g_rom_prepare_busy) lv_obj_add_state(g_rom_picker_rows[row], LV_STATE_DISABLED);
    else lv_obj_clear_state(g_rom_picker_rows[row], LV_STATE_DISABLED);
    style_button(g_rom_picker_rows[row], g_rom_picker_first + row == g_rom_picker_selected);
    lv_label_set_text(g_rom_picker_row_labels[row], host_names[row]);
  }
  if (g_rom_picker_status != NULL) {
    if (g_rom_prepare_busy) {
      const uint64_t elapsed = g_svc->get_time_us() - g_rom_prepare_started;
      static const char *const phases[] = {".", "..", "...", "...."};
      lv_label_set_text_fmt(g_rom_picker_status, "Preparing ROM%s  %us  |  B BACK (after preparation)",
                            phases[(elapsed / 250000U) % 4], (unsigned)(elapsed / 1000000U));
    } else if (g_install_status[0]) lv_label_set_text(g_rom_picker_status, g_install_status);
    else if (g_rom_picker_count == 0)
      lv_label_set_text_fmt(g_rom_picker_status, "No games found in roms/%s/ on SD or USB. Formats: %s ZIP",
                            g_rom_picker_profile->id, g_rom_picker_profile->extensions);
    else
      lv_label_set_text_fmt(g_rom_picker_status, "%d game%s  |  A SELECT  |  B BACK",
                            g_rom_picker_count, g_rom_picker_count == 1 ? "" : "s");
  }
  return 0;
}

static void rom_picker_add_folder(const char *folder, const char *storage_tag)
{
  if (g_svc == NULL || g_svc->file_list_dir == NULL ||
      g_rom_picker_count >= MAX_ROM_CHOICES) return;
  memset(g_listing, 0, sizeof(g_listing));
  const int entry_count = g_svc->file_list_dir(folder, g_listing, sizeof(g_listing));
  if (entry_count < 0) {
    if (g_svc->log_printf != NULL)
      g_svc->log_printf("launcher_ui: ROM folder unavailable: %s\n", folder);
    return;
  }
  const int count_before = g_rom_picker_count;
  int cursor = 0;
  for (int entry = 0; entry < entry_count && g_rom_picker_count < MAX_ROM_CHOICES; entry++) {
    if (cursor >= (int)sizeof(g_listing)) break;
    const char *name = g_listing + cursor;
    size_t length = 0;
    while (cursor + (int)length < (int)sizeof(g_listing) && name[length] != '\0') length++;
    if (cursor + (int)length >= (int)sizeof(g_listing)) break;
    cursor += (int)length + 1;
    if (length == 0 || name[length - 1] == '/' ||
        !pax_rom_profile_accepts(g_rom_picker_profile, name)) continue;

    if (strlen(folder) + length >= PATH_BYTES) continue;
    char path[PATH_BYTES];
    copy_text(path, sizeof(path), folder);
    append_text(path, sizeof(path), name);
    bool duplicate = false;
    for (int i = 0; i < g_rom_picker_count; i++)
      if (strcmp(g_rom_picker_paths[i], path) == 0) duplicate = true;
    if (duplicate) continue;
    const int index = g_rom_picker_count++;
    copy_text(g_rom_picker_paths[index], sizeof(g_rom_picker_paths[index]), path);
    copy_text(g_rom_picker_names[index], sizeof(g_rom_picker_names[index]), storage_tag);
    append_text(g_rom_picker_names[index], sizeof(g_rom_picker_names[index]), name);
  }
  if (g_svc->log_printf != NULL)
    g_svc->log_printf("launcher_ui: ROM folder %s entries=%d supported=%d\n",
                      folder, entry_count, g_rom_picker_count - count_before);
}

static void rom_picker_load_games(const char *app_path)
{
  g_rom_picker_count = 0;
  g_install_status[0] = '\0';
  g_rom_picker_selected = 0;
  g_rom_picker_first = 0;
  g_rom_picker_rows_shown = ROM_PICKER_VISIBLE_ROWS;
  g_rom_picker_rom_selected = false;
  g_rom_picker_profile = pax_rom_profile_for_app(app_path);
  if (g_rom_picker_profile == NULL) return;
  char folder[PATH_BYTES];
  copy_text(folder, sizeof(folder), "/sd/roms/");
  append_text(folder, sizeof(folder), g_rom_picker_profile->id);
  append_text(folder, sizeof(folder), "/");
  rom_picker_add_folder(folder, "SD  ");
  copy_text(folder, sizeof(folder), "/usb0/roms/");
  append_text(folder, sizeof(folder), g_rom_picker_profile->id);
  append_text(folder, sizeof(folder), "/");
  rom_picker_add_folder(folder, "USB ");
  /* NES Plus also uses the shared NES collection and its disk BIOS. */
  if (strcmp(g_rom_picker_profile->id, "nesplus") == 0) {
    rom_picker_add_folder("/sd/roms/nes/", "SD  ");
    rom_picker_add_folder("/usb0/roms/nes/", "USB ");
  }
}

/* Inputs and output belong to the waiting entry task. The worker never reads
 * mutable picker selection/profile state or publishes a ROM preference. */
typedef struct {
  char path[PATH_BYTES], extensions[128], stem[64], extracted[PATH_BYTES];
} RomPrepareJob;
static int rom_prepare_job(void *argument)
{
  RomPrepareJob *job = argument;
  return g_svc->file_zip_extract_first(job->path, job->extensions,
      "/sd/roms/pax/.zipcache", job->stem, job->extracted, sizeof(job->extracted));
}

static bool rom_picker_set_selected_game(void)
{
  if (mutations_busy() || g_rom_picker_selected < 0 || g_rom_picker_selected >= g_rom_picker_count)
    return false;
  char launch_path[PATH_BYTES];
  copy_text(launch_path, sizeof(launch_path), g_rom_picker_paths[g_rom_picker_selected]);
  if (extract_ends_with_ci(launch_path, ".zip") &&
      strstr(g_rom_picker_profile->extensions, ".zip|") == NULL) {
    if (g_svc->file_zip_extract_first == NULL) {
      copy_text(g_install_status, sizeof(g_install_status), "ZIP extraction is not available in this firmware.");
      return false;
    }
    uint32_t hash = 2166136261U;
    for (const unsigned char *p = (const unsigned char *)launch_path; *p != '\0'; p++)
      hash = (hash ^ *p) * 16777619U;
    static const char hex[] = "0123456789abcdef";
    char cache_stem[64];
    copy_text(cache_stem, sizeof(cache_stem), g_rom_picker_profile->id);
    append_text(cache_stem, sizeof(cache_stem), "_00000000");
    const size_t hash_start = strlen(cache_stem) - 8;
    for (int digit = 0; digit < 8; digit++)
      cache_stem[hash_start + digit] = hex[(hash >> (28 - digit * 4)) & 0x0fU];
    RomPrepareJob job = {0};
    copy_text(job.path, sizeof(job.path), launch_path);
    copy_text(job.extensions, sizeof(job.extensions), g_rom_picker_profile->extensions);
    copy_text(job.stem, sizeof(job.stem), cache_stem);
    g_rom_prepare_busy = true;
    g_rom_prepare_started = g_svc->get_time_us();
    (void)g_svc->ui_call_sync(update_rom_picker_on_host, NULL, 1000);
    const int result = run_action(rom_prepare_job, &job, "Preparing ROM... Please wait");
    g_rom_prepare_busy = false;
    /* Finish the cache write safely, then honor Back/navigation without
     * changing the selected ROM or launching the previously selected app. */
    if (g_discard_io) return false;
    if (result != 0 || !job.extracted[0]) {
      copy_text(g_install_status, sizeof(g_install_status), "No compatible ROM could be extracted from that ZIP.");
      (void)g_svc->ui_call_sync(update_rom_picker_on_host, NULL, 1000);
      return false;
    }
    copy_text(launch_path, sizeof(launch_path), job.extracted);
  }
  if (g_svc->settings_rom_path_set == NULL) {
    copy_text(g_install_status, sizeof(g_install_status), "This firmware cannot save the ROM selection.");
    return false;
  }
  g_svc->settings_rom_path_set(launch_path);
  if (g_svc->file_open != NULL && g_svc->file_write != NULL && g_svc->file_close != NULL) {
    char config_path[PATH_BYTES];
    copy_text(config_path, sizeof(config_path), "/sd/roms/pax/");
    append_text(config_path, sizeof(config_path), g_rom_picker_profile->id);
    append_text(config_path, sizeof(config_path), ".rom");
    void *sidecar = g_svc->file_open(config_path, "wb");
    if (sidecar != NULL) {
      const size_t length = strlen(launch_path);
      (void)g_svc->file_write(launch_path, 1, length, sidecar);
      (void)g_svc->file_write("\n", 1, 1, sidecar);
      (void)g_svc->file_close(sidecar);
    }
  }
  g_rom_picker_rom_selected = true;
  g_svc->log_printf("launcher_ui: selected ROM %s\n", launch_path);
  return true;
}

static int build_rom_picker_overlay(lv_obj_t *screen)
{
  lv_obj_t *shade = lv_obj_create(screen);
  lv_obj_remove_style_all(shade);
  lv_obj_set_size(shade, g_canvas_w, g_canvas_h);
  lv_obj_set_pos(shade, 0, 0);
  lv_obj_set_style_bg_color(shade, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(shade, LV_OPA_70, LV_PART_MAIN);
  lv_obj_clear_flag(shade, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  const int margin = ui_px(12);
  const int panel_w = g_canvas_w - 2 * margin;
  const int panel_h = g_canvas_h - 2 * margin;
  lv_obj_t *panel = lv_obj_create(screen);
  lv_obj_set_size(panel, panel_w, panel_h);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, lv_color_hex(COL_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(panel, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel, ui_px(10), LV_PART_MAIN);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text_fmt(title, "SELECT GAME: %s", g_rom_picker_profile->title);
  lv_obj_set_style_text_color(title, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, ui_font(), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, ui_px(5));

  const int button_h = ui_px(18);
  const int gap = ui_px(2);
  const int status_h = ui_px(12);
  const int cancel_h = ui_px(22);
  const int list_y = ui_px(28);
  const int reserved = list_y + status_h + cancel_h + ui_px(10);
  const int available = panel_h - reserved;
  g_rom_picker_rows_shown = available / (button_h + gap);
  if (g_rom_picker_rows_shown > ROM_PICKER_VISIBLE_ROWS)
    g_rom_picker_rows_shown = ROM_PICKER_VISIBLE_ROWS;
  if (g_rom_picker_rows_shown < 1) g_rom_picker_rows_shown = 1;
  if (g_rom_picker_first + g_rom_picker_rows_shown > g_rom_picker_count)
    g_rom_picker_first = g_rom_picker_count > g_rom_picker_rows_shown ?
                         g_rom_picker_count - g_rom_picker_rows_shown : 0;

  for (int row = 0; row < ROM_PICKER_VISIBLE_ROWS; row++) {
    g_rom_picker_rows[row] = lv_button_create(panel);
    lv_obj_set_size(g_rom_picker_rows[row], panel_w - ui_px(24), button_h);
    lv_obj_set_pos(g_rom_picker_rows[row], ui_px(2), list_y + row * (button_h + gap));
    lv_obj_set_style_radius(g_rom_picker_rows[row], 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(g_rom_picker_rows[row], ui_px(8), LV_PART_MAIN);
    style_button(g_rom_picker_rows[row], false);
    lv_obj_add_event_cb(g_rom_picker_rows[row], rom_picker_button_event, LV_EVENT_CLICKED,
                        (void *)(intptr_t)row);
    g_rom_picker_row_labels[row] = lv_label_create(g_rom_picker_rows[row]);
    lv_label_set_long_mode(g_rom_picker_row_labels[row], LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_rom_picker_row_labels[row], panel_w - ui_px(44));
    lv_obj_set_style_text_color(g_rom_picker_row_labels[row], lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_rom_picker_row_labels[row], &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_rom_picker_row_labels[row], LV_ALIGN_LEFT_MID, 0, 0);
  }

  g_rom_picker_status = lv_label_create(panel);
  lv_obj_set_style_text_color(g_rom_picker_status, lv_color_hex(COL_MUTED), LV_PART_MAIN);
  lv_obj_set_style_text_font(g_rom_picker_status, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(g_rom_picker_status, LV_ALIGN_BOTTOM_MID, 0, -cancel_h - ui_px(8));
  lv_obj_t *cancel = lv_button_create(panel);
  lv_obj_set_size(cancel, ui_px(100), cancel_h);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -ui_px(2));
  style_button(cancel, false);
  lv_obj_add_event_cb(cancel, rom_picker_button_event, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
  lv_obj_t *cancel_label = lv_label_create(cancel);
  lv_label_set_text(cancel_label, "BACK");
  lv_obj_set_style_text_color(cancel_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_center(cancel_label);
  (void)update_rom_picker_on_host(NULL);
  lv_obj_move_foreground(panel);
  return 0;
}

static void search_value_event(lv_event_t *event)
{
  if (g_syncing_search || lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || g_search_area == NULL) return;
  copy_text(g_search_text, sizeof(g_search_text), lv_textarea_get_text(g_search_area));
  if (g_settings_mode && (g_network_edit_field >= 0 || g_archive_auth_edit_field >= 0)) return;
  if (g_source == SOURCE_ARCHIVE && g_archive_level == ARCHIVE_LEVEL_SEARCH) {
    g_archive_search_pending = true;
    g_archive_search_changed_ms = g_svc != NULL && g_svc->get_time_us != NULL ?
        (uint32_t)(g_svc->get_time_us() / 1000U) : 0;
  }
  mark_ui_action();g_filter_dirty = true;
}

static void search_click_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  /* The entry task shows the page's existing keyboard after the click and
   * gives controller focus to its first key. */
  g_keyboard_focus = 0;
  g_toolbar_focus = 7;
  play_ui_sound(PAX_UI_SOUND_CONFIRM, 0);
  mark_ui_action();g_open_index = -8;
}

static void keyboard_close_event(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  if ((code != LV_EVENT_READY && code != LV_EVENT_CANCEL) || g_keyboard == NULL) return;
  g_keyboard_focus = (int)lv_buttonmatrix_get_selected_button(g_keyboard);
  g_keyboard_open = false;
  mark_ui_action();g_open_index = -3;
}

static void keyboard_value_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || g_keyboard == NULL) return;
  g_keyboard_focus = (int)lv_buttonmatrix_get_selected_button(g_keyboard);
  play_ui_sound(PAX_UI_SOUND_CLICK, 0);
}

static int keyboard_focus_on_host(void *unused)
{
  (void)unused;
  if (g_keyboard != NULL) {
    lv_buttonmatrix_set_selected_button(g_keyboard, (uint32_t)g_keyboard_focus);
    lv_obj_add_state(g_keyboard, LV_STATE_FOCUSED);
  }
  return 0;
}

static int keyboard_activate_on_host(void *unused)
{
  (void)unused;
  if (g_keyboard == NULL || g_search_area == NULL) return -1;
  if (g_keyboard_focus < 0) g_keyboard_focus = 0;
  lv_buttonmatrix_set_selected_button(g_keyboard, (uint32_t)g_keyboard_focus);
  const char *key = lv_keyboard_get_button_text(g_keyboard, (uint32_t)g_keyboard_focus);
  if (key == NULL) return -1;

  /* LVGL's default keyboard callback is designed for pointer-release events.
   * A gamepad A press has no pointer event, so dispatch its selected key
   * action directly while already on the LVGL host thread. */
  if (strcmp(key, "1#") == 0) {
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_SPECIAL);
    g_keyboard_focus = 0;
  } else if (strcmp(key, "ABC") == 0) {
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
    g_keyboard_focus = 0;
  } else if (strcmp(key, "abc") == 0) {
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    g_keyboard_focus = 0;
  } else if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
    lv_textarea_delete_char(g_search_area);
  } else if (strcmp(key, LV_SYMBOL_LEFT) == 0) {
    lv_textarea_cursor_left(g_search_area);
  } else if (strcmp(key, LV_SYMBOL_RIGHT) == 0) {
    lv_textarea_cursor_right(g_search_area);
  } else if (strcmp(key, LV_SYMBOL_NEW_LINE) == 0 || strcmp(key, LV_SYMBOL_OK) == 0) {
    lv_obj_send_event(g_keyboard, LV_EVENT_READY, NULL);
    lv_obj_send_event(g_search_area, LV_EVENT_READY, NULL);
  } else if (strcmp(key, LV_SYMBOL_KEYBOARD) == 0 || strcmp(key, LV_SYMBOL_CLOSE) == 0) {
    lv_obj_send_event(g_keyboard, LV_EVENT_CANCEL, NULL);
    lv_obj_send_event(g_search_area, LV_EVENT_CANCEL, NULL);
  } else {
    lv_textarea_add_text(g_search_area, key);
  }
  play_ui_sound(PAX_UI_SOUND_CLICK, 0);
  return 0;
}

static void move_keyboard_focus(int dx, int dy)
{
  /* LVGL 9.5's lower/upper text keyboard has four rows with these button
   * counts. Keeping directional movement row-aware makes the native widget
   * usable from a controller rather than only from touch. */
  static const int row_start[] = {0, 12, 23, 35};
  static const int row_count[] = {12, 11, 12, 5};
  int row = 0;
  for (int i = 0; i < 4; i++) {
    if (g_keyboard_focus >= row_start[i]) row = i;
  }
  int col = g_keyboard_focus - row_start[row];
  int target_row = row + dy;
  if (target_row < 0) target_row = 0;
  if (target_row > 3) target_row = 3;
  if (dy != 0 && target_row != row) {
    col = row_count[row] > 1 ?
        (col * (row_count[target_row] - 1) + (row_count[row] - 1) / 2) /
            (row_count[row] - 1) : 0;
  }
  if (dx != 0) {
    col += dx;
    if (col < 0) col = 0;
    if (col >= row_count[target_row]) col = row_count[target_row] - 1;
  }
  g_keyboard_focus = row_start[target_row] + col;
}

static int usb_text_on_host(void *unused)
{
  (void)unused;
  if (!g_pending_key_valid) return 0;
  if (g_search_area == NULL ||
      (g_settings_mode && g_network_edit_field < 0 && g_archive_auth_edit_field < 0) ||
      g_detail_index >= 0) {
    g_pending_key_valid = false;
    return 0;
  }
  g_toolbar_focus = 7;
  lv_obj_add_state(g_search_area, LV_STATE_FOCUSED);
  lv_textarea_set_cursor_pos(g_search_area, LV_TEXTAREA_CURSOR_LAST);
  if (g_pending_key == 8 || g_pending_key == 127)
    lv_textarea_delete_char(g_search_area);
  else if (g_pending_key >= 32 && g_pending_key <= 126)
    lv_textarea_add_char(g_search_area, (uint32_t)g_pending_key);
  lv_textarea_set_cursor_pos(g_search_area, LV_TEXTAREA_CURSOR_LAST);
  g_pending_key_valid = false;
  return 0;
}

static void settings_slider_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *slider = lv_event_get_target(event);
  int value = 0;
  if (slider == g_volume_slider) {
    g_volume_level = lv_slider_get_value(slider);
    value = g_volume_level;
    if (g_volume_value != NULL) lv_label_set_text_fmt(g_volume_value, "%d%%", g_volume_level);
  } else if (slider == g_brightness_slider) {
    g_brightness_level = lv_slider_get_value(slider);
    value = g_brightness_level;
    if (g_brightness_value != NULL) lv_label_set_text_fmt(g_brightness_value, "%d%%", g_brightness_level);
  }
  play_ui_sound(PAX_UI_SOUND_VOLUME, (uint8_t)value);
  g_settings_dirty = true;
}

static uint32_t c6_progress_percent(const pax_c6_status_t *status)
{
  uint64_t percent=status->total ? (uint64_t)status->done*100U/status->total : 0;
  return percent>100 ? 100 : (uint32_t)percent;
}

/* Refresh independently of focus changes so a long verification keeps
 * updating the bar without rebuilding or closing the Settings page. */
static int update_c6_progress_on_host(void *unused)
{
  (void)unused;
  if(!g_settings_mode || !g_c6_progress)return 0;
  refresh_c6_status();
  for(int i=0;i<3;i++)if(g_settings_focus_widgets[31+i]){
    bool enabled=!mutations_busy() && g_c6_available && g_c6_status.state!=PAX_C6_BUSY && g_c6_status.state!=PAX_C6_REBOOTING;
    if(i==2)enabled=enabled && g_c6_status.state==PAX_C6_READY && strcmp(g_c6_source,g_c6_checked_source)==0;
    if(enabled)lv_obj_clear_state(g_settings_focus_widgets[31+i],LV_STATE_DISABLED);else lv_obj_add_state(g_settings_focus_widgets[31+i],LV_STATE_DISABLED);
  }
  if(g_c6_labels[0])lv_label_set_text(g_c6_labels[0],g_c6_source);
  if(g_c6_labels[1])lv_label_set_text(g_c6_labels[1],g_c6_available && g_c6_status.state==PAX_C6_BUSY ? "WORKING..." : "CHECK FIRMWARE");
  if(g_c6_progress){
    uint32_t percent=c6_progress_percent(&g_c6_status);
    lv_bar_set_value(g_c6_progress,(int)percent,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_c6_progress,lv_color_hex(g_c6_status.state==PAX_C6_ERROR ? COL_ERROR : COL_ACCENT),LV_PART_INDICATOR);
  }
  if(g_c6_labels[3]){
    char text[220];copy_text(text,sizeof(text),g_c6_available ? g_c6_status.message : "Requires P4 ESP-Hosted firmware");
    append_text(text,sizeof(text)," | ");
    if(g_c6_status.total){append_uint(text,sizeof(text),c6_progress_percent(&g_c6_status));append_text(text,sizeof(text),"% | ");}
    append_uint(text,sizeof(text),g_c6_status.done);append_text(text,sizeof(text)," / ");append_uint(text,sizeof(text),g_c6_status.total);append_text(text,sizeof(text)," bytes");
    lv_label_set_text(g_c6_labels[3],text);
  }
  if(g_c6_available && g_c6_status.state==PAX_C6_BUSY && g_c6_last_state!=PAX_C6_BUSY && g_c6_labels[3])
    lv_obj_scroll_to_view(g_c6_labels[3],LV_ANIM_OFF);
  g_c6_last_state=g_c6_status.state;
  return 0;
}

static int update_firmware_on_host(void *unused)
{
  (void)unused;
  if(!g_settings_mode || !g_firmware_labels[2])return 0;
  refresh_firmware_status();
  bool idle=g_firmware_available && g_firmware_status.profile[0] &&
    g_firmware_status.state!=PAX_FIRMWARE_CHECKING &&
    !(g_firmware_status.state>=PAX_FIRMWARE_WAITING && g_firmware_status.state<=PAX_FIRMWARE_REBOOTING);
  for(int i=0;i<2;i++)if(g_settings_focus_widgets[34+i]){
    bool enabled=idle && !mutations_busy() && (i==0 || g_firmware_status.state==PAX_FIRMWARE_AVAILABLE);
    if(enabled)lv_obj_clear_state(g_settings_focus_widgets[34+i],LV_STATE_DISABLED);
    else lv_obj_add_state(g_settings_focus_widgets[34+i],LV_STATE_DISABLED);
  }
  if(g_firmware_labels[0])lv_label_set_text(g_firmware_labels[0],g_firmware_status.state==PAX_FIRMWARE_CHECKING ? "CHECKING..." : "CHECK FOR UPDATES");
  if(g_firmware_labels[1])lv_label_set_text(g_firmware_labels[1],g_firmware_confirm ? "CONFIRM & RESTART" : "INSTALL UPDATE");
  char text[512];
  if(g_firmware_available){
    copy_text(text,sizeof(text),g_firmware_status.profile);append_text(text,sizeof(text)," | Installed version: ");
    for(int i=0;i<2;i++){
      uint64_t build=i ? g_firmware_status.latest_build : g_firmware_status.installed_build;
      if(i){if(!build)break;append_text(text,sizeof(text),"\nLatest published version: ");}
      if(!build)append_text(text,sizeof(text),"local build (unversioned)");
      else {
        append_text(text,sizeof(text),"build ");
        char digits[21];size_t count=0;
        do{digits[count++]=(char)('0'+build%10);build/=10;}while(build && count<20);
        while(count){char digit[2]={digits[--count],0};append_text(text,sizeof(text),digit);}
      }
    }
    append_text(text,sizeof(text),"\n");
    if(g_firmware_status.state==PAX_FIRMWARE_CURRENT)append_text(text,sizeof(text),
      g_firmware_status.installed_build==g_firmware_status.latest_build ? "You are on the latest version. " : "No newer version available. ");
    append_text(text,sizeof(text),g_firmware_confirm ?
      "Tap CONFIRM & RESTART to install. Settings and SD files are kept. Keep power connected." : g_firmware_status.message);
  }else copy_text(text,sizeof(text),"Requires a one-time P4 firmware upgrade to enable GitHub updates");
  lv_label_set_text(g_firmware_labels[2],text);
  if(g_firmware_status.state!=g_firmware_last_state &&
     (g_firmware_status.state==PAX_FIRMWARE_CHECKING || g_firmware_last_state==PAX_FIRMWARE_CHECKING))
    lv_obj_scroll_to_view(g_firmware_labels[2],LV_ANIM_OFF);
  g_firmware_last_state=g_firmware_status.state;
  return 0;
}

static void update_clock_label(void)
{
  if (g_status_clock == NULL) return;
  const char *text = g_device_status.local_time[0] != '\0' ?
                       g_device_status.local_time : "--:--";
  char compact[48];
  if (g_show_date) {
    char date[64];
    text = "No date";
    if (g_svc->device_date_get != NULL) {
      if (g_svc->device_date_get(date, sizeof(date)) == 0) {
        /* Firmware returns weekday\nDD Month YYYY. Keep the full weekday
         * and year, abbreviating only the month for a single-line date. */
        const char *day = strchr(date, '\n');
        if (day != NULL && strlen(day + 1) >= 6) {
          const char *year = strchr(day + 4, ' ');
          if (year != NULL && year > day + 6) {
            size_t prefix = (size_t)(day - date);
            if (prefix > 15) prefix = 15;
            for (size_t i = 0; i < prefix; ++i) compact[i] = date[i];
            compact[prefix++] = ' ';
            compact[prefix++] = day[1]; compact[prefix++] = day[2]; compact[prefix++] = ' ';
            compact[prefix++] = day[4]; compact[prefix++] = day[5]; compact[prefix++] = day[6];
            compact[prefix++] = ' '; compact[prefix] = '\0';
            append_text(compact, sizeof(compact), year + 1);
            text = compact;
          }
        }
      } else text = "Syncing...";
    }
  }
  lv_obj_set_style_text_font(g_status_clock, g_show_date ?
                             &lv_font_montserrat_14 : &lv_font_montserrat_20, LV_PART_MAIN);
  lv_label_set_text(g_status_clock, text);
}

static void clock_tap_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  g_show_date = !g_show_date;
  update_clock_label();
}

static int update_device_status_on_host(void *unused)
{
  (void)unused;
  if (g_status_clock == NULL || g_status_network_icon == NULL ||
      g_status_battery_icon == NULL || g_status_battery_level == NULL)
    return 0;
  memset(&g_device_status, 0, sizeof(g_device_status));
  g_device_status.battery_percent = -1;
  if (g_svc->device_status_get != NULL)
    (void)g_svc->device_status_get(&g_device_status);
  update_clock_label();
  const uint32_t network_icon = g_device_status.network_type == PAX_NETWORK_ETHERNET ? 0xf796U :
                                (g_device_status.network_type == PAX_NETWORK_WIFI ? 0xf1ebU : 0xf071U);
  set_fa_glyph(g_status_network_icon, network_icon);
  if (g_device_status.battery_percent >= 0) {
    const int percent = g_device_status.battery_percent;
    const uint32_t battery_icon = percent >= 75 ? 0xf240U :
                                  (percent >= 50 ? 0xf241U :
                                   (percent >= 25 ? 0xf242U :
                                    (percent >= 10 ? 0xf243U : 0xf244U)));
    set_fa_glyph(g_status_battery_icon, battery_icon);
    lv_label_set_text_fmt(g_status_battery_level, "%d%%", percent);
  } else {
    set_fa_glyph(g_status_battery_icon, 0xf240U);
    lv_label_set_text(g_status_battery_level, "--");
  }
  return 0;
}

static void toggle_auto_brightness(void)
{
  g_auto_brightness_enabled = !g_auto_brightness_enabled;
  if (g_svc->settings_auto_brightness_set != NULL)
    g_svc->settings_auto_brightness_set(g_auto_brightness_enabled ? 1 : 0);
}

static void set_auto_brightness(bool enabled)
{
  if (g_auto_brightness_enabled == enabled) return;
  g_auto_brightness_enabled = enabled;
  if (g_svc->settings_auto_brightness_set != NULL)
    g_svc->settings_auto_brightness_set(enabled ? 1 : 0);
}

static void auto_brightness_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  toggle_auto_brightness();
  if (g_auto_brightness_state_label != NULL)
    lv_label_set_text(g_auto_brightness_state_label, g_auto_brightness_enabled ? "ON" : "OFF");
  play_ui_sound(PAX_UI_SOUND_CONFIRM, 0);
}

static void select_source(int source)
{
  if (source < 0 || source >= SOURCE_COUNT) return;
  if (source != g_source || g_favorites_mode)
    play_ui_sound(PAX_UI_SOUND_CHIME, 0);
  /* Cancel a deferred Archive search before another source can own the list. */
  g_archive_search_pending = false;
  __atomic_add_fetch(&g_navigation_generation,1,__ATOMIC_ACQ_REL);
  mark_ui_action();
  g_source = source;
  g_catalog_stale=true;
  g_favorites_mode = false;
  g_settings_mode = false;
  g_toolbar_focus = -1;
  g_selected = 0;
  if (source == SOURCE_ARCHIVE) {
    g_archive_level = ARCHIVE_LEVEL_SEARCH;
    g_archive_identifier[0] = '\0';
    g_archive_zip_url[0] = '\0';
    g_search_text[0] = '\0';
    g_archive_search_pending = false;
  }
  g_open_index = -3;
}

static void source_button_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  int source = (int)(intptr_t)lv_event_get_user_data(event);
  if (source < 0 || source >= SOURCE_COUNT) return;
  if (g_detail_index >= 0 && g_svc != NULL && g_svc->log_printf != NULL)
    g_svc->log_printf("launcher_ui detail toolbar click: source=%d active=%d\n",
                      source, g_source);
  g_toolbar_focus = -1;
  if (source == g_source && !g_favorites_mode) {
    /* Choosing the already-active source from an app's details page is still
     * an explicit request to return to that source's list. Without this,
     * controller A remains armed on the detail page's Play action. */
    if (g_detail_index >= 0 || g_settings_mode ||
        __atomic_load_n(&g_io_state,__ATOMIC_ACQUIRE)==1) {
      select_source(source);
      return;
    }
    if (source == SOURCE_ARCHIVE)
      g_open_index = g_archive_level != ARCHIVE_LEVEL_SEARCH ? -15 : -3;
    return;
  }
  select_source(source);
}

static void toolbar_button_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  g_toolbar_focus = -1;
  const int action = (int)(intptr_t)lv_event_get_user_data(event);
  if(mutations_busy() && (action==-35 || action==-36 || action==-37 || action==-65 || action==-71 || action==-72))return;
  play_ui_sound(action == -9 || action == -2 ? PAX_UI_SOUND_BACK : PAX_UI_SOUND_CONFIRM, 0);
  g_open_index = action;mark_ui_action();
}

static int toolbar_focus_step(int current, int direction)
{
  if (current < SOURCE_COUNT + 2) {
    int next = (current + direction + SOURCE_COUNT + 2) % (SOURCE_COUNT + 2);
    return next;
  }

  /* The second toolbar row follows its visual left-to-right order. Archive
   * inserts the parent-directory control between Favorites and Search. */
  static const int archive_order[] = {6, 11, 7, 8, 9, 10};
  static const int standard_order[] = {6, 7, 8, 9, 10};
  const int *order = g_source == SOURCE_ARCHIVE ? archive_order : standard_order;
  const int count = g_source == SOURCE_ARCHIVE ?
                    (int)(sizeof(archive_order) / sizeof(archive_order[0])) :
                    (int)(sizeof(standard_order) / sizeof(standard_order[0]));
  int position = 0;
  while (position < count && order[position] != current) position++;
  if (position == count) position = 0;
  position = (position + direction + count) % count;
  return order[position];
}

static void background_dropdown_event(lv_event_t *event)
{
  if (g_background_dropdown == NULL) return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    g_settings_focus = 3;
    g_background_dropdown_open = lv_dropdown_is_open(g_background_dropdown);
    g_background_choice = (int)lv_dropdown_get_selected(g_background_dropdown);
    return;
  }
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
  const uint32_t selected = lv_dropdown_get_selected(g_background_dropdown);
  if (selected > (uint32_t)g_background_count) return;
  g_background_index = (int)selected - 1;
  g_background_choice = (int)selected;
  g_background_dropdown_open = false;
  play_ui_sound(PAX_UI_SOUND_CONFIRM, 0);
  g_open_index = -10;
}

static int background_dropdown_choice_on_host(void *arg)
{
  const int choice = (int)(intptr_t)arg;
  if (g_background_dropdown == NULL || choice < 0 || choice > g_background_count) return -1;
  lv_dropdown_set_selected(g_background_dropdown, (uint32_t)choice);
  return 0;
}

static int wifi_choice_action(void)
{
  return g_wifi_choice==0 ? -80 : g_wifi_choice==1 ? -81 : -1000-(g_wifi_choice-2);
}
static void wifi_dropdown_event(lv_event_t *event)
{
  if(!g_wifi_dropdown)return;
  if(lv_event_get_code(event)==LV_EVENT_CANCEL){g_wifi_dropdown_open=false;return;}
  if(lv_event_get_code(event)==LV_EVENT_CLICKED || lv_event_get_code(event)==LV_EVENT_READY){
    g_settings_focus=36;g_wifi_dropdown_open=lv_dropdown_is_open(g_wifi_dropdown);
    g_wifi_choice=(int)lv_dropdown_get_selected(g_wifi_dropdown);
    if(g_wifi_dropdown_open && g_wifi_scan.state==0)g_open_index=-80;
  }else if(lv_event_get_code(event)==LV_EVENT_VALUE_CHANGED){
    g_wifi_choice=(int)lv_dropdown_get_selected(g_wifi_dropdown);g_wifi_dropdown_open=false;
    g_open_index=wifi_choice_action();
  }
}
static int wifi_dropdown_state_on_host(void *unused)
{
  (void)unused;if(!g_wifi_dropdown)return 0;
  lv_dropdown_set_selected(g_wifi_dropdown,(uint32_t)g_wifi_choice);
  if(g_wifi_dropdown_open){
    if(g_wifi_scan_message_label)lv_obj_scroll_to_view(g_wifi_scan_message_label,LV_ANIM_OFF);
    lv_dropdown_open(g_wifi_dropdown);
  }else lv_dropdown_close(g_wifi_dropdown);
  return 0;
}
static int update_wifi_picker_on_host(void *unused)
{
  (void)unused;if(!g_wifi_dropdown)return 0;
  copy_text(g_wifi_options,sizeof(g_wifi_options),g_wifi_scan.state==1 ? "Scanning...\nEnter manually" : "Scan again\nEnter manually");
  if(g_wifi_scan.state==2)for(uint32_t i=0;i<g_wifi_scan.count;i++){
    char name[33],line[96];copy_text(name,sizeof(name),g_wifi_scan.aps[i].ssid);
    for(char *p=name;*p;p++)if((unsigned char)*p<32 || *p==127)*p='?';
    lv_snprintf(line,sizeof(line),"\n%s  %ld dBm  %s",name,(long)g_wifi_scan.aps[i].rssi,g_wifi_scan.aps[i].secured ? "Secured" : "Open");
    append_text(g_wifi_options,sizeof(g_wifi_options),line);
  }
  lv_dropdown_set_options(g_wifi_dropdown,g_wifi_options);
  if(g_wifi_choice<0 || (uint32_t)g_wifi_choice>=g_wifi_scan.count+2)g_wifi_choice=0;
  lv_dropdown_set_selected(g_wifi_dropdown,(uint32_t)g_wifi_choice);
  lv_dropdown_set_text(g_wifi_dropdown,g_wifi_scan.state==1 ? "SCANNING..." : g_wifi_scan.state==2 ? "CHOOSE NETWORK" : "SCAN NEARBY");
  if(g_wifi_scan_message_label)lv_label_set_text(g_wifi_scan_message_label,g_wifi_scan.message[0] ? g_wifi_scan.message : "Scan nearby, or enter a network name manually");
  if(g_settings_message_label)lv_label_set_text(g_settings_message_label,g_settings_message);
  if(g_wifi_dropdown_open && !g_keyboard_open){
    if(g_wifi_scan_message_label)lv_obj_scroll_to_view(g_wifi_scan_message_label,LV_ANIM_OFF);
    lv_dropdown_open(g_wifi_dropdown);
  }
  return 0;
}

static void settings_focus_event(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_CLICKED) return;
  const int focus = (int)(intptr_t)lv_event_get_user_data(event);
  if (focus >= 0 && focus < SETTINGS_FOCUS_COUNT) g_settings_focus = focus;
}

static void update_archive_s3_credential_display(void)
{
  const char *access_text = g_archive_s3_access[0] == '\0' ?
      "Select to enter access key" :
      (g_archive_s3_access_revealed ? g_archive_s3_access : "********");
  const char *secret_text = g_archive_s3_secret[0] == '\0' ?
      "Select to enter secret key" :
      (g_archive_s3_secret_revealed ? g_archive_s3_secret : "********");
  if (g_archive_s3_access_value_label != NULL) {
    lv_label_set_text(g_archive_s3_access_value_label, access_text);
    lv_label_set_long_mode(g_archive_s3_access_value_label,
        g_archive_s3_access_revealed ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
  }
  if (g_archive_s3_secret_value_label != NULL) {
    lv_label_set_text(g_archive_s3_secret_value_label, secret_text);
    lv_label_set_long_mode(g_archive_s3_secret_value_label,
        g_archive_s3_secret_revealed ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
  }
  for (int field = 0; field < 2; field++) {
    lv_obj_t *slash = g_archive_s3_eye_slashes[field];
    if (slash == NULL) continue;
    const bool revealed = field == 0 ? g_archive_s3_access_revealed :
                                      g_archive_s3_secret_revealed;
    if (revealed) lv_obj_add_flag(slash, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(slash, LV_OBJ_FLAG_HIDDEN);
  }
}

static void archive_s3_visibility_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const int field = (int)(intptr_t)lv_event_get_user_data(event);
  if (field == 0) {
    g_archive_s3_access_revealed = !g_archive_s3_access_revealed;
    g_settings_focus = 8;
  } else if (field == 1) {
    g_archive_s3_secret_revealed = !g_archive_s3_secret_revealed;
    g_settings_focus = 9;
  } else {
    return;
  }
  play_ui_sound(PAX_UI_SOUND_CLICK, 0);
  update_archive_s3_credential_display();
}

static lv_obj_t *create_archive_s3_visibility_button(lv_obj_t *parent, int x, int y,
                                                     int field)
{
  lv_obj_t *button = lv_button_create(parent);
  const int button_size = ui_px(18);
  lv_obj_set_size(button, button_size, button_size);
  lv_obj_set_pos(button, x, y + (ui_px(22) - button_size) / 2);
  lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
  style_button(button, false);
  /* Keep this adjacent visibility control's click local so it toggles the
   * value instead of opening the key editor. */
  lv_obj_clear_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(button, archive_s3_visibility_event, LV_EVENT_CLICKED,
                      (void *)(intptr_t)field);

  static const lv_point_precise_t eye_points[] = {
    {0, 5}, {5, 0}, {10, 5}, {5, 10}, {0, 5}
  };
  static const lv_point_precise_t slash_points[] = {{1, 10}, {10, 1}};
  lv_obj_t *outline = lv_line_create(button);
  lv_line_set_points(outline, eye_points,
                     (uint32_t)(sizeof(eye_points) / sizeof(eye_points[0])));
  lv_obj_set_style_line_color(outline, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_line_width(outline, 1, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(outline, true, LV_PART_MAIN);
  lv_obj_center(outline);
  lv_obj_t *pupil = lv_obj_create(button);
  lv_obj_remove_style_all(pupil);
  lv_obj_set_size(pupil, ui_px(3), ui_px(3));
  lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(pupil, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(pupil);
  lv_obj_t *slash = lv_line_create(button);
  lv_line_set_points(slash, slash_points,
                     (uint32_t)(sizeof(slash_points) / sizeof(slash_points[0])));
  lv_obj_set_style_line_color(slash, lv_color_hex(COL_WARN), LV_PART_MAIN);
  lv_obj_set_style_line_width(slash, 2, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(slash, true, LV_PART_MAIN);
  lv_obj_center(slash);

  g_archive_s3_eye_buttons[field] = button;
  g_archive_s3_eye_slashes[field] = slash;
  return button;
}

static void app_details_switch_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
  g_app_details_enabled = g_app_details_switch != NULL &&
                          lv_obj_has_state(g_app_details_switch, LV_STATE_CHECKED);
  g_settings_focus = 20;
  g_open_index = -43;
}

static void list_switch_event(lv_event_t *event)
{
  if(lv_event_get_code(event)!=LV_EVENT_VALUE_CHANGED)return;
  int option=(int)(intptr_t)lv_event_get_user_data(event);
  bool enabled=g_list_switches[option] && lv_obj_has_state(g_list_switches[option],LV_STATE_CHECKED);
  if(option==0)g_hide_pax_extension=enabled;else g_list_file_details=enabled;
  g_settings_focus=22+option;g_open_index=-48;
}

static void begin_network_edit(int field)
{
  if (field < 0 || field > 7) return;
  copy_text(g_search_before_settings_edit, sizeof(g_search_before_settings_edit), g_search_text);
  uint32_t value = field == 0 ? g_network_config.ipv4 :
                   field == 1 ? g_network_config.netmask :
                   field == 2 ? g_network_config.gateway : g_network_config.dns;
  if(field<4)ipv4_to_text(value,g_search_text,sizeof(g_search_text));
  else copy_text(g_search_text,sizeof(g_search_text),field==4 ? (g_network_auto ? "auto" : g_network_url) : field==5 ? g_wifi_ssid : field==6 ? g_wifi_password : g_c6_source);
  g_network_edit_field = field;
  g_keyboard_focus = 0;
  g_keyboard_open = true;
  g_toolbar_focus = 7;
}

static bool commit_network_edit(void)
{
  if (g_network_edit_field < 0 || g_network_edit_field > 7) return false;
  uint32_t address = 0;
  const int field = g_network_edit_field;
  bool valid=false;
  if(field==4){
    if(strlen(g_search_text)==4 && has_prefix_ascii_case_insensitive(g_search_text,"auto")){valid=save_network_mode(true);if(valid)g_catalog_stale=true;}
    else {char normalized[PATH_BYTES];valid=normalize_network_url(g_search_text,normalized,sizeof(normalized)) && save_network_url(normalized) && save_network_mode(false);if(valid)g_catalog_stale=true;}
    copy_text(g_settings_message,sizeof(g_settings_message),valid ? (g_network_auto ? "Automatic PAX Supply discovery enabled; open LAN" : "Manual PAX Supply URL saved on SD") : "Enter auto for discovery or an HTTP(S) URL; SD is required to save");
  }
  else if(field==5){g_wifi_needs_password=false;valid=strlen(g_search_text)>0 && strlen(g_search_text)<sizeof(g_wifi_ssid);if(valid)copy_text(g_wifi_ssid,sizeof(g_wifi_ssid),g_search_text);copy_text(g_settings_message,sizeof(g_settings_message),valid ? "Wi-Fi name staged; choose APPLY WI-FI" : "Wi-Fi name must be 1-32 bytes");}
  else if(field==7){valid=strlen(g_search_text)>0 && strlen(g_search_text)<sizeof(g_c6_source);if(valid)copy_text(g_c6_source,sizeof(g_c6_source),g_search_text);copy_text(g_settings_message,sizeof(g_settings_message),valid ? "C6 source staged; only the board-approved image hash is accepted" : "Firmware path or URL required");}
  else if(field==6){valid=strlen(g_search_text)<sizeof(g_wifi_password);if(valid){copy_text(g_wifi_password,sizeof(g_wifi_password),g_search_text);g_wifi_password_changed=true;}copy_text(g_settings_message,sizeof(g_settings_message),valid ? "Password staged; choose APPLY WI-FI (blank means open network)" : "Password is too long");}
  else valid=ipv4_from_text(g_search_text,&address);
  if (field<4 && valid) {
    g_network_config_dirty=true;
    if (field == 0) g_network_config.ipv4 = address;
    else if (field == 1) g_network_config.netmask = address;
    else if (field == 2) g_network_config.gateway = address;
    else g_network_config.dns = address;
    copy_text(g_settings_message, sizeof(g_settings_message), "Address staged; select APPLY NETWORK to save");
  } else if(field<4) {
    copy_text(g_settings_message, sizeof(g_settings_message), "Invalid IPv4 address; expected n.n.n.n");
  }
  secure_clear(g_search_text,sizeof(g_search_text));
  copy_text(g_search_text, sizeof(g_search_text), g_search_before_settings_edit);
  secure_clear(g_search_before_settings_edit,sizeof(g_search_before_settings_edit));
  g_network_edit_field = -1;
  g_toolbar_focus = -1;
  return valid;
}

static const char *network_type_name(int type)
{
  if (type == PAX_NETWORK_ETHERNET) return "Ethernet";
  if (type == PAX_NETWORK_WIFI) return "Wi-Fi";
  return "Offline";
}

static void append_storage_size(char *out, size_t capacity, uint64_t bytes)
{
  const char *unit = "B";
  uint64_t divisor = 1;
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    unit = "GB";
    divisor = 1024ULL * 1024ULL * 1024ULL;
  } else if (bytes >= 1024ULL * 1024ULL) {
    unit = "MB";
    divisor = 1024ULL * 1024ULL;
  } else if (bytes >= 1024ULL) {
    unit = "KB";
    divisor = 1024ULL;
  }
  append_uint(out, capacity, (uint32_t)(bytes / divisor));
  if (divisor > 1) {
    append_text(out, capacity, ".");
    append_uint(out, capacity, (uint32_t)(((bytes % divisor) * 10ULL) / divisor));
  }
  append_text(out, capacity, " ");
  append_text(out, capacity, unit);
}

static void format_storage_status(int storage, char *out, size_t capacity)
{
  if(g_settings_scan_busy || g_settings_scan_pending){copy_text(out,capacity,"Checking storage...");return;}
  if (storage < 0 || storage >= PAX_STORAGE_COUNT) {
    copy_text(out, capacity, storage == PAX_STORAGE_SD ? "Not mounted" : "Not connected");
    return;
  }
  if (g_storage_status_inferred[storage] && g_storage_path_accessible[storage]) {
    copy_text(out, capacity, "Mounted | ");
    append_uint(out, capacity, (uint32_t)g_storage_app_count[storage]);
    append_text(out, capacity, " apps | capacity unavailable");
    return;
  }
  if (!g_storage_status[storage].available || !g_storage_status[storage].mounted) {
    if (g_storage_path_accessible[storage]) {
      copy_text(out, capacity, "Path accessible | ");
      append_uint(out, capacity, (uint32_t)g_storage_app_count[storage]);
      append_text(out, capacity, " apps | capacity unavailable");
    } else if (g_storage_status[storage].available) {
      copy_text(out, capacity, "Connected | Not mounted");
    } else {
      copy_text(out, capacity, storage == PAX_STORAGE_SD ? "Not mounted" : "Not connected");
    }
    return;
  }
  const uint64_t total = g_storage_status[storage].total_bytes;
  const uint64_t free_bytes = g_storage_status[storage].free_bytes;
  if (total == 0) {
    copy_text(out, capacity, "Mounted | ");
    append_uint(out, capacity, (uint32_t)g_storage_app_count[storage]);
    append_text(out, capacity, " apps | capacity unavailable");
    return;
  }
  const uint64_t used_bytes = free_bytes < total ? total - free_bytes : 0;
  out[0] = '\0';
  copy_text(out, capacity, "Mounted | ");
  append_uint(out, capacity, (uint32_t)g_storage_app_count[storage]);
  append_text(out, capacity, " apps | ");
  append_storage_size(out, capacity, total);
  append_text(out, capacity, " total | ");
  append_storage_size(out, capacity, used_bytes);
  append_text(out, capacity, " used | ");
  append_storage_size(out, capacity, free_bytes);
  append_text(out, capacity, " free");
}

static void app_button_event(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED && g_list_touch_swiped) {
    /* A drag over a row must scroll it, never launch the row on release. */
    g_list_touch_swiped = false;
    return;
  }
  const int row = (int)(intptr_t)lv_event_get_user_data(event);
  const int view_index = g_first_visible + row;
  if (view_index < 0 || view_index >= g_view_count) return;
  const int index = g_visible_indices[view_index];
  g_selected = view_index;
  for (int visible_row = 0; visible_row < VISIBLE_ROWS; visible_row++) {
    if (g_buttons[visible_row] != NULL)
      style_button(g_buttons[visible_row], visible_row == row);
  }
  if (g_toolbar_buttons[6] != NULL) {
    const bool allowed = favorite_allowed(index);
    if (allowed) lv_obj_clear_state(g_toolbar_buttons[6], LV_STATE_DISABLED);
    else lv_obj_add_state(g_toolbar_buttons[6], LV_STATE_DISABLED);
    style_button(g_toolbar_buttons[6], allowed && path_is_favorite(g_paths[index]));
  }
  if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    play_ui_sound(PAX_UI_SOUND_CONFIRM, 0);
  if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    g_open_index = g_item_source[index] == SOURCE_ARCHIVE ? -14 :
                   (g_app_details_enabled ? -18 : index);
}

static void detail_button_event(lv_event_t *event)
{
  const int action = (int)(intptr_t)lv_event_get_user_data(event);
  if (action < 0 || action >= g_detail_action_count) return;
  if(mutations_busy() && action<g_detail_action_count-1)return;
  if (g_svc != NULL && g_svc->log_printf != NULL)
    g_svc->log_printf("launcher_ui detail action click: action=%d\n", action);
  g_detail_focus = action;
  for (int i = 0; i < g_detail_action_count; i++) {
    if (g_detail_buttons[i] != NULL)
      style_button(g_detail_buttons[i], i == action);
  }
  if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    play_ui_sound(PAX_UI_SOUND_CONFIRM, 0);
  if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    g_open_index = action;mark_ui_action();
}

/* Settings are rendered with storage first so card status and mount controls
 * are immediately visible. Preserve the existing IDs used by callbacks and
 * expose navigation in that same visual order. */
static int settings_focus_step(int focus, int delta)
{
  static const uint8_t order[SETTINGS_FOCUS_COUNT] = {
    18, 19, 0, 1, 2, 3, 12, 13, 14, 15, 16, 25, 26, 36, 27, 28, 17,
    4, 5, 6, 7, 8, 9, 10, 11, 20, 37, 22, 23, 30, 29, 24, 31, 32, 33, 34, 35, 21
  };
  int position = 0;
  for (int i = 0; i < SETTINGS_FOCUS_COUNT; i++) {
    if (order[i] == focus) {
      position = i;
      break;
    }
  }
  int next = (position + delta) % SETTINGS_FOCUS_COUNT;
  if (next < 0) next += SETTINGS_FOCUS_COUNT;
  return order[next];
}

static void detail_info_scroll_event(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_SCROLL_BEGIN && code != LV_EVENT_SCROLL_END) return;
  lv_obj_t *view = lv_event_get_target(event);
  if (g_svc != NULL && g_svc->log_printf != NULL)
    g_svc->log_printf("launcher_ui detail info scroll %s: offset=%ld remaining=%ld\n",
                      code == LV_EVENT_SCROLL_BEGIN ? "begin" : "end",
                      (long)lv_obj_get_scroll_y(view), (long)lv_obj_get_scroll_bottom(view));
}

/* Trigger scrolling for long app descriptions. Positive/negative values
 * represent a small page count and this runs on LVGL's host task. */
static int detail_scroll_on_host(void *arg)
{
  const int pages = (int)(intptr_t)arg;
  const int direction = pages < 0 ? -1 : pages > 0 ? 1 : 0;
  lv_obj_t *view = g_detail_info_view;
  if (view == NULL || direction == 0) return 0;

  lv_obj_update_layout(view);
  const int scroll_y = lv_obj_get_scroll_y(view);
  const int scroll_bottom = lv_obj_get_scroll_bottom(view);
  if ((direction < 0 && scroll_y <= 0) ||
      (direction > 0 && scroll_bottom <= 0))
    return 0;

  /* LVGL uses a negative dy to reveal content farther down the document. */
  int page_count = pages < 0 ? -pages : pages;
  if (page_count > 5) page_count = 5;
  lv_obj_scroll_by_bounded(view, 0, -direction * ui_px(42) * page_count, LV_ANIM_OFF);
  if (g_svc != NULL && g_svc->log_printf != NULL)
    g_svc->log_printf("launcher_ui detail info trigger scroll: direction=%s pages=%d y=%ld remaining=%ld\n",
                      direction < 0 ? "up" : "down",
                      page_count,
                      (long)lv_obj_get_scroll_y(view),
                      (long)lv_obj_get_scroll_bottom(view));
  return 1;
}

static void poll_install_status(void)
{
  if (g_svc == NULL || g_svc->store_install_status == NULL) return;
  pax_install_progress_t progress;
  if (g_svc->store_install_status(&progress) != 0) return;
  if(progress.state==0){g_transfer_busy=false;return;}
  g_transfer_busy=progress.state==1;
  copy_text(g_transfer_message,sizeof(g_transfer_message),progress.status);
  work_status(g_transfer_message);
  if(!g_transfer_busy){g_catalog_stale=true;g_settings_scan_pending=true;}
  if(g_detail_index<0 || !g_detail_is_remote || strcmp(g_detail_name,g_transfer_name)!=0)return;
  g_install_state = progress.state;
  g_install_done = progress.done;
  g_install_total = progress.total;
  copy_text(g_install_status, sizeof(g_install_status), progress.status);
  if(progress.state==2)g_transfer_verify_pending=true;
}

static void poll_archive_download(void)
{
  if (!g_archive_download_active || g_svc == NULL || g_svc->store_install_status == NULL) return;
  pax_install_progress_t progress;
  if (g_svc->store_install_status(&progress) != 0) return;
  g_install_done = progress.done;
  g_install_total = progress.total;
  if (progress.state == 1) {
    char status[128];
    copy_text(status, sizeof(status), "Downloading ");
    append_uint(status, sizeof(status), progress.done / 1024U);
    append_text(status, sizeof(status), " KB");
    if (progress.total > 0) {
      append_text(status, sizeof(status), " / ");
      append_uint(status, sizeof(status), progress.total / 1024U);
      append_text(status, sizeof(status), " KB");
    }
    copy_text(g_archive_status, sizeof(g_archive_status), status);
  } else if (progress.state == 2) {
    copy_text(g_archive_status, sizeof(g_archive_status), "Download complete - file synced to SD card");
    g_archive_download_active = false;
  } else if (progress.state == 3) {
    copy_text(g_archive_status, sizeof(g_archive_status), progress.status[0] != '\0' ?
              progress.status : "Download failed; file was not committed");
    g_archive_download_active = false;
  }
}

static void save_launcher_resume_state(void)
{
  if (g_svc == NULL || g_svc->app_set_resume_arg == NULL) return;
  char state[48];
  state[0] = '\0';
  copy_text(state, sizeof(state), "lu4:");
  append_uint(state, sizeof(state), (uint32_t)g_source);
  append_text(state, sizeof(state), ":");
  append_uint(state, sizeof(state), (uint32_t)g_selected);
  append_text(state, sizeof(state), ":");
  /* Zero means resume to the list; record index + 1 reopens its information
   * page after a child PAX (such as disc_installer) returns. */
  append_uint(state, sizeof(state), g_detail_index >= 0 ?
              (uint32_t)g_detail_index + 1U : 0U);
  append_text(state, sizeof(state), ":");
  append_uint(state, sizeof(state), g_favorites_mode ? 1U : 0U);
  (void)g_svc->app_set_resume_arg(state);
}

static int parse_state_number(const char **cursor)
{
  int value = 0;
  bool any = false;
  while (**cursor >= '0' && **cursor <= '9') {
    any = true;
    value = value * 10 + (**cursor - '0');
    if (value > 1000000) return -1;
    (*cursor)++;
  }
  return any ? value : -1;
}

static void restore_launcher_resume_state(void)
{
  if (g_svc == NULL || g_svc->app_get_arg == NULL) return;
  char state[48];
  if (g_svc->app_get_arg(state, sizeof(state)) <= 0) return;
  const bool version4 = has_prefix(state, "lu4:");
  const bool version3 = version4 || has_prefix(state, "lu3:");
  if (!version3 && !has_prefix(state, "lu2:")) return;
  const char *cursor = state + 4;
  const int source = parse_state_number(&cursor);
  if (source < 0 || *cursor++ != ':') return;
  const int selected = parse_state_number(&cursor);
  if (selected < 0 || source >= SOURCE_COUNT || selected >= MAX_APPS) return;
  int detail_plus_one = 0;
  if (version3) {
    if (*cursor++ != ':') return;
    detail_plus_one = parse_state_number(&cursor);
    if (detail_plus_one < 0 || detail_plus_one > MAX_APPS) return;
  }
  int favorites = 0;
  if (version4) {
    if (*cursor++ != ':') return;
    favorites = parse_state_number(&cursor);
    if (favorites < 0 || favorites > 1) return;
  }
  if (*cursor != '\0') return;
  g_source = source;
  g_selected = selected;
  g_resume_detail_record = detail_plus_one > 0 ? detail_plus_one - 1 : -1;
  g_favorites_mode = favorites != 0;
}

typedef struct {
  bool valid, details;
  int width, unit;
  char name[LIST_LABEL_BYTES], info[120];
} RowAppearance;
static RowAppearance g_row_appearance[VISIBLE_ROWS];
static void update_list_row_text(int row, int index, const char *name)
{
  lv_obj_t *label=g_labels[row], *details=g_list_detail_labels[row];
  char text[120]="";
  const bool show=g_list_file_details && index>=0;
  if(show)pax_list_details_units(text,sizeof(text),g_list_stats_valid[index] ? &g_list_stats[index] : NULL,g_size_unit);
  const int width=lv_obj_get_content_width(g_buttons[row]);
  RowAppearance *cache=&g_row_appearance[row];
  if(cache->valid && cache->width==width && cache->details==show && cache->unit==g_size_unit &&
     !strcmp(cache->name,name) && !strcmp(cache->info,text))return;
  g_row_updates++;
  cache->valid=true;cache->width=width;cache->details=show;cache->unit=g_size_unit;
  copy_text(cache->name,sizeof(cache->name),name);copy_text(cache->info,sizeof(cache->info),text);
  label_text_changed(label,name);
  const int inset=ui_px(8);
  int detail_width=0;
  if(show){
    const lv_font_t *font=g_ui_scale>=1.5f ? &lv_font_montserrat_20 : &lv_font_montserrat_12;
    lv_obj_set_style_text_font(details,font,LV_PART_MAIN);
    label_text_changed(details,text);
    lv_point_t size;lv_text_get_size(&size,text,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    detail_width=size.x;if(detail_width>width*3/5)detail_width=width*3/5;
    lv_obj_set_width(details,detail_width);lv_obj_clear_flag(details,LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(details,LV_ALIGN_RIGHT_MID,-inset,0);
  }else lv_obj_add_flag(details,LV_OBJ_FLAG_HIDDEN);
  int name_width=width-2*inset-(detail_width ? detail_width+inset : 0);
  lv_obj_set_width(label,name_width>0 ? name_width : 1);
  lv_obj_align(label,LV_ALIGN_LEFT_MID,inset,0);
}

static void append_wifi_summary(char *out, size_t capacity, const pax_wifi_status_t *status)
{
  if(status->ssid[0]){append_text(out,capacity," | ");append_text(out,capacity,status->ssid);}
  if(status->frequency_mhz){
    append_text(out,capacity,status->frequency_mhz<3000 ? " | 2.4 GHz (" : " | 5 GHz (");
    append_uint(out,capacity,status->frequency_mhz);append_text(out,capacity," MHz, ch ");
    append_uint(out,capacity,status->channel);append_text(out,capacity,")");
  }
  if(status->security[0]){append_text(out,capacity," | ");append_text(out,capacity,status->security);}
}

/* The list recycles five row objects, so its scrollbar follows the logical
 * viewport rather than LVGL's physical child bounds. */
static int list_scrollbar_thumb_height(void)
{
  if (g_view_count <= VISIBLE_ROWS) return g_list_scrollbar_height;
  int height = g_list_scrollbar_height * VISIBLE_ROWS / g_view_count;
  if (height < ui_px(22)) height = ui_px(22);
  if (height > g_list_scrollbar_height) height = g_list_scrollbar_height;
  return height;
}

static void update_list_scrollbar(void)
{
  if (g_list_scrollbar == NULL || g_list_scrollbar_thumb == NULL) return;
  /* Filtering can change overflow without rebuilding the row widgets. */
  if (g_list != NULL){
    const int pad=lv_obj_get_style_pad_left(g_list,LV_PART_MAIN)+(g_view_count>VISIBLE_ROWS ? ui_px(16) : 0);
    if(lv_obj_get_style_pad_right(g_list,LV_PART_MAIN)!=pad)lv_obj_set_style_pad_right(g_list,pad,LV_PART_MAIN);
  }
  if (g_view_count <= VISIBLE_ROWS) {
    lv_obj_add_flag(g_list_scrollbar, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(g_list_scrollbar, LV_OBJ_FLAG_HIDDEN);
  const int height = list_scrollbar_thumb_height();
  const int travel = g_list_scrollbar_height - height;
  const int max_first = g_view_count - VISIBLE_ROWS;
  lv_obj_set_height(g_list_scrollbar_thumb, height);
  lv_obj_set_y(g_list_scrollbar_thumb, travel * g_first_visible / max_first);
}

static void sync_search_context(void)
{
  if(!g_search_area)return;
  g_syncing_search=true;
  char search_placeholder[96];
  if (g_source == SOURCE_ARCHIVE && g_archive_level == ARCHIVE_LEVEL_SEARCH) {
    copy_text(search_placeholder, sizeof(search_placeholder), "Search collection: ");
    append_text(search_placeholder, sizeof(search_placeholder), g_archive_collection);
  } else {
    copy_text(search_placeholder, sizeof(search_placeholder), "Search apps");
  }
  if (g_settings_mode && g_archive_auth_edit_field == 0)
    copy_text(search_placeholder, sizeof(search_placeholder), "Archive.org email");
  else if (g_settings_mode && g_archive_auth_edit_field == 1)
    copy_text(search_placeholder, sizeof(search_placeholder), "Archive.org password");
  else if (g_settings_mode && g_archive_auth_edit_field == 2)
    copy_text(search_placeholder, sizeof(search_placeholder), "Archive S3 access key");
  else if (g_settings_mode && g_archive_auth_edit_field == 3)
    copy_text(search_placeholder, sizeof(search_placeholder), "Archive S3 secret key");
  if(g_settings_mode && g_network_edit_field>=4)copy_text(search_placeholder,sizeof(search_placeholder),g_network_edit_field==4 ? "auto or http://computer-ip:8000/" : g_network_edit_field==5 ? "Wi-Fi network name" : g_network_edit_field==6 ? "New Wi-Fi password; blank for open" : "C6 .bin: /sd/firmware/... or https://...");
  lv_textarea_set_max_length(g_search_area,g_network_edit_field==5 ? 32 : g_network_edit_field==6 ? 64 : sizeof(g_search_text)-1);
  if(!lv_textarea_get_placeholder_text(g_search_area) || strcmp(lv_textarea_get_placeholder_text(g_search_area),search_placeholder))lv_textarea_set_placeholder_text(g_search_area, search_placeholder);
  if(strcmp(lv_textarea_get_text(g_search_area),g_search_text))lv_textarea_set_text(g_search_area, g_search_text);
  bool secret=g_settings_mode && (g_archive_auth_edit_field==1 || g_archive_auth_edit_field==3 || g_network_edit_field==6);
  if(lv_textarea_get_password_mode(g_search_area)!=secret)lv_textarea_set_password_mode(g_search_area,secret);
  g_syncing_search=false;
 }
static void sync_keyboard_layout(void);

static int update_selection_on_host(void *arg)
{
  (void)arg;
  sync_search_context();
  sync_keyboard_layout();
  tooltip_focus_current();
  if(g_tooltips_value)label_text_changed(g_tooltips_value,g_tooltips_enabled ? "ON" : "OFF");
  if(g_footer_label && !g_settings_mode && g_detail_index<0){
    char footer[160];lv_snprintf(footer,sizeof(footer),"%s   L/R SOURCE   Y STAR   START FAV   SELECT SETTINGS",g_favorites_mode ? "FAVORITES" : source_label(g_source));
    label_text_changed(g_footer_label,footer);
  }
  if (g_detail_index >= 0) {
    const bool source_focus = g_toolbar_focus >= 0 && g_toolbar_focus < SOURCE_COUNT;
    for (int i = 0; i < SOURCE_COUNT; i++) {
      if (g_toolbar_buttons[i] != NULL)
        style_button(g_toolbar_buttons[i], source_focus ? i == g_toolbar_focus : i == g_source);
    }
    for (int i = 0; i < g_detail_action_count; i++) {
      if (g_detail_buttons[i] != NULL)
        style_button(g_detail_buttons[i], !source_focus && i == g_detail_focus);
      if (g_detail_button_labels[i] != NULL)
        label_text_changed(g_detail_button_labels[i], detail_action_text(i));
      if (g_detail_buttons[i] != NULL) {
        if ((i == 1 && g_detail_is_remote && !g_detail_install_valid) ||
            (i == 2 && !g_detail_is_installed) ||
            (mutations_busy() && i < g_detail_action_count-1))
          lv_obj_add_state(g_detail_buttons[i], LV_STATE_DISABLED);
        else
          lv_obj_clear_state(g_detail_buttons[i], LV_STATE_DISABLED);
      }
    }
    if (g_detail_status_label != NULL)
      label_text_changed(g_detail_status_label, g_install_status);
    if (g_detail_progress != NULL) {
      const int percent = g_install_total == 0 ? 0 :
          (int)(((uint64_t)g_install_done * 100U) / g_install_total);
      lv_bar_set_value(g_detail_progress, percent > 100 ? 100 : percent, LV_ANIM_OFF);
    }
    return 0;
  }
  const int selected = g_selected;
  for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
    const bool active = i == g_toolbar_focus ||
                        (i < SOURCE_COUNT && !g_favorites_mode && i == g_source) ||
                        (i == 4 && g_favorites_mode) ||
                        (i == 5 && g_settings_mode) ||
                        (i == 9 && g_keyboard_open);
    if (g_toolbar_buttons[i] != NULL) style_button(g_toolbar_buttons[i], active);
  }
  if (g_toolbar_buttons[6] != NULL) {
    const int record = selected_record_index();
    const bool allowed = favorite_allowed(record);
    if (allowed) lv_obj_clear_state(g_toolbar_buttons[6], LV_STATE_DISABLED);
    else lv_obj_add_state(g_toolbar_buttons[6], LV_STATE_DISABLED);
    style_button(g_toolbar_buttons[6], allowed && path_is_favorite(g_paths[record]));
  }
  if (g_search_area != NULL)
    lv_obj_set_style_border_color(g_search_area,
        lv_color_hex(g_toolbar_focus == 7 ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
  if (g_archive_up_button != NULL) {
    style_button(g_archive_up_button, g_toolbar_focus == 11);
    if (g_source == SOURCE_ARCHIVE && g_archive_level == ARCHIVE_LEVEL_SEARCH)
      lv_obj_add_state(g_archive_up_button, LV_STATE_DISABLED);
    else
      lv_obj_clear_state(g_archive_up_button, LV_STATE_DISABLED);
  }
  update_list_scrollbar();
  if(g_list)lv_obj_update_layout(g_list);
  char visible_names[VISIBLE_ROWS][LIST_LABEL_BYTES];
  for (int row = 0; row < VISIBLE_ROWS; row++) {
    const int view_index = g_first_visible + row;
    const bool visible = view_index < g_view_count;
    if (g_buttons[row] != NULL) {
      if (visible) {
        const int index = g_visible_indices[view_index];
        pax_list_label(visible_names[row], sizeof(visible_names[row]), display_name_for(index), g_hide_pax_extension && !g_titles[index][0]);
        lv_obj_clear_flag(g_buttons[row], LV_OBJ_FLAG_HIDDEN);
        update_list_row_text(row, index, visible_names[row]);
        style_button(g_buttons[row], view_index == selected);
      } else {
        lv_obj_add_flag(g_buttons[row], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
  if (g_count_label != NULL)
    label_number_changed(g_count_label,g_view_count," APPS");
  if (g_archive_status_label != NULL)
    label_text_changed(g_archive_status_label, g_archive_status);
  if (g_archive_progress != NULL) {
    const int percent = g_install_total == 0 ? 0 :
        (int)(((uint64_t)g_install_done * 100U) / g_install_total);
    lv_bar_set_value(g_archive_progress, percent > 100 ? 100 : percent, LV_ANIM_OFF);
  }
  if (g_empty_label != NULL) {
    if (g_view_count > 0) lv_obj_add_flag(g_empty_label, LV_OBJ_FLAG_HIDDEN);
    else {
      const char *message = g_catalog_loading ? "LOADING APPS... YOU CAN SWITCH SOURCES" : g_catalog_failed ? (g_source==SOURCE_NETWORK ? "PAX SUPPLY UNAVAILABLE - START SERVER OR CHECK URL" : "SEARCH FAILED - CHECK STATUS OR REFRESH") :
          (g_favorites_mode ? "NO FAVORITES YET - PRESS Y ON AN APP" :
           (g_search_text[0] != '\0' ? "NO SEARCH MATCHES" : "NO APPS FOUND"));
      label_text_changed(g_empty_label, message);
      lv_obj_clear_flag(g_empty_label, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (g_volume_slider != NULL) {
    lv_slider_set_value(g_volume_slider, g_volume_level, LV_ANIM_OFF);
    if (g_volume_value != NULL) label_number_changed(g_volume_value,g_volume_level,"%");
    lv_obj_set_style_border_color(g_volume_slider,
        lv_color_hex(g_settings_focus == 0 ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_volume_slider, g_settings_focus == 0 ? 2 : 1, LV_PART_MAIN);
  }
  if (g_brightness_slider != NULL) {
    lv_slider_set_value(g_brightness_slider, g_brightness_level, LV_ANIM_OFF);
    if (g_brightness_value != NULL) label_number_changed(g_brightness_value,g_brightness_level,"%");
    lv_obj_set_style_border_color(g_brightness_slider,
        lv_color_hex(g_settings_focus == 1 ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_brightness_slider, g_settings_focus == 1 ? 2 : 1, LV_PART_MAIN);
  }
  for (int i = 0; i < SETTINGS_FOCUS_COUNT; i++) {
    if (g_settings_focus_widgets[i] == NULL) continue;
    const bool focused = i == g_settings_focus;
    if (g_settings_focus_widgets[i] == g_volume_slider ||
        g_settings_focus_widgets[i] == g_brightness_slider) {
      lv_obj_set_style_border_color(g_settings_focus_widgets[i],
                                    lv_color_hex(focused ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
      lv_obj_set_style_border_width(g_settings_focus_widgets[i], focused ? 2 : 1, LV_PART_MAIN);
    } else if (g_settings_focus_widgets[i] == g_background_dropdown) {
      lv_obj_set_style_border_color(g_background_dropdown,
                                    lv_color_hex(focused ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
      lv_obj_set_style_border_width(g_background_dropdown, focused ? 2 : 1, LV_PART_MAIN);
    } else {
      style_button(g_settings_focus_widgets[i], focused);
    }
  }
  if (g_settings_back_button != NULL)
    style_button(g_settings_back_button, g_settings_focus == 21);
  if (g_app_details_switch != NULL) {
    if (g_app_details_enabled) lv_obj_add_state(g_app_details_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(g_app_details_switch, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(g_app_details_switch,
        lv_color_hex(g_settings_focus == 20 ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
  }
  for(int option=0;option<2;option++)if(g_list_switches[option]){
    if(option==0 ? g_hide_pax_extension : g_list_file_details)lv_obj_add_state(g_list_switches[option],LV_STATE_CHECKED);
    else lv_obj_clear_state(g_list_switches[option],LV_STATE_CHECKED);
    lv_obj_set_style_border_color(g_list_switches[option],lv_color_hex(g_settings_focus==22+option ? COL_ACCENT : COL_MUTED),LV_PART_MAIN);
  }
  if(g_theme_value_label)label_text_changed(g_theme_value_label,g_theme.name);
  if(g_size_value_label)label_text_changed(g_size_value_label,pax_size_unit_name(g_size_unit));
  if(g_sort_value_label)label_text_changed(g_sort_value_label,pax_sort_name(g_sort_mode));
  if (g_auto_brightness_button != NULL) {
    style_button(g_auto_brightness_button, g_settings_focus == 2);
    if (g_auto_brightness_state_label != NULL)
      label_text_changed(g_auto_brightness_state_label, g_auto_brightness_enabled ? "ON" : "OFF");
  }
  if (g_background_dropdown != NULL) {
    lv_obj_set_style_border_color(g_background_dropdown,
        lv_color_hex(g_settings_focus == 3 ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_background_dropdown, g_settings_focus == 3 ? 2 : 1, LV_PART_MAIN);
    if (g_background_value_label != NULL) {
      lv_label_set_long_mode(g_background_value_label,
          g_settings_focus == 3 ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP);
      if (g_background_index >= 0 && g_background_index < g_background_count) {
        const char *name = g_background_paths[g_background_index];
        for (const char *p = name; *p != '\0'; p++) if (*p == '/') name = p + 1;
        label_text_changed(g_background_value_label, name);
      } else {
        label_text_changed(g_background_value_label, "None");
      }
    }
  }
  if(g_archive_collection_value_label)label_text_changed(g_archive_collection_value_label,g_archive_collection);
  if (g_archive_collection_button != NULL)
    style_button(g_archive_collection_button, g_settings_focus == 4);
  if (g_archive_email_value_label != NULL)
    label_text_changed(g_archive_email_value_label,
                      g_archive_email[0] != '\0' ? g_archive_email : "Select to enter email");
  if (g_archive_password_value_label != NULL)
    label_text_changed(g_archive_password_value_label,
                      g_archive_password[0] != '\0' ? "********" : "Select to enter password");
  update_archive_s3_credential_display();
  if (g_archive_s3_status_label != NULL) {
    label_text_changed(g_archive_s3_status_label, g_archive_s3_status);
    const uint32_t status_color = has_prefix(g_archive_s3_status, "S3 keys verified") ? COL_OK :
        (has_prefix(g_archive_s3_status, "S3 key check was not accepted") ||
         has_prefix(g_archive_s3_status, "S3 keys rejected") ? COL_ERROR : COL_MUTED);
    lv_obj_set_style_text_color(g_archive_s3_status_label, lv_color_hex(status_color), LV_PART_MAIN);
  }
  if (g_archive_auth_status_label != NULL)
    label_text_changed(g_archive_auth_status_label, g_archive_auth_status);
  if (g_archive_login_button != NULL) {
    style_button(g_archive_login_button, g_settings_focus == 7);
    if (g_archive_login_button_label != NULL)
      label_text_changed(g_archive_login_button_label, g_archive_authenticated ? "SIGN OUT" : "SIGN IN");
  }
  for(int i=0;i<4;i++)if(g_network_extra_labels[i]){
    char supply_value[PATH_BYTES+16];
    lv_snprintf(supply_value,sizeof(supply_value),g_network_auto ? "AUTO: %s" : "%s",g_network_url[0] ? g_network_url : "Open LAN to find server");
    const char *value=i==0 ? supply_value : i==1 ? g_wifi_ssid : i==2 ? (g_wifi_password_changed ? (g_wifi_password[0] ? "******** (staged)" : "Open network (staged)") : (g_wifi_available ? (g_wifi_status.password_set ? "******** (unchanged)" : "Open network") : "Firmware update needed")) : (!g_wifi_available ? "FIRMWARE UPDATE NEEDED" : g_wifi_status.status==1 ? "CONNECTING..." : g_wifi_status.status==-1 ? "FAILED; RESTORING WORKING WI-FI" : g_wifi_status.status==-2 ? "NOT CONNECTED; CHECK NAME/PASSWORD, THEN RETRY" : "APPLY WI-FI");
    label_text_changed(g_network_extra_labels[i],value);
  }
  (void)update_c6_progress_on_host(NULL);
  (void)update_firmware_on_host(NULL);
  if (g_network_dhcp_label != NULL)
    label_text_changed(g_network_dhcp_label, g_network_config.dhcp_enabled ? "ON" : "OFF");
  if (g_network_status_label != NULL) {
    char address[20];
    char network_summary[256];
    ipv4_to_text(g_network_live.ipv4, address, sizeof(address));
    copy_text(network_summary, sizeof(network_summary), network_type_name(g_network_live.network_type));
    append_text(network_summary, sizeof(network_summary), g_network_live.connected ? " | Connected | " : " | No link | ");
    if(g_network_live.connected)append_text(network_summary,sizeof(network_summary),g_network_live.dhcp_enabled ? "DHCP | " : "Static IPv4 | ");
    append_text(network_summary, sizeof(network_summary), g_network_live.ipv4 != 0 ? address : "No IPv4 address");
    if(g_network_live.network_type==PAX_NETWORK_WIFI && g_network_live.connected && g_wifi_available){
      append_wifi_summary(network_summary,sizeof(network_summary),&g_wifi_status);
    }
    label_text_changed(g_network_status_label, network_summary);
  }
  uint32_t network_values[4] = {g_network_config.ipv4, g_network_config.netmask,
                                g_network_config.gateway, g_network_config.dns};
  for (int i = 0; i < 4; i++) {
    if (g_network_value_labels[i] == NULL) continue;
    char address[20];
    ipv4_to_text(network_values[i], address, sizeof(address));
    label_text_changed(g_network_value_labels[i], address);
  }
  const int mutation_focus[]={28};
  for(size_t k=0;k<sizeof(mutation_focus)/sizeof(mutation_focus[0]);k++){
    int focus=mutation_focus[k];
    if(focus<SETTINGS_FOCUS_COUNT && g_settings_focus_widgets[focus]){
      if(mutations_busy())lv_obj_add_state(g_settings_focus_widgets[focus],LV_STATE_DISABLED);
      else lv_obj_clear_state(g_settings_focus_widgets[focus],LV_STATE_DISABLED);
    }
  }
  if(g_network_apply_button){
    if(g_settings_scan_busy || g_settings_scan_pending || mutations_busy())lv_obj_add_state(g_network_apply_button,LV_STATE_DISABLED);
    else lv_obj_clear_state(g_network_apply_button,LV_STATE_DISABLED);
  }
  for (int i = 0; i < PAX_STORAGE_COUNT; i++) {
    if(g_storage_buttons[i]){
      if(g_settings_scan_busy || g_settings_scan_pending || mutations_busy())lv_obj_add_state(g_storage_buttons[i],LV_STATE_DISABLED);
      else lv_obj_clear_state(g_storage_buttons[i],LV_STATE_DISABLED);
    }
    if (g_storage_status_labels[i] != NULL) {
      char summary[120];
      format_storage_status(i, summary, sizeof(summary));
      label_text_changed(g_storage_status_labels[i], summary);
    }
    if (g_storage_button_labels[i] != NULL)
      label_text_changed(g_storage_button_labels[i],
          g_storage_status[i].mounted ? "EJECT" : "MOUNT");
  }
  if (g_settings_message_label != NULL)
    label_text_changed(g_settings_message_label, g_settings_message);
  if (g_settings_mode && !(g_c6_available && g_c6_status.state==PAX_C6_BUSY)) {
    lv_obj_t *focused = g_settings_focus >= 0 && g_settings_focus < SETTINGS_FOCUS_COUNT ?
                        g_settings_focus_widgets[g_settings_focus] : NULL;
    if (focused != NULL && g_last_settings_focus!=g_settings_focus){lv_obj_scroll_to_view(focused, LV_ANIM_OFF);g_last_settings_focus=g_settings_focus;}
  }
  return 0;
}

static void list_scrollbar_event(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  lv_indev_t *indev = lv_indev_active();
  if ((code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) ||
      indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER ||
      g_view_count <= VISIBLE_ROWS) return;
  lv_point_t point;
  lv_area_t area;
  lv_indev_get_point(indev, &point);
  lv_obj_get_coords(g_list_scrollbar, &area);
  const int height = list_scrollbar_thumb_height();
  const int travel = g_list_scrollbar_height - height;
  if (travel <= 0) return;
  int offset = point.y - area.y1 - height / 2;
  if (offset < 0) offset = 0;
  if (offset > travel) offset = travel;
  const int max_first = g_view_count - VISIBLE_ROWS;
  const int first = (offset * max_first + travel / 2) / travel;
  if (first == g_first_visible) return;
  g_first_visible = first;
  if (g_selected < first) g_selected = first;
  if (g_selected >= first + VISIBLE_ROWS) g_selected = first + VISIBLE_ROWS - 1;
  g_toolbar_focus = -1;
  (void)update_selection_on_host(NULL);
}

static void list_touch_event(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  lv_indev_t *indev = lv_indev_active();
  if (code == LV_EVENT_PRESSED) {
    g_list_touch_tracking = indev != NULL &&
                            lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER;
    g_list_touch_swiped = false;
    g_list_touch_scroll_accum = 0;
    if (g_list_touch_tracking) {
      lv_indev_get_point(indev, &g_list_touch_start);
      g_list_touch_last = g_list_touch_start;
    }
    return;
  }
  if (code == LV_EVENT_RELEASED) {
    g_list_touch_tracking = false;
    return;
  }
  if (code != LV_EVENT_PRESSING || !g_list_touch_tracking ||
      indev == NULL || g_view_count <= VISIBLE_ROWS)
    return;

  lv_point_t point;
  lv_indev_get_point(indev, &point);
  const int total_dx = point.x - g_list_touch_start.x;
  const int total_dy = point.y - g_list_touch_start.y;
  if (abs_int(total_dy) <= abs_int(total_dx)) return;

  g_list_touch_scroll_accum += point.y - g_list_touch_last.y;
  g_list_touch_last = point;
  /* Advance one virtual row per row-height of finger travel. The former
   * fixed 32 px threshold plus LVGL's page gesture caused a single drag to
   * jump five entries (and sometimes applied both handlers). */
  const int row_step = g_list_touch_row_step > 0 ? g_list_touch_row_step : ui_px(48);
  if (abs_int(g_list_touch_scroll_accum) < row_step) return;

  int steps = abs_int(g_list_touch_scroll_accum) / row_step;
  const int direction = g_list_touch_scroll_accum < 0 ? 1 : -1;
  g_list_touch_scroll_accum += direction * steps * row_step;
  /* Even a drag at either end of the list is a scroll gesture, not a tap
   * that should accidentally launch the row when the finger is released. */
  g_list_touch_swiped = true;
  const int old_selected = g_selected;
  const int old_first = g_first_visible;
  const int max_first = g_view_count > VISIBLE_ROWS ?
                        g_view_count - VISIBLE_ROWS : 0;
  g_selected += direction * steps;
  if (g_selected < 0) g_selected = 0;
  if (g_selected >= g_view_count) g_selected = g_view_count - 1;
  g_first_visible += direction * steps;
  if (g_first_visible < 0) g_first_visible = 0;
  if (g_first_visible > max_first) g_first_visible = max_first;
  if (g_selected < g_first_visible)
    g_first_visible = g_selected;
  else if (g_selected >= g_first_visible + VISIBLE_ROWS)
    g_first_visible = g_selected - VISIBLE_ROWS + 1;
  if (g_first_visible > max_first) g_first_visible = max_first;
  if (g_selected == old_selected && g_first_visible == old_first) return;
  if (g_svc != NULL && g_svc->log_printf != NULL)
    g_svc->log_printf("launcher_ui list touch drag: %d->%d first=%d/%d\n",
                      old_selected, g_selected, g_first_visible, g_view_count);
  (void)update_selection_on_host(NULL);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  (void)indev;
  int x = 0, y = 0;
  if (g_svc->touch_read != NULL && g_svc->touch_read(&x, &y)) {
    /* The loader already maps physical touch coordinates into the selected
     * PAX canvas. Preserve them directly; scaling a canvas coordinate again
     * would shift touch targets whenever the launcher uses a non-legacy size. */
    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    if (data->point.x < 0) data->point.x = 0;
    if (data->point.y < 0) data->point.y = 0;
    if (data->point.x >= g_canvas_w) data->point.x = g_canvas_w - 1;
    if (data->point.y >= g_canvas_h) data->point.y = g_canvas_h - 1;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  (void)indev;
  bool activity = false;
  int dx = 0, dy = 0, buttons = 0;
  if (g_svc != NULL && g_svc->input_mouse_read != NULL &&
      g_svc->input_mouse_read(&dx, &dy, &buttons)) {
    /* The HID provider normalizes relative movement to screen direction. */
    g_mouse_x += dx;
    g_mouse_y += dy;
    activity = dx != 0 || dy != 0 || buttons != g_mouse_buttons;
    g_mouse_buttons = buttons;
  }
  int wheel = 0;
  if (g_svc != NULL && g_svc->input_mouse_scroll_read != NULL &&
      g_svc->input_mouse_scroll_read(&wheel) && wheel != 0) {
    activity = true;
    int pending = __atomic_add_fetch(&g_mouse_wheel_pending, wheel, __ATOMIC_RELAXED);
    if (pending > 32) __atomic_store_n(&g_mouse_wheel_pending, 32, __ATOMIC_RELAXED);
    if (pending < -32) __atomic_store_n(&g_mouse_wheel_pending, -32, __ATOMIC_RELAXED);
  }
  const uint32_t now_ms = lv_tick_get();
  if (activity) {
    g_mouse_cursor_seen = true;
    g_mouse_last_activity_ms = now_ms;
  }
  if (g_mouse_cursor != NULL) {
    const bool visible = g_mouse_cursor_seen &&
        (g_mouse_buttons != 0 || (uint32_t)(now_ms - g_mouse_last_activity_ms) < MOUSE_CURSOR_IDLE_MS);
    if (visible) lv_obj_clear_flag(g_mouse_cursor, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g_mouse_cursor, LV_OBJ_FLAG_HIDDEN);
  }
  if (g_mouse_x < 0) g_mouse_x = 0;
  if (g_mouse_y < 0) g_mouse_y = 0;
  if (g_mouse_x >= g_canvas_w) g_mouse_x = g_canvas_w - 1;
  if (g_mouse_y >= g_canvas_h) g_mouse_y = g_canvas_h - 1;
  data->point.x = g_mouse_x;
  data->point.y = g_mouse_y;
  data->state = g_mouse_cursor_seen && (g_mouse_buttons & 3) != 0 ?
                LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->continue_reading = false;
}

static void create_mouse_cursor(lv_obj_t *screen)
{
  if (g_svc == NULL || g_svc->input_mouse_read == NULL) return;
  /* Cursor dimensions are canvas pixels, independent of the UI text scale. */
  static lv_point_precise_t arrow_points[8];
  arrow_points[0] = (lv_point_precise_t){0, 0};
  arrow_points[1] = (lv_point_precise_t){0, 15};
  arrow_points[2] = (lv_point_precise_t){4, 11};
  arrow_points[3] = (lv_point_precise_t){7, 17};
  arrow_points[4] = (lv_point_precise_t){10, 16};
  arrow_points[5] = (lv_point_precise_t){7, 10};
  arrow_points[6] = (lv_point_precise_t){13, 10};
  arrow_points[7] = (lv_point_precise_t){0, 0};
  g_mouse_indev = lv_indev_create();
  if (g_mouse_indev == NULL) return;
  lv_indev_set_type(g_mouse_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(g_mouse_indev, mouse_read_cb);
  lv_indev_set_display(g_mouse_indev, g_display);
  /* Read touchpad reports at display cadence rather than LVGL's slower
   * default input period, which makes relative-pointer movement feel less
   * stepped without increasing the display refresh rate. */
  lv_timer_t *read_timer = lv_indev_get_read_timer(g_mouse_indev);
  if (read_timer != NULL) lv_timer_set_period(read_timer, 16);
  g_mouse_cursor = lv_obj_create(screen);
  if (g_mouse_cursor == NULL) return;
  lv_obj_remove_style_all(g_mouse_cursor);
  lv_obj_set_size(g_mouse_cursor, 15, 19);
  lv_obj_clear_flag(g_mouse_cursor, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *outline = lv_line_create(g_mouse_cursor);
  lv_line_set_points(outline, arrow_points,
                     sizeof(arrow_points) / sizeof(arrow_points[0]));
  lv_obj_set_style_line_color(outline, lv_color_hex(COL_BG), LV_PART_MAIN);
  lv_obj_set_style_line_width(outline, 2, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(outline, false, LV_PART_MAIN);
  lv_obj_t *fill = lv_line_create(g_mouse_cursor);
  lv_line_set_points(fill, arrow_points,
                     sizeof(arrow_points) / sizeof(arrow_points[0]));
  lv_obj_set_style_line_color(fill, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_line_width(fill, 1, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(fill, false, LV_PART_MAIN);
  lv_obj_add_flag(g_mouse_cursor, LV_OBJ_FLAG_HIDDEN);
  lv_indev_set_cursor(g_mouse_indev, g_mouse_cursor);
}

static bool mouse_inside_object(const lv_obj_t *object)
{
  if (object == NULL) return false;
  lv_area_t area;
  lv_obj_get_coords(object, &area);
  return g_mouse_x >= area.x1 && g_mouse_x <= area.x2 &&
         g_mouse_y >= area.y1 && g_mouse_y <= area.y2;
}

/* Called on the LVGL host task. Wheel-positive is the HID convention for
 * scrolling toward earlier content; LVGL uses negative dy for that motion. */
static int mouse_scroll_on_host(void *arg)
{
  const int wheel = (int)(intptr_t)arg;
  if (wheel == 0) return 0;

  if (g_detail_index >= 0) {
    if (!mouse_inside_object(g_detail_info_view)) return 0;
    const int steps = wheel > 8 ? 8 : (wheel < -8 ? -8 : wheel);
    lv_obj_scroll_by_bounded(g_detail_info_view, 0, -steps * ui_px(34), LV_ANIM_ON);
    return 1;
  }
  if (g_settings_mode) {
    if (!mouse_inside_object(g_settings_panel)) return 0;
    const int steps = wheel > 8 ? 8 : (wheel < -8 ? -8 : wheel);
    lv_obj_scroll_by(g_settings_panel, 0, -steps * ui_px(32), LV_ANIM_ON);
    return 1;
  }
  if (!mouse_inside_object(g_list) || g_view_count <= 0) return 0;

  /* The library is virtualized to a handful of visible rows, so move its
   * logical selection rather than scrolling those recycled row widgets away. */
  g_toolbar_focus = -1;
  g_selected -= wheel;
  if (g_selected < 0) g_selected = 0;
  if (g_selected >= g_view_count) g_selected = g_view_count - 1;
  if (g_selected < g_first_visible)
    g_first_visible = g_selected;
  else if (g_selected >= g_first_visible + VISIBLE_ROWS)
    g_first_visible = g_selected - VISIBLE_ROWS + 1;
  const int max_first = g_view_count > VISIBLE_ROWS ? g_view_count - VISIBLE_ROWS : 0;
  if (g_first_visible > max_first) g_first_visible = max_first;
  (void)update_selection_on_host(NULL);
  return 1;
}

static void ensure_keyboard(lv_obj_t *screen)
{
  if(g_keyboard)return;
    g_keyboard = lv_keyboard_create(screen);
    if (g_keyboard != NULL) {
      g_svc->log_printf("launcher_ui: keyboard created; screen=%dx%d initial=%dx%d\n",
                        lv_obj_get_width(screen), lv_obj_get_height(screen),
                        lv_obj_get_width(g_keyboard), lv_obj_get_height(g_keyboard));
      lv_obj_set_size(g_keyboard, g_canvas_w - 2 * ui_px(8), ui_px(100));
      /* lv_keyboard_create() aligns itself to the parent's bottom edge.
       * Clear that inherited alignment before applying an explicit position;
       * otherwise LVGL interprets keyboard_y as a bottom-alignment offset and
       * places the keyboard below the screen. */
      lv_obj_set_align(g_keyboard, LV_ALIGN_DEFAULT);
      lv_obj_set_pos(g_keyboard, ui_px(8), g_canvas_h-ui_px(100)-ui_px(4));
      /* LVGL stores position/size as style values and resolves object coords
       * during layout. Force that pass before rendering the first keyboard
       * frame; otherwise the fresh button-matrix can remain at 0x0 until a
       * later layout event, leaving only the list's reserved blank area. */
      lv_obj_update_layout(screen);
      lv_obj_move_foreground(g_keyboard);
      lv_keyboard_set_textarea(g_keyboard, g_search_area);
      lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
      lv_keyboard_set_popovers(g_keyboard, false);
      lv_obj_set_style_bg_color(g_keyboard, lv_color_hex(COL_BG), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_60, LV_PART_MAIN);
      lv_obj_set_style_border_color(g_keyboard, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
      lv_obj_set_style_border_width(g_keyboard, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(g_keyboard, ui_px(5), LV_PART_MAIN);
      lv_obj_set_style_pad_all(g_keyboard, ui_px(2), LV_PART_MAIN);
      lv_obj_set_style_bg_color(g_keyboard, lv_color_hex(COL_PANEL), LV_PART_ITEMS);
      lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_70, LV_PART_ITEMS);
      lv_obj_set_style_border_color(g_keyboard, lv_color_hex(COL_MUTED), LV_PART_ITEMS);
      lv_obj_set_style_border_width(g_keyboard, 1, LV_PART_ITEMS);
      lv_obj_set_style_radius(g_keyboard, ui_px(3), LV_PART_ITEMS);
      lv_obj_set_style_text_color(g_keyboard, lv_color_hex(COL_TEXT), LV_PART_ITEMS);
      lv_obj_set_style_text_opa(g_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
      lv_obj_set_style_text_font(g_keyboard, &lv_font_montserrat_20, LV_PART_ITEMS);
      lv_obj_set_style_pad_row(g_keyboard, ui_px(2), LV_PART_MAIN);
      lv_obj_set_style_pad_column(g_keyboard, ui_px(2), LV_PART_MAIN);
      lv_obj_set_style_bg_color(g_keyboard, lv_color_hex(COL_SELECTED),
                                LV_PART_ITEMS | LV_STATE_FOCUSED);
      lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_90,
                               LV_PART_ITEMS | LV_STATE_FOCUSED);
      lv_obj_set_style_border_color(g_keyboard, lv_color_hex(COL_ACCENT),
                                    LV_PART_ITEMS | LV_STATE_FOCUSED);
      lv_obj_set_style_text_color(g_keyboard, lv_color_hex(COL_TEXT),
                                  LV_PART_ITEMS | LV_STATE_FOCUSED);
      lv_obj_add_event_cb(g_keyboard, keyboard_value_event, LV_EVENT_VALUE_CHANGED, NULL);
      lv_obj_add_event_cb(g_keyboard, keyboard_close_event, LV_EVENT_READY, NULL);
      lv_obj_add_event_cb(g_keyboard, keyboard_close_event, LV_EVENT_CANCEL, NULL);
      /* LVGL 9.5's text keyboard map has 40 focusable keys across four rows. */
      if (g_keyboard_focus < 0 || g_keyboard_focus >= 40) g_keyboard_focus = 0;
      lv_buttonmatrix_set_selected_button(g_keyboard, (uint32_t)g_keyboard_focus);
      lv_obj_add_state(g_keyboard, LV_STATE_FOCUSED);
      lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
      g_svc->log_printf("launcher_ui: native keyboard visible=%d at (%d,%d) %dx%d; focus=%d\n",
                        lv_obj_is_visible(g_keyboard) ? 1 : 0,
                        lv_obj_get_x(g_keyboard), lv_obj_get_y(g_keyboard),
                        lv_obj_get_width(g_keyboard), lv_obj_get_height(g_keyboard),
                        g_keyboard_focus);
    }
 }

static void sync_keyboard_layout(void)
{
  if(!g_keyboard || g_keyboard_layout==(int)g_keyboard_open)return;
  if(g_keyboard_layout>=0)g_keyboard_transitions++;
  g_keyboard_layout=g_keyboard_open;
  if(g_keyboard_open){
    lv_obj_clear_flag(g_keyboard,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(g_keyboard);
    lv_buttonmatrix_set_selected_button(g_keyboard,(uint32_t)g_keyboard_focus);
  }else lv_obj_add_flag(g_keyboard,LV_OBJ_FLAG_HIDDEN);
  if(g_footer_label){if(g_keyboard_open)lv_obj_add_flag(g_footer_label,LV_OBJ_FLAG_HIDDEN);else lv_obj_clear_flag(g_footer_label,LV_OBJ_FLAG_HIDDEN);}
  if(g_archive_status_label){if(g_keyboard_open)lv_obj_add_flag(g_archive_status_label,LV_OBJ_FLAG_HIDDEN);else lv_obj_clear_flag(g_archive_status_label,LV_OBJ_FLAG_HIDDEN);}
  if(g_archive_progress){if(g_keyboard_open)lv_obj_add_flag(g_archive_progress,LV_OBJ_FLAG_HIDDEN);else lv_obj_clear_flag(g_archive_progress,LV_OBJ_FLAG_HIDDEN);}
  const int list_y=ui_px(24)+ui_px(30)+ui_px(5),pad=ui_px(5),gap=ui_px(4);
  const int keyboard_y=g_canvas_h-ui_px(100)-ui_px(4);
  if(g_settings_panel){
    const int bottom=g_keyboard_open ? keyboard_y-ui_px(4) : g_canvas_h-ui_px(10);
    lv_obj_set_height(g_settings_panel,bottom-(ui_px(24)+ui_px(30)+ui_px(7)));
  }
  if(g_list){
    const int bottom=g_keyboard_open ? keyboard_y-ui_px(4) : (g_source==SOURCE_ARCHIVE ? g_canvas_h-ui_px(34) : g_canvas_h-ui_px(24)-ui_px(5));
    const int height=bottom-list_y,row=(height-2*pad-(VISIBLE_ROWS-1)*gap)/VISIBLE_ROWS;
    lv_obj_set_height(g_list,height);g_list_scrollbar_height=height-2*pad;
    if(g_list_scrollbar)lv_obj_set_height(g_list_scrollbar,g_list_scrollbar_height);
    g_list_touch_row_step=(row>ui_px(25) ? row : ui_px(25))+gap;
    for(int i=0;i<VISIBLE_ROWS;i++)if(g_buttons[i])lv_obj_set_height(g_buttons[i],row>ui_px(25) ? row : ui_px(25));
    memset(g_row_appearance,0,sizeof(g_row_appearance));update_list_scrollbar();
  }
}

static int build_ui_on_host(void *arg)
{
  tooltip_reset();
  g_tooltips_value=NULL;
  g_ui_builds++;g_button_appearance_count=0;
  memset(g_row_appearance,0,sizeof(g_row_appearance));
  g_keyboard_layout=-1;g_last_settings_focus=-1;g_footer_label=NULL;
  (void)arg;
  char host_names[VISIBLE_ROWS][LIST_LABEL_BYTES];
  /* load_apps() already fetched and parsed this source on the PAX worker.
   * Copy its bounded snapshot here for LVGL instead of repeating SD/network
   * I/O from the ESPHome UI task. In particular, a TLS fetch here could block
   * the whole dashboard for seconds while the network/store tab is opened. */
  for (int row = 0; row < VISIBLE_ROWS; row++) {
    g_buttons[row] = NULL;
    g_labels[row] = NULL;
    g_list_detail_labels[row] = NULL;
  }
  g_list_scrollbar = NULL;
  g_list_scrollbar_thumb = NULL;
  g_settings_panel = NULL;
  g_detail_info_view = NULL;
  __atomic_store_n(&g_mouse_wheel_pending, 0, __ATOMIC_RELAXED);
  g_keyboard = NULL;
  for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) g_toolbar_buttons[i] = NULL;
  for (int i = 0; i < MAX_DETAIL_FILES; i++) g_detail_file_state_labels[i] = NULL;
  g_search_area = NULL;
  g_archive_up_button = NULL;
  g_archive_status_label = NULL;
  g_archive_progress = NULL;
  g_volume_slider = NULL;
  g_brightness_slider = NULL;
  g_volume_value = NULL;
  g_brightness_value = NULL;
  g_settings_back_button = NULL;
  g_app_details_switch = NULL;
  g_list_switches[0]=g_list_switches[1]=NULL;
  g_theme_value_label=NULL;
  g_sort_value_label=NULL;g_size_value_label=NULL;
  g_auto_brightness_button = NULL;
  g_auto_brightness_state_label = NULL;
  g_background_value_label = NULL;
  g_archive_collection_value_label = NULL;
  g_archive_collection_button = NULL;
  g_status_clock_icon = NULL;
  g_status_clock = NULL;
  g_status_network_icon = NULL;
  g_status_battery_icon = NULL;
  g_status_battery_level = NULL;
  g_background_button = NULL;
  g_background_dropdown = NULL;
  g_wifi_dropdown = NULL;g_wifi_scan_message_label=NULL;
  g_network_status_label = NULL;
  g_network_dhcp_button = NULL;
  g_network_dhcp_label = NULL;
  g_network_apply_button = NULL;
  for(int i=0;i<4;i++){g_network_extra_labels[i]=NULL;g_c6_labels[i]=NULL;}
  for(int i=0;i<3;i++)g_firmware_labels[i]=NULL;
  g_firmware_confirm=false;
  g_firmware_last_state=UINT32_MAX;
  g_c6_progress=NULL;
  g_settings_message_label = NULL;
  for (int i = 0; i < SETTINGS_FOCUS_COUNT; i++) g_settings_focus_widgets[i] = NULL;
  g_archive_auth_status_label = NULL;
  g_archive_s3_status_label = NULL;
  g_archive_email_value_label = NULL;
  g_archive_password_value_label = NULL;
  g_archive_s3_access_value_label = NULL;
  g_archive_s3_secret_value_label = NULL;
  for (int i = 0; i < 2; i++) {
    g_archive_s3_eye_buttons[i] = NULL;
    g_archive_s3_eye_slashes[i] = NULL;
  }
  g_archive_login_button_label = NULL;
  g_archive_login_button = NULL;
  for (int i = 0; i < 4; i++) g_network_value_labels[i] = NULL;
  for (int i = 0; i < PAX_STORAGE_COUNT; i++) {
    g_storage_status_labels[i] = NULL;
    g_storage_buttons[i] = NULL;
    g_storage_button_labels[i] = NULL;
  }
  g_empty_label = NULL;
  for (int row = 0; row < VISIBLE_ROWS; row++) {
    const int view_index = g_first_visible + row;
    if (view_index < g_view_count)
      pax_list_label(host_names[row], sizeof(host_names[row]), display_name_for(g_visible_indices[view_index]), g_hide_pax_extension && !g_titles[g_visible_indices[view_index]][0]);
    else
      host_names[row][0] = '\0';
  }
  g_svc->log_printf("launcher_ui host rows=%d/%d first='%s'\n",
                    g_view_count < VISIBLE_ROWS ? g_view_count : VISIBLE_ROWS,
                    g_view_count,
                    g_view_count > 0 ? host_names[0] : "(none)");
  lv_init();
  lv_tick_set_cb(tick_cb);
  g_display = lv_display_create(g_canvas_w, g_canvas_h);
  if (g_display == NULL) return -1;
  /* This PAX creates a second LVGL display beside ESPHome's dashboard. Make
   * the PAX display explicit so screen and overlay-layer helpers (notably the
   * on-screen keyboard) cannot accidentally attach to the dashboard display. */
  lv_display_set_default(g_display);
  lv_display_set_flush_cb(g_display, disp_flush_cb);
  lv_display_set_buffers(g_display, g_canvas, NULL, g_canvas_w * g_canvas_h * sizeof(uint16_t),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  g_indev = lv_indev_create();
  if (g_indev == NULL) return -2;
  lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(g_indev, touch_read_cb);
  lv_indev_set_display(g_indev, g_display);

  lv_obj_t *screen = lv_display_get_screen_active(g_display);
  if (screen == NULL) return -3;
  lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), LV_PART_MAIN);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  create_mouse_cursor(screen);
  g_background_widget=lv_image_create(screen);
  lv_obj_clear_flag(g_background_widget,LV_OBJ_FLAG_CLICKABLE);
  lv_image_set_antialias(g_background_widget,false);
  if (g_background_pixels != NULL && g_background_width > 0 && g_background_height > 0) {
    lv_obj_t *background = g_background_widget;
    lv_image_set_src(background, &g_background_descriptor);
    const uint32_t scale_x = (uint32_t)(((uint64_t)g_canvas_w * 256U) / g_background_width);
    const uint32_t scale_y = (uint32_t)(((uint64_t)g_canvas_h * 256U) / g_background_height);
    lv_image_set_scale_x(background, scale_x);
    lv_image_set_scale_y(background, scale_y);
    lv_image_set_antialias(background, false);
    lv_obj_center(background);
  }else lv_obj_add_flag(g_background_widget,LV_OBJ_FLAG_HIDDEN);

  const int status_h = ui_px(24);
  const int header_h = ui_px(30);
  const int margin = ui_px(8);
  const lv_font_t *font = ui_font();
  const int logical_width = (int)(g_canvas_w / g_ui_scale);

  lv_obj_t *topbar = lv_obj_create(screen);
  lv_obj_remove_style_all(topbar);
  lv_obj_set_size(topbar, g_canvas_w, status_h);
  lv_obj_set_pos(topbar, 0, 0);
  lv_obj_set_style_bg_color(topbar, lv_color_hex(COL_PANEL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(topbar, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_border_side(topbar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(topbar, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_border_width(topbar, 1, LV_PART_MAIN);

  /* Keep the LAN source distinct from the live Ethernet status glyph at the
   * right of the top bar. */
  const uint32_t view_icons[6] = {0xf0a0U, 0xf6ffU, 0xf54eU, 0xf187U, 0xf005U, 0xf013U};
  for (int i = 0; i < 6; i++) {
    lv_obj_t *button = lv_button_create(topbar);
    g_toolbar_buttons[i] = button;
    lv_obj_set_size(button, ui_px(18), ui_px(18));
    lv_obj_set_pos(button, ui_px(8 + i * 20), (status_h - ui_px(18)) / 2);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    style_button(button, i < SOURCE_COUNT ?
                 (!g_favorites_mode && i == g_source) :
                 (i == 4 ? g_favorites_mode : g_settings_mode));
    if (i < SOURCE_COUNT)
      lv_obj_add_event_cb(button, source_button_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    else
      lv_obj_add_event_cb(button, toolbar_button_event, LV_EVENT_CLICKED,
                          (void *)(intptr_t)(i == 4 ? -6 : -7));
    lv_obj_t *icon = create_fa_label(button, view_icons[i], lv_color_hex(COL_TEXT));
    lv_obj_center(icon);
  }

  lv_obj_t *brand = lv_label_create(topbar);
  const int brand_width = ui_px(112);
  const lv_font_t *brand_font = &lv_font_montserrat_28;
  lv_point_t brand_size;
  lv_text_get_size(&brand_size, "ESPHOMEBREW", brand_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  if (brand_size.x > brand_width) brand_font = &lv_font_montserrat_20;
  lv_label_set_text(brand, "ESPHOMEBREW");
  lv_label_set_long_mode(brand, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(brand, brand_width);
  lv_obj_set_style_text_color(brand, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_text_font(brand, brand_font, LV_PART_MAIN);
  lv_obj_set_style_text_align(brand, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(brand, LV_ALIGN_CENTER, 0, 0);

  const int status_item_h = ui_px(18);
  /* Use the text's actual font height as a visual reference: the labels are
   * top-aligned inside their boxes, so center-aligning the boxes alone leaves
   * the right-hand status row looking high next to the brand. */
  const int status_item_y = (status_h - status_item_h) / 2 + ui_px(5);
  /* Reserve the widest time before laying out the row. Font metrics are in
   * physical pixels, unlike ui_px(), and 10:00 AM exceeds the old 44-unit box.
   * Check every digit and suffix so the row stays still as the time changes;
   * the same box also accommodates 24-hour HH:MM strings. */
  int clock_width = ui_px(44);
  for (char digit = '0'; digit <= '9'; ++digit) {
    char sample[] = "00:00 AM";
    sample[0] = sample[1] = sample[3] = sample[4] = digit;
    for (int pm = 0; pm < 2; ++pm) {
      sample[6] = pm ? 'P' : 'A';
      lv_point_t size;
      lv_text_get_size(&size, sample, &lv_font_montserrat_20, 0, 0,
                       LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      if (size.x + ui_px(2) > clock_width) clock_width = size.x + ui_px(2);
    }
  }
  /* Reserve the longest weekday/month combination too, keeping the bar
   * stable when tapping between the clock and its full local date. */
  const char *weekdays[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
  const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int day = 0; day < 7; ++day) for (int month = 0; month < 12; ++month) {
    char sample[48];
    copy_text(sample, sizeof(sample), weekdays[day]);
    append_text(sample, sizeof(sample), " 88 ");
    append_text(sample, sizeof(sample), months[month]);
    append_text(sample, sizeof(sample), " 8888");
    lv_point_t size;
    lv_text_get_size(&size, sample, &lv_font_montserrat_14, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (size.x + ui_px(2) > clock_width) clock_width = size.x + ui_px(2);
  }
  const int clock_x = ui_px(logical_width - 84) - clock_width;
  const int clock_icon_x = clock_x - ui_px(18);
  /* Keep the centered brand clear of the time on narrower panels. */
  const int brand_right = clock_icon_x - ui_px(4);
  int available_brand = brand_right - ui_px(128);
  if (available_brand <= 0) lv_obj_add_flag(brand, LV_OBJ_FLAG_HIDDEN);
  else {
    if (available_brand > brand_width) available_brand = brand_width;
    lv_obj_set_width(brand, available_brand);
    lv_text_get_size(&brand_size, "ESPHOMEBREW", brand_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (brand_size.x > available_brand) {
      brand_font = &lv_font_montserrat_14;
      lv_obj_set_style_text_font(brand, brand_font, LV_PART_MAIN);
    }
    if ((g_canvas_w + available_brand) / 2 > brand_right)
      lv_obj_set_x(brand, brand_right - available_brand - (g_canvas_w - available_brand) / 2);
  }
  g_status_clock_icon = create_fa_label(topbar, 0xf017U, lv_color_hex(COL_ACCENT));
  lv_obj_set_size(g_status_clock_icon, ui_px(16), status_item_h);
  lv_obj_set_pos(g_status_clock_icon, clock_icon_x, status_item_y);
  lv_obj_set_style_text_align(g_status_clock_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  g_status_clock = lv_label_create(topbar);
  lv_label_set_long_mode(g_status_clock, LV_LABEL_LONG_CLIP);
  lv_obj_set_size(g_status_clock, clock_width, status_item_h);
  lv_obj_set_pos(g_status_clock, clock_x, status_item_y);
  lv_obj_set_style_text_color(g_status_clock, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_font(g_status_clock, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_align(g_status_clock, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_flag(g_status_clock, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(g_status_clock, ui_px(4));
  lv_obj_add_event_cb(g_status_clock, clock_tap_event, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(g_status_clock_icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(g_status_clock_icon, ui_px(4));
  lv_obj_add_event_cb(g_status_clock_icon, clock_tap_event, LV_EVENT_CLICKED, NULL);
  g_status_network_icon = create_fa_label(topbar, 0xf071U, lv_color_hex(COL_ACCENT));
  lv_obj_set_size(g_status_network_icon, ui_px(16), status_item_h);
  lv_obj_set_pos(g_status_network_icon, ui_px(logical_width - 80), status_item_y);
  lv_obj_set_style_text_align(g_status_network_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  g_status_battery_icon = create_fa_label(topbar, 0xf240U, lv_color_hex(COL_ACCENT));
  lv_obj_set_size(g_status_battery_icon, ui_px(16), status_item_h);
  lv_obj_set_pos(g_status_battery_icon, ui_px(logical_width - 51), status_item_y);
  lv_obj_set_style_text_align(g_status_battery_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  g_status_battery_level = lv_label_create(topbar);
  lv_obj_set_size(g_status_battery_level, ui_px(27), status_item_h);
  lv_obj_set_pos(g_status_battery_level, ui_px(logical_width - 32), status_item_y);
  lv_obj_set_style_text_color(g_status_battery_level, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_font(g_status_battery_level, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_align(g_status_battery_level, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  (void)update_device_status_on_host(NULL);

  lv_obj_t *header = lv_obj_create(screen);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, g_canvas_w, header_h);
  lv_obj_set_pos(header, 0, status_h);
  lv_obj_set_style_bg_color(header, lv_color_hex(COL_PANEL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(header, LV_OPA_60, LV_PART_MAIN);

  lv_obj_t *favorite_item_button = lv_button_create(header);
  g_toolbar_buttons[6] = favorite_item_button;
  const int button_h = ui_px(22);
  const int search_h = ui_px(25);
  const int search_y = (header_h - search_h) / 2;
  const int button_y = (header_h - button_h) / 2;
  const int button_w = ui_px(20);
  lv_obj_set_size(favorite_item_button, button_w, button_h);
  lv_obj_set_pos(favorite_item_button, ui_px(8), button_y);
  lv_obj_set_style_pad_all(favorite_item_button, 0, LV_PART_MAIN);
  const int initially_selected = selected_record_index();
  style_button(favorite_item_button, initially_selected >= 0 &&
               favorite_allowed(initially_selected) &&
               path_is_favorite(g_paths[initially_selected]));
  if (!favorite_allowed(initially_selected))
    lv_obj_add_state(favorite_item_button, LV_STATE_DISABLED);
  lv_obj_add_event_cb(favorite_item_button, toolbar_button_event, LV_EVENT_CLICKED,
                      (void *)(intptr_t)-5);
  lv_obj_t *star_label = create_fa_label(favorite_item_button, 0xf005U, lv_color_hex(COL_TEXT));
  lv_obj_center(star_label);

  const bool archive_source = g_source == SOURCE_ARCHIVE;
  g_archive_up_button = lv_button_create(header);
  g_toolbar_buttons[11] = g_archive_up_button;
  lv_obj_set_size(g_archive_up_button, button_w, button_h);
  lv_obj_set_pos(g_archive_up_button, ui_px(36), button_y);
  lv_obj_set_style_pad_all(g_archive_up_button, 0, LV_PART_MAIN);
  style_button(g_archive_up_button, g_toolbar_focus == 11);
  if (!archive_source) lv_obj_add_flag(g_archive_up_button, LV_OBJ_FLAG_HIDDEN);
  if (archive_source && g_archive_level == ARCHIVE_LEVEL_SEARCH)
    lv_obj_add_state(g_archive_up_button, LV_STATE_DISABLED);
  lv_obj_add_event_cb(g_archive_up_button, toolbar_button_event, LV_EVENT_CLICKED,
                      (void *)(intptr_t)-15);
  /* Draw a tray and up arrow in button-local coordinates. Some bundled FA
   * subsets omit upload glyphs, and rotated substitutes do not center
   * reliably across LVGL layouts. */
  const int icon_unit = 2;
  const int icon_cx = button_w / 2;
  const int icon_cy = button_h / 2;
  static lv_point_precise_t up_arrow_points[5];
  up_arrow_points[0] = (lv_point_precise_t){icon_cx - 5 * icon_unit, icon_cy - 2 * icon_unit};
  up_arrow_points[1] = (lv_point_precise_t){icon_cx, icon_cy - 7 * icon_unit};
  up_arrow_points[2] = (lv_point_precise_t){icon_cx + 5 * icon_unit, icon_cy - 2 * icon_unit};
  up_arrow_points[3] = (lv_point_precise_t){icon_cx, icon_cy - 7 * icon_unit};
  up_arrow_points[4] = (lv_point_precise_t){icon_cx, icon_cy + 3 * icon_unit};
  lv_obj_t *up_arrow = lv_line_create(g_archive_up_button);
  lv_line_set_points(up_arrow, up_arrow_points,
                     sizeof(up_arrow_points) / sizeof(up_arrow_points[0]));
  lv_obj_set_style_line_color(up_arrow, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_line_width(up_arrow, ui_px(1), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(up_arrow, true, LV_PART_MAIN);
  lv_obj_clear_flag(up_arrow, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  static lv_point_precise_t tray_points[4];
  tray_points[0] = (lv_point_precise_t){icon_cx - 6 * icon_unit, icon_cy + 1 * icon_unit};
  tray_points[1] = (lv_point_precise_t){icon_cx - 6 * icon_unit, icon_cy + 6 * icon_unit};
  tray_points[2] = (lv_point_precise_t){icon_cx + 6 * icon_unit, icon_cy + 6 * icon_unit};
  tray_points[3] = (lv_point_precise_t){icon_cx + 6 * icon_unit, icon_cy + 1 * icon_unit};
  lv_obj_t *up_tray = lv_line_create(g_archive_up_button);
  lv_line_set_points(up_tray, tray_points,
                     sizeof(tray_points) / sizeof(tray_points[0]));
  lv_obj_set_style_line_color(up_tray, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_line_width(up_tray, ui_px(1), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(up_tray, true, LV_PART_MAIN);
  lv_obj_clear_flag(up_tray, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  const int right_pad = ui_px(8);
  const int button_gap = ui_px(4);
  const int search_x = ui_px(36) + (archive_source ? button_w + button_gap : 0);
  const int count_w = ui_px(90);
  const int count_x = g_canvas_w - right_pad - count_w;
  const int refresh_x = count_x - button_gap - button_w;
  const int key_x = refresh_x - button_gap - button_w;
  const int clear_x = key_x - button_gap - button_w;
  const int search_width = clear_x - search_x - ui_px(4);
  g_search_area = lv_textarea_create(header);
  g_toolbar_buttons[7] = g_search_area;
  lv_obj_set_size(g_search_area, search_width > ui_px(48) ? search_width : ui_px(48), search_h);
  lv_obj_set_pos(g_search_area, search_x, search_y);
  lv_textarea_set_one_line(g_search_area, true);
  /* LVGL's one-line mode normalizes the textarea to its line height. Set the
   * intended control height again after enabling that mode so it remains
   * slightly taller than the compact icon buttons and gives the caret room. */
  lv_obj_set_height(g_search_area, search_h);
  lv_textarea_set_cursor_click_pos(g_search_area, true);
  lv_textarea_set_max_length(g_search_area, sizeof(g_search_text) - 1);
  lv_obj_set_scrollbar_mode(g_search_area, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_radius(g_search_area, 8, LV_PART_MAIN);
  lv_obj_set_style_border_width(g_search_area, 1, LV_PART_MAIN);
  /* LVGL's one-line textarea anchors the text to its upper inner edge. Add a
   * modest top inset so the text and caret sit near the middle of the taller
   * field instead of clinging to its top border. */
  lv_obj_set_style_pad_top(g_search_area, ui_px(7), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(g_search_area, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_left(g_search_area, ui_px(5), LV_PART_MAIN);
  lv_obj_set_style_pad_right(g_search_area, ui_px(5), LV_PART_MAIN);
  sync_search_context();
  lv_obj_set_style_bg_color(g_search_area, lv_color_hex(COL_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_search_area, (lv_opa_t)140, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_search_area, lv_color_hex(COL_MUTED), LV_PART_MAIN);
  lv_obj_set_style_text_color(g_search_area, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  /* Slightly smaller than the old header font leaves room for the caret and
   * keeps typed text vertically centered in the now-taller field. */
  lv_obj_set_style_text_font(g_search_area, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_add_event_cb(g_search_area, search_value_event, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(g_search_area, search_click_event, LV_EVENT_CLICKED, NULL);
  lv_obj_t *clear_button = lv_button_create(header);
  g_toolbar_buttons[8] = clear_button;
  lv_obj_set_size(clear_button, button_w, button_h);
  lv_obj_set_pos(clear_button, clear_x, button_y);
  lv_obj_set_style_pad_all(clear_button, 0, LV_PART_MAIN);
  style_button(clear_button, g_toolbar_focus == 8);
  lv_obj_add_event_cb(clear_button, toolbar_button_event, LV_EVENT_CLICKED, (void *)(intptr_t)-12);
  lv_obj_t *clear_label = lv_label_create(clear_button);
  set_fa_glyph(clear_label, 0xf00dU);
  lv_obj_set_style_text_color(clear_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_font(clear_label, &launcher_fa_icons, LV_PART_MAIN);
  lv_obj_center(clear_label);
  lv_obj_t *keyboard_button = lv_button_create(header);
  g_toolbar_buttons[9] = keyboard_button;
  lv_obj_set_size(keyboard_button, button_w, button_h);
  lv_obj_set_pos(keyboard_button, key_x, button_y);
  lv_obj_set_style_pad_all(keyboard_button, 0, LV_PART_MAIN);
  style_button(keyboard_button, g_keyboard_open);
  lv_obj_add_event_cb(keyboard_button, toolbar_button_event, LV_EVENT_CLICKED, (void *)(intptr_t)-8);
  lv_obj_t *keyboard_label = lv_label_create(keyboard_button);
  set_fa_glyph(keyboard_label, 0xf11cU);
  lv_obj_set_style_text_color(keyboard_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_font(keyboard_label, &launcher_fa_icons, LV_PART_MAIN);
  lv_obj_center(keyboard_label);

  /* Keep refresh next to the app count, as in the original launcher header. */
  lv_obj_t *refresh_button = lv_button_create(header);
  g_toolbar_buttons[10] = refresh_button;
  lv_obj_set_size(refresh_button, button_w, button_h);
  lv_obj_set_pos(refresh_button, refresh_x, button_y);
  lv_obj_set_style_pad_all(refresh_button, 0, LV_PART_MAIN);
  style_button(refresh_button, g_toolbar_focus == 10);
  lv_obj_add_event_cb(refresh_button, toolbar_button_event, LV_EVENT_CLICKED,
                      (void *)(intptr_t)-3);
  lv_obj_t *refresh_label = lv_label_create(refresh_button);
  set_fa_glyph(refresh_label, 0xf021U);
  lv_obj_set_style_text_color(refresh_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_font(refresh_label, &launcher_fa_icons, LV_PART_MAIN);
  lv_obj_center(refresh_label);

  lv_obj_t *count_cell = lv_obj_create(header);
  lv_obj_remove_style_all(count_cell);
  lv_obj_set_size(count_cell, count_w, button_h);
  lv_obj_set_pos(count_cell, count_x, button_y);
  lv_obj_clear_flag(count_cell, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *count = lv_label_create(count_cell);
  g_count_label = count;
  lv_label_set_text_fmt(count, "%d APPS", g_view_count);
  lv_obj_set_style_text_color(count, lv_color_hex(COL_OK), LV_PART_MAIN);
  lv_obj_set_style_text_font(count, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_long_mode(count, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(count, count_w);
  lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_align(count, LV_ALIGN_RIGHT_MID, 0, 0);

  if (g_settings_mode) {
    lv_obj_t *panel = lv_obj_create(screen);
    g_settings_panel = panel;
    const int panel_y = status_h + header_h + ui_px(7);
    const int panel_w = g_canvas_w - 2 * margin;
    const int panel_pad = ui_px(8);
    const int content_w = panel_w - 2 * panel_pad;
    const int keyboard_height = ui_px(100);
    const int keyboard_y = g_canvas_h - keyboard_height - ui_px(4);
    const int panel_bottom = g_keyboard_open ? keyboard_y - ui_px(4) : g_canvas_h - ui_px(10);
    lv_obj_set_pos(panel, margin, panel_y);
    lv_obj_set_size(panel, panel_w, panel_bottom - panel_y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, panel_pad, LV_PART_MAIN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_row(panel, ui_px(2), LV_PART_MAIN);
    lv_obj_t *heading = lv_label_create(panel);
    lv_label_set_text(heading, "SETTINGS");
    lv_obj_set_style_text_color(heading, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(heading, font, LV_PART_MAIN);
    lv_obj_set_pos(heading, ui_px(6), ui_px(1));
    lv_obj_set_width(heading, content_w - ui_px(110));
  g_settings_back_button = lv_button_create(panel);
    lv_obj_set_size(g_settings_back_button, ui_px(48), ui_px(17));
    lv_obj_align(g_settings_back_button, LV_ALIGN_TOP_RIGHT, -ui_px(6), ui_px(1));
    lv_obj_set_style_pad_all(g_settings_back_button, 0, LV_PART_MAIN);
    style_button(g_settings_back_button, g_settings_focus == 21);
    g_settings_focus_widgets[21] = g_settings_back_button;
    lv_obj_add_event_cb(g_settings_back_button, settings_focus_event, LV_EVENT_PRESSED,
                        (void *)(intptr_t)21);
    lv_obj_add_event_cb(g_settings_back_button, toolbar_button_event, LV_EVENT_CLICKED,
                        (void *)(intptr_t)-9);
    lv_obj_t *settings_back_label = create_fa_label(g_settings_back_button, 0xf053U,
                                                    lv_color_hex(COL_TEXT));
    lv_obj_center(settings_back_label);

    const int label_x = ui_px(8);
    const int control_x = content_w * 46 / 100;
    const int control_w = content_w - control_x - ui_px(8);
    const int row_h = ui_px(22);
    const int row_gap = ui_px(4);
    const int first_row_y = ui_px(24);
    const int step_y = row_h + row_gap;
    int row_y[48];
    for (int row = 0; row < 48; row++) {
      int visual_row = row;
      if (row < 20) visual_row += 5;
      else if (row < 25) visual_row -= 20;
      if(row>=30)visual_row=24+(row-30);
      else if(visual_row>=24)visual_row+=4;
      if(row==35)visual_row=32;
      else if(row==34)visual_row=33;
      else if(row==28 || row==29)visual_row+=2;
      if(row>=36)visual_row=row;
      if(row==29)visual_row=52;
      /* Network precedes Archive; callback IDs and the later rows stay stable. */
      if(visual_row>=17 && visual_row<=28)visual_row-=8;
      else if(visual_row>=9 && visual_row<=16)visual_row+=12;
      if(visual_row>=30)visual_row++; /* Room for Tooltips after App Details. */
      row_y[row] = first_row_y + step_y * visual_row;
      if(visual_row>=18)row_y[row]+=ui_px(18); /* Wi-Fi scan status. */
      if(visual_row>=29)row_y[row]+=ui_px(12); /* Archive S3 status height. */
    }

    row_y[47]=row_y[25]+step_y;

    settings_text(panel, "MASTER VOLUME", label_x, row_y[0],
                  control_x - label_x - ui_px(6), font, false);
    g_volume_value = lv_label_create(panel);
    lv_label_set_text_fmt(g_volume_value, "%d%%", g_volume_level);
    lv_obj_set_style_text_color(g_volume_value, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_volume_value, font, LV_PART_MAIN);
    lv_obj_set_width(g_volume_value, control_w);
    lv_obj_set_style_text_align(g_volume_value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(g_volume_value, LV_ALIGN_TOP_LEFT, control_x, row_y[0] - ui_px(1));
    g_volume_slider = lv_slider_create(panel);
    lv_obj_set_size(g_volume_slider, control_w, ui_px(7));
    lv_obj_set_pos(g_volume_slider, control_x, row_y[0] + row_h - ui_px(10));
    lv_slider_set_range(g_volume_slider, 0, 100);
    lv_slider_set_value(g_volume_slider, g_volume_level, LV_ANIM_OFF);
    lv_obj_add_event_cb(g_volume_slider, settings_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_volume_slider, settings_focus_event, LV_EVENT_PRESSED, (void *)(intptr_t)0);
    g_settings_focus_widgets[0] = g_volume_slider;

    settings_text(panel, "DISPLAY BRIGHTNESS", label_x, row_y[1],
                  control_x - label_x - ui_px(6), font, false);
    g_brightness_value = lv_label_create(panel);
    lv_label_set_text_fmt(g_brightness_value, "%d%%", g_brightness_level);
    lv_obj_set_style_text_color(g_brightness_value, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_brightness_value, font, LV_PART_MAIN);
    lv_obj_set_width(g_brightness_value, control_w);
    lv_obj_set_style_text_align(g_brightness_value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(g_brightness_value, LV_ALIGN_TOP_LEFT, control_x, row_y[1] - ui_px(1));
    g_brightness_slider = lv_slider_create(panel);
    lv_obj_set_size(g_brightness_slider, control_w, ui_px(7));
    lv_obj_set_pos(g_brightness_slider, control_x, row_y[1] + row_h - ui_px(10));
    lv_slider_set_range(g_brightness_slider, 5, 100);
    lv_slider_set_value(g_brightness_slider, g_brightness_level, LV_ANIM_OFF);
    lv_obj_add_event_cb(g_brightness_slider, settings_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_brightness_slider, settings_focus_event, LV_EVENT_PRESSED, (void *)(intptr_t)1);
    g_settings_focus_widgets[1] = g_brightness_slider;

    settings_text(panel, "AUTO BRIGHTNESS", label_x, row_y[2],
                  control_x - label_x - ui_px(6), font, false);
    g_auto_brightness_button = lv_button_create(panel);
    lv_obj_set_size(g_auto_brightness_button, ui_px(44), ui_px(18));
    lv_obj_set_pos(g_auto_brightness_button, control_x, row_y[2] + (row_h - ui_px(18)) / 2);
    style_button(g_auto_brightness_button, g_settings_focus == 2);
    lv_obj_add_event_cb(g_auto_brightness_button, auto_brightness_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_auto_brightness_button, settings_focus_event, LV_EVENT_PRESSED,
                        (void *)(intptr_t)2);
    g_auto_brightness_state_label = lv_label_create(g_auto_brightness_button);
    lv_label_set_text(g_auto_brightness_state_label, g_auto_brightness_enabled ? "ON" : "OFF");
    lv_obj_set_style_text_color(g_auto_brightness_state_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_auto_brightness_state_label, font, LV_PART_MAIN);
    lv_obj_center(g_auto_brightness_state_label);
    g_settings_focus_widgets[2] = g_auto_brightness_button;

    settings_text(panel, "BACKGROUND ART", label_x, row_y[3],
                  control_x - label_x - ui_px(6), font, false);
    g_background_dropdown = lv_dropdown_create(panel);
    lv_obj_set_size(g_background_dropdown, control_w, ui_px(18));
    lv_obj_set_pos(g_background_dropdown, control_x, row_y[3] + (row_h - ui_px(18)) / 2);
    lv_obj_set_style_bg_color(g_background_dropdown, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_background_dropdown, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_background_dropdown,
                                  lv_color_hex(g_settings_focus == 3 ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_background_dropdown, g_settings_focus == 3 ? 2 : 1, LV_PART_MAIN);
    lv_obj_set_style_radius(g_background_dropdown, 4, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_background_dropdown, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_background_dropdown, font, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_background_dropdown, lv_color_hex(COL_PANEL), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(g_background_dropdown, LV_OPA_70, LV_PART_ITEMS);
    lv_obj_set_style_border_color(g_background_dropdown, lv_color_hex(COL_MUTED), LV_PART_ITEMS);
    lv_obj_set_style_border_width(g_background_dropdown, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(g_background_dropdown, 4, LV_PART_ITEMS);
    lv_obj_set_style_text_color(g_background_dropdown, lv_color_hex(COL_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_opa(g_background_dropdown, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_font(g_background_dropdown, font, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(g_background_dropdown, lv_color_hex(COL_SELECTED), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(g_background_dropdown, LV_OPA_80, LV_PART_SELECTED);
    lv_obj_set_style_text_color(g_background_dropdown, lv_color_hex(COL_TEXT), LV_PART_SELECTED);
    lv_obj_set_style_text_opa(g_background_dropdown, LV_OPA_COVER, LV_PART_SELECTED);
    lv_dropdown_set_options(g_background_dropdown, g_background_options);
    lv_dropdown_set_selected(g_background_dropdown, (uint32_t)(g_background_index + 1));
    lv_dropdown_set_dir(g_background_dropdown, LV_DIR_TOP);
    lv_dropdown_set_text_static(g_background_dropdown, "");
    g_background_value_label = lv_label_create(g_background_dropdown);
    lv_label_set_long_mode(g_background_value_label,
        g_settings_focus == 3 ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP);
    lv_obj_set_width(g_background_value_label, control_w - ui_px(28));
    lv_obj_set_style_text_color(g_background_value_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_background_value_label, font, LV_PART_MAIN);
    lv_obj_align(g_background_value_label, LV_ALIGN_LEFT_MID, ui_px(5), 0);
    lv_obj_add_event_cb(g_background_dropdown, background_dropdown_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_background_dropdown, background_dropdown_event, LV_EVENT_CLICKED, NULL);
    g_settings_focus_widgets[3] = g_background_dropdown;
    g_background_choice = g_background_index + 1;
    if (g_background_dropdown_open)
      lv_dropdown_open(g_background_dropdown);

    settings_text(panel, "ARCHIVE COLLECTION", label_x, row_y[4],
                  control_x - label_x - ui_px(6), font, false);
    g_archive_collection_button = settings_button(panel, 4, control_x, row_y[4],
                                                   control_w, ui_px(18), -13);
    g_archive_collection_value_label = lv_label_create(g_archive_collection_button);
    lv_label_set_text(g_archive_collection_value_label, g_archive_collection);
    lv_label_set_long_mode(g_archive_collection_value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_archive_collection_value_label, control_w - ui_px(10));
    lv_obj_set_style_text_color(g_archive_collection_value_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_archive_collection_value_label, font, LV_PART_MAIN);
    lv_obj_align(g_archive_collection_value_label, LV_ALIGN_LEFT_MID, 0, 0);

    settings_text(panel, "ARCHIVE EMAIL", label_x, row_y[5],
                  control_x - label_x - ui_px(6), font, false);
    lv_obj_t *archive_email_button = settings_button(panel, 5, control_x, row_y[5],
                                                     control_w, ui_px(18), -40);
    g_archive_email_value_label = lv_label_create(archive_email_button);
    lv_label_set_text(g_archive_email_value_label,
                      g_archive_email[0] != '\0' ? g_archive_email : "Select to enter email");
    lv_label_set_long_mode(g_archive_email_value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_archive_email_value_label, control_w - ui_px(10));
    lv_obj_set_style_text_color(g_archive_email_value_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_archive_email_value_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_archive_email_value_label, LV_ALIGN_LEFT_MID, 0, 0);

    settings_text(panel, "ARCHIVE PASSWORD", label_x, row_y[6],
                  control_x - label_x - ui_px(6), font, false);
    lv_obj_t *archive_password_button = settings_button(panel, 6, control_x, row_y[6],
                                                        control_w, ui_px(18), -41);
    g_archive_password_value_label = lv_label_create(archive_password_button);
    lv_label_set_text(g_archive_password_value_label,
                      g_archive_password[0] != '\0' ? "********" : "Select to enter password");
    lv_obj_set_style_text_color(g_archive_password_value_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_archive_password_value_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_archive_password_value_label, LV_ALIGN_LEFT_MID, 0, 0);

    settings_text(panel, "SESSION", label_x, row_y[7],
                  control_x - label_x - ui_px(6), font, false);
    g_archive_auth_status_label = settings_text(panel, g_archive_auth_status, control_x,
                                                 row_y[7], control_w, &lv_font_montserrat_14, true);

    settings_text(panel, "ARCHIVE ACCOUNT", label_x, row_y[8],
                  control_x - label_x - ui_px(6), font, false);
    g_archive_login_button = settings_button(panel, 7, control_x, row_y[8],
                                              control_w, ui_px(18), -42);
    g_archive_login_button_label = lv_label_create(g_archive_login_button);
    lv_label_set_text(g_archive_login_button_label, g_archive_authenticated ? "SIGN OUT" : "SIGN IN");
    lv_obj_set_style_text_color(g_archive_login_button_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_archive_login_button_label, font, LV_PART_MAIN);
    lv_obj_center(g_archive_login_button_label);

    settings_text(panel, "IA S3 ACCESS KEY", label_x, row_y[9],
                  control_x - label_x - ui_px(6), font, false);
    const int key_eye_gap = ui_px(3);
    const int key_eye_size = ui_px(18);
    const int key_field_w = control_w - key_eye_size - key_eye_gap;
    lv_obj_t *archive_s3_access_button = settings_button(panel, 8, control_x, row_y[9],
                                                         key_field_w, ui_px(18), -44);
    g_archive_s3_access_value_label = lv_label_create(archive_s3_access_button);
    lv_obj_set_width(g_archive_s3_access_value_label, key_field_w - ui_px(10));
    lv_obj_set_style_text_color(g_archive_s3_access_value_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_archive_s3_access_value_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_archive_s3_access_value_label, LV_ALIGN_LEFT_MID, 0, 0);
    (void)create_archive_s3_visibility_button(panel,
        control_x + key_field_w + key_eye_gap, row_y[9], 0);

    settings_text(panel, "IA S3 SECRET KEY", label_x, row_y[10],
                  control_x - label_x - ui_px(6), font, false);
    lv_obj_t *archive_s3_secret_button = settings_button(panel, 9, control_x, row_y[10],
                                                         key_field_w, ui_px(18), -45);
    g_archive_s3_secret_value_label = lv_label_create(archive_s3_secret_button);
    lv_obj_set_width(g_archive_s3_secret_value_label, key_field_w - ui_px(10));
    lv_obj_set_style_text_color(g_archive_s3_secret_value_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_archive_s3_secret_value_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_archive_s3_secret_value_label, LV_ALIGN_LEFT_MID, 0, 0);
    (void)create_archive_s3_visibility_button(panel,
        control_x + key_field_w + key_eye_gap, row_y[10], 1);

    lv_obj_t *s3_status_title = settings_text(panel, "S3 STATUS (PLAIN-TEXT SD)", label_x, row_y[11],
                                              control_x - label_x - ui_px(6), font, false);
    lv_obj_set_pos(s3_status_title, label_x,
                   row_y[11] + (ui_px(34) - ui_px(18)) / 2);
    const int key_action_gap = ui_px(4);
    const int key_status_w = control_w * 38 / 100;
    const int key_action_w = (control_w - key_status_w - key_action_gap * 2) / 2;
    g_archive_s3_status_label = settings_text(panel, g_archive_s3_status,
        control_x, row_y[11], key_status_w,
        &lv_font_montserrat_14, true);
    lv_label_set_long_mode(g_archive_s3_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(g_archive_s3_status_label, ui_px(34));
    lv_obj_set_pos(g_archive_s3_status_label, control_x, row_y[11]);
    lv_obj_t *check_keys_button = settings_button(panel, 10, control_x + key_status_w + key_action_gap,
        row_y[11] + ui_px(7), key_action_w, ui_px(18), -46);
    lv_obj_t *check_keys_label = lv_label_create(check_keys_button);
    lv_label_set_text(check_keys_label, "CHECK");
    lv_obj_set_style_text_color(check_keys_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(check_keys_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(check_keys_label);
    lv_obj_t *clear_keys_button = settings_button(panel, 11, control_x + key_status_w + key_action_gap * 2 + key_action_w,
        row_y[11] + ui_px(7), key_action_w, ui_px(18), -47);
    lv_obj_t *clear_keys_label = lv_label_create(clear_keys_button);
    lv_label_set_text(clear_keys_label, "CLEAR");
    lv_obj_set_style_text_color(clear_keys_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(clear_keys_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(clear_keys_label);

    lv_obj_t *network_heading = lv_label_create(panel);
    lv_label_set_text(network_heading, "NETWORK");
    lv_obj_set_style_text_color(network_heading, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(network_heading, font, LV_PART_MAIN);
    lv_obj_set_pos(network_heading, label_x, row_y[12]);
    g_network_status_label = settings_text(panel, "", label_x, row_y[13], content_w-label_x-ui_px(8), &lv_font_montserrat_14, true);

    settings_text(panel, "DHCP", label_x, row_y[14], control_x - label_x - ui_px(6), font, false);
    g_network_dhcp_button = settings_button(panel, 12, control_x, row_y[14], control_w, ui_px(18), -34);
    g_network_dhcp_label = lv_label_create(g_network_dhcp_button);
    lv_obj_set_style_text_color(g_network_dhcp_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_network_dhcp_label, font, LV_PART_MAIN);
    lv_obj_center(g_network_dhcp_label);
    g_settings_focus_widgets[12] = g_network_dhcp_button;

    static const char *const network_field_names[4] = {"IP ADDRESS", "SUBNET MASK", "GATEWAY", "DNS SERVER"};
    static const int network_field_actions[4] = {-30, -31, -32, -33};
    for (int field = 0; field < 4; field++) {
      const int row = 15 + field;
      settings_text(panel, network_field_names[field], label_x, row_y[row],
                    control_x - label_x - ui_px(6), font, false);
      lv_obj_t *button = settings_button(panel, 13 + field, control_x, row_y[row],
                                         control_w, ui_px(18), network_field_actions[field]);
      g_network_value_labels[field] = lv_label_create(button);
      lv_obj_set_style_text_color(g_network_value_labels[field], lv_color_hex(COL_TEXT), LV_PART_MAIN);
      lv_obj_set_style_text_font(g_network_value_labels[field], &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_align(g_network_value_labels[field], LV_ALIGN_LEFT_MID, 0, 0);
    }
    const char *extra_names[4]={"PAX SUPPLY SERVER","WI-FI NAME","WI-FI PASSWORD","CHANGE WI-FI"};
    for(int item=0;item<4;item++){
      settings_text(panel,extra_names[item],label_x,row_y[30+item],control_x-label_x-ui_px(6),font,false);
      lv_obj_t *button=settings_button(panel,25+item,control_x,row_y[30+item],item==1 ? control_w/2-ui_px(2) : control_w,ui_px(18),-62-item);
      g_network_extra_labels[item]=lv_label_create(button);
      lv_obj_set_width(g_network_extra_labels[item],(item==1 ? control_w/2 : control_w)-ui_px(12));
      lv_label_set_long_mode(g_network_extra_labels[item],LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_font(g_network_extra_labels[item],&lv_font_montserrat_14,LV_PART_MAIN);
      lv_obj_set_style_text_color(g_network_extra_labels[item],lv_color_hex(COL_TEXT),LV_PART_MAIN);
      lv_obj_center(g_network_extra_labels[item]);
    }
    g_wifi_dropdown=lv_dropdown_create(panel);
    lv_obj_set_pos(g_wifi_dropdown,control_x+control_w/2+ui_px(2),row_y[31]+(ui_px(22)-ui_px(18))/2);
    lv_obj_set_size(g_wifi_dropdown,control_w/2-ui_px(2),ui_px(18));
    lv_obj_set_style_pad_all(g_wifi_dropdown,ui_px(2),LV_PART_MAIN);
    lv_obj_set_style_text_font(g_wifi_dropdown,&lv_font_montserrat_14,LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_wifi_dropdown,lv_color_hex(COL_PANEL),LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wifi_dropdown,lv_color_hex(COL_TEXT),LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_wifi_dropdown,lv_color_hex(COL_PANEL),LV_PART_ITEMS);
    lv_obj_set_style_text_color(g_wifi_dropdown,lv_color_hex(COL_TEXT),LV_PART_ITEMS);
    lv_obj_set_style_text_font(g_wifi_dropdown,&lv_font_montserrat_14,LV_PART_ITEMS);
    lv_obj_set_style_bg_color(g_wifi_dropdown,lv_color_hex(COL_SELECTED),LV_PART_SELECTED);
    lv_dropdown_set_dir(g_wifi_dropdown,LV_DIR_TOP);
    g_settings_focus_widgets[36]=g_wifi_dropdown;
    lv_obj_add_event_cb(g_wifi_dropdown,wifi_dropdown_event,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(g_wifi_dropdown,wifi_dropdown_event,LV_EVENT_READY,NULL);
    lv_obj_add_event_cb(g_wifi_dropdown,wifi_dropdown_event,LV_EVENT_CANCEL,NULL);
    lv_obj_add_event_cb(g_wifi_dropdown,wifi_dropdown_event,LV_EVENT_VALUE_CHANGED,NULL);
    g_wifi_scan_message_label=lv_label_create(panel);
    lv_obj_set_pos(g_wifi_scan_message_label,control_x,row_y[31]+ui_px(22));
    lv_obj_set_width(g_wifi_scan_message_label,control_w);
    lv_label_set_long_mode(g_wifi_scan_message_label,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_wifi_scan_message_label,&lv_font_montserrat_14,LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wifi_scan_message_label,lv_color_hex(COL_MUTED),LV_PART_MAIN);
    (void)update_wifi_picker_on_host(NULL);
    settings_text(panel, "SAVE NETWORK SETTINGS", label_x, row_y[19],
                  control_x - label_x - ui_px(6), font, false);
    g_network_apply_button = settings_button(panel, 17, control_x, row_y[19],
                                              control_w, ui_px(18), -35);
    lv_obj_t *apply_label = lv_label_create(g_network_apply_button);
    lv_label_set_text(apply_label, "APPLY");
    lv_obj_set_style_text_color(apply_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(apply_label, font, LV_PART_MAIN);
    lv_obj_center(apply_label);

    lv_obj_t *storage_heading = lv_label_create(panel);
    lv_label_set_text(storage_heading, "STORAGE");
    lv_obj_set_style_text_color(storage_heading, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(storage_heading, font, LV_PART_MAIN);
    lv_obj_set_pos(storage_heading, label_x, row_y[20]);
    settings_text(panel, "SD CARD (/sd)", label_x, row_y[21], control_x - label_x - ui_px(6), font, false);
    g_storage_status_labels[PAX_STORAGE_SD] = settings_text(panel, "", control_x, row_y[21],
                                                              control_w, &lv_font_montserrat_14, true);
    settings_text(panel, "SD CARD ACCESS", label_x, row_y[22], control_x - label_x - ui_px(6), font, false);
    g_storage_buttons[PAX_STORAGE_SD] = settings_button(panel, 18, control_x, row_y[22],
                                                          control_w, ui_px(18), -36);
    g_storage_button_labels[PAX_STORAGE_SD] = lv_label_create(g_storage_buttons[PAX_STORAGE_SD]);
    lv_obj_set_style_text_color(g_storage_button_labels[PAX_STORAGE_SD], lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_storage_button_labels[PAX_STORAGE_SD], font, LV_PART_MAIN);
    lv_obj_center(g_storage_button_labels[PAX_STORAGE_SD]);
    settings_text(panel, "USB STORAGE (/usb0)", label_x, row_y[23], control_x - label_x - ui_px(6), font, false);
    g_storage_status_labels[PAX_STORAGE_USB] = settings_text(panel, "", control_x, row_y[23],
                                                               control_w, &lv_font_montserrat_14, true);
    settings_text(panel, "USB STORAGE ACCESS", label_x, row_y[24], control_x - label_x - ui_px(6), font, false);
    g_storage_buttons[PAX_STORAGE_USB] = settings_button(panel, 19, control_x, row_y[24],
                                                           control_w, ui_px(18), -37);
    g_storage_button_labels[PAX_STORAGE_USB] = lv_label_create(g_storage_buttons[PAX_STORAGE_USB]);
    lv_obj_set_style_text_color(g_storage_button_labels[PAX_STORAGE_USB], lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_storage_button_labels[PAX_STORAGE_USB], font, LV_PART_MAIN);
    lv_obj_center(g_storage_button_labels[PAX_STORAGE_USB]);
    settings_text(panel, "APP DETAILS ON SELECT", label_x, row_y[25],
                  control_x - label_x - ui_px(6), font, false);
    g_app_details_switch = lv_switch_create(panel);
    lv_obj_set_size(g_app_details_switch, ui_px(44), ui_px(18));
    lv_obj_set_pos(g_app_details_switch, control_x, row_y[25] + (row_h - ui_px(18)) / 2);
    if (g_app_details_enabled) lv_obj_add_state(g_app_details_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(g_app_details_switch, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(g_app_details_switch, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_app_details_switch, lv_color_hex(COL_OK), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(g_app_details_switch, lv_color_hex(COL_MUTED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_app_details_switch, lv_color_hex(COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_border_color(g_app_details_switch,
        lv_color_hex(g_settings_focus == 20 ? COL_ACCENT : COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_app_details_switch, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(g_app_details_switch, settings_focus_event, LV_EVENT_PRESSED,
                        (void *)(intptr_t)20);
    lv_obj_add_event_cb(g_app_details_switch, app_details_switch_event, LV_EVENT_VALUE_CHANGED, NULL);
    g_settings_focus_widgets[20] = g_app_details_switch;
    settings_text(panel,"TOOLTIPS",label_x,row_y[47],control_x-label_x-ui_px(6),font,false);
    lv_obj_t *tips=settings_button(panel,37,control_x,row_y[47],control_w,ui_px(18),-75);
    g_tooltips_value=lv_label_create(tips);
    lv_label_set_text(g_tooltips_value,g_tooltips_enabled ? "ON" : "OFF");
    lv_obj_set_style_text_font(g_tooltips_value,font,LV_PART_MAIN);
    lv_obj_set_style_text_color(g_tooltips_value,lv_color_hex(COL_TEXT),LV_PART_MAIN);
    lv_obj_center(g_tooltips_value);
    const char *list_options[2]={"HIDE .PAX EXTENSION","FILE DETAILS IN LIST"};
    for(int option=0;option<2;option++){
      settings_text(panel,list_options[option],label_x,row_y[26+option],control_x-label_x-ui_px(6),font,false);
      lv_obj_t *toggle=lv_switch_create(panel);g_list_switches[option]=toggle;
      lv_obj_set_size(toggle,ui_px(44),ui_px(18));
      lv_obj_set_pos(toggle,control_x,row_y[26+option]+(row_h-ui_px(18))/2);
      if(option==0 ? g_hide_pax_extension : g_list_file_details)lv_obj_add_state(toggle,LV_STATE_CHECKED);
      lv_obj_set_style_bg_color(toggle,lv_color_hex(COL_PANEL),LV_PART_MAIN);
      lv_obj_set_style_bg_color(toggle,lv_color_hex(COL_OK),LV_PART_INDICATOR|LV_STATE_CHECKED);
      lv_obj_set_style_bg_color(toggle,lv_color_hex(COL_MUTED),LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(toggle,lv_color_hex(COL_TEXT),LV_PART_KNOB);
      lv_obj_set_style_border_width(toggle,1,LV_PART_MAIN);
      lv_obj_set_style_border_color(toggle,lv_color_hex(g_settings_focus==22+option ? COL_ACCENT : COL_MUTED),LV_PART_MAIN);
      lv_obj_add_event_cb(toggle,settings_focus_event,LV_EVENT_PRESSED,(void *)(intptr_t)(22+option));
      lv_obj_add_event_cb(toggle,list_switch_event,LV_EVENT_VALUE_CHANGED,(void *)(intptr_t)option);
      g_settings_focus_widgets[22+option]=toggle;
    }
    settings_text(panel,"FILE SIZE UNITS",label_x,row_y[35],control_x-label_x-ui_px(6),font,false);
    lv_obj_t *size_button=settings_button(panel,30,control_x,row_y[35],control_w,ui_px(18),-68);
    g_size_value_label=lv_label_create(size_button);lv_label_set_text(g_size_value_label,pax_size_unit_name(g_size_unit));
    lv_obj_set_style_text_color(g_size_value_label,lv_color_hex(COL_TEXT),LV_PART_MAIN);
    lv_obj_set_style_text_font(g_size_value_label,font,LV_PART_MAIN);lv_obj_center(g_size_value_label);
    settings_text(panel,"LIST SORT",label_x,row_y[34],control_x-label_x-ui_px(6),font,false);
    lv_obj_t *sort_button=settings_button(panel,29,control_x,row_y[34],control_w,ui_px(18),-67);
    g_sort_value_label=lv_label_create(sort_button);lv_label_set_text(g_sort_value_label,pax_sort_name(g_sort_mode));
    lv_obj_set_style_text_color(g_sort_value_label,lv_color_hex(COL_TEXT),LV_PART_MAIN);
    lv_obj_set_style_text_font(g_sort_value_label,font,LV_PART_MAIN);lv_obj_center(g_sort_value_label);
    settings_text(panel,"THEME",label_x,row_y[28],control_x-label_x-ui_px(6),font,false);
    lv_obj_t *theme_button=settings_button(panel,24,control_x,row_y[28],control_w,ui_px(18),-49);
    g_theme_value_label=lv_label_create(theme_button);lv_label_set_text(g_theme_value_label,g_theme.name);
    lv_obj_set_style_text_color(g_theme_value_label,lv_color_hex(COL_TEXT),LV_PART_MAIN);
    lv_obj_set_style_text_font(g_theme_value_label,font,LV_PART_MAIN);lv_obj_center(g_theme_value_label);
    const char *c6_names[3]={"C6 APPROVED SOURCE","VERIFY C6 FIRMWARE","INSTALL VERIFIED C6"};
    for(int i=0;i<3;i++){
      settings_text(panel,c6_names[i],label_x,row_y[36+i],control_x-label_x-ui_px(6),font,false);
      lv_obj_t *button=settings_button(panel,31+i,control_x,row_y[36+i],control_w,ui_px(18),-70-i);
      g_c6_labels[i]=lv_label_create(button);
      lv_label_set_text(g_c6_labels[i],i==0 ? g_c6_source : i==1 ? "CHECK FIRMWARE" : "INSTALL & RESTART");
      lv_obj_set_width(g_c6_labels[i],control_w-ui_px(12));lv_label_set_long_mode(g_c6_labels[i],LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_font(g_c6_labels[i],&lv_font_montserrat_14,LV_PART_MAIN);lv_obj_center(g_c6_labels[i]);
    }
    g_c6_progress=lv_bar_create(panel);
    lv_obj_set_pos(g_c6_progress,label_x,row_y[39]+ui_px(6));
    lv_obj_set_size(g_c6_progress,content_w-label_x-ui_px(8),ui_px(10));
    lv_bar_set_range(g_c6_progress,0,100);
    lv_obj_set_style_bg_color(g_c6_progress,lv_color_hex(COL_PANEL),LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_c6_progress,lv_color_hex(COL_ACCENT),LV_PART_INDICATOR);
    g_c6_labels[3]=settings_text(panel,"C6 firmware: board-recommended image is selected automatically",label_x,row_y[39]+step_y,content_w-label_x-ui_px(8),&lv_font_montserrat_14,true);
    lv_obj_set_height(g_c6_labels[3],ui_px(48));
    lv_label_set_long_mode(g_c6_labels[3],LV_LABEL_LONG_WRAP);
    settings_text(panel,"P4 FIRMWARE (GITHUB)",label_x,row_y[43],content_w-label_x-ui_px(8),font,false);
    for(int i=0;i<2;i++){
      settings_text(panel,i==0 ? "CHECK THIS DEVICE" : "UPDATE & RESTART",label_x,row_y[44+i],control_x-label_x-ui_px(6),font,false);
      lv_obj_t *button=settings_button(panel,34+i,control_x,row_y[44+i],control_w,ui_px(18),-73-i);
      g_firmware_labels[i]=lv_label_create(button);
      lv_obj_set_width(g_firmware_labels[i],control_w-ui_px(12));lv_label_set_long_mode(g_firmware_labels[i],LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_font(g_firmware_labels[i],&lv_font_montserrat_14,LV_PART_MAIN);lv_obj_center(g_firmware_labels[i]);
    }
    g_firmware_labels[2]=settings_text(panel,"",label_x,row_y[46],content_w-label_x-ui_px(8),&lv_font_montserrat_14,true);
    lv_obj_set_height(g_firmware_labels[2],ui_px(112));lv_label_set_long_mode(g_firmware_labels[2],LV_LABEL_LONG_WRAP);
    g_settings_message_label = settings_text(panel,g_settings_message,label_x,row_y[29],
                                              content_w-ui_px(12),&lv_font_montserrat_14,true);

    ensure_keyboard(screen);
    g_footer_label=lv_label_create(screen);
    lv_label_set_text(g_footer_label,"SETTINGS  A EDIT/SELECT  LEFT/RIGHT ADJUST  B BACK");
    lv_obj_set_style_text_color(g_footer_label,lv_color_hex(COL_MUTED),LV_PART_MAIN);
    lv_obj_align(g_footer_label,LV_ALIGN_BOTTOM_MID,0,-ui_px(4));
    /* This branch returns before the regular list-page refresh. Populate the
     * settings controls now so LVGL's default label text ('Text') is never
     * shown, including immediately after an SD eject/rebuild. */
    (void)update_selection_on_host(NULL);
    lv_obj_invalidate(screen);
    tooltip_configure();
    return 0;
  }

  if (g_detail_index >= 0) {
    const int detail_y = status_h + header_h + ui_px(2);
    const int detail_h = g_canvas_h - detail_y - status_h - ui_px(6);
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_pos(panel, margin, detail_y);
    lv_obj_set_size(panel, g_canvas_w - 2 * margin, detail_h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, ui_px(10), LV_PART_MAIN);
    /* The panel is only a frame. Keep it out of pointer hit-testing so that
     * its children and the fixed header are the actual touch targets. */
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    const int cover_size = ui_px(28);
    const int action_width = ui_px(72);
    const int action_x = ui_px(4);
    const int cover_x = (action_width - cover_size) / 2;
    const int cover_y = 0;
    const int info_x = ui_px(80);
    const int info_width = g_canvas_w - 2 * margin - info_x - ui_px(14);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, g_detail_title[0] != '\0' ? g_detail_title : g_detail_name);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, font, LV_PART_MAIN);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(title, info_x, ui_px(0));
    lv_obj_set_width(title, info_width);

    lv_obj_t *version_label = lv_label_create(panel);
    lv_label_set_text(version_label, "");
    lv_obj_set_style_text_color(version_label, lv_color_hex(COL_OK), LV_PART_MAIN);
    lv_obj_set_style_text_font(version_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(version_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(version_label, info_width);

    lv_obj_t *metadata_label = lv_label_create(panel);
    lv_label_set_text(metadata_label, "");
    lv_obj_set_style_text_color(metadata_label, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(metadata_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(metadata_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(metadata_label, info_width);

    if (g_detail_cover_pixels != NULL) {
      lv_obj_t *cover_image = lv_image_create(panel);
      lv_image_set_src(cover_image, &g_detail_cover_descriptor);
      lv_image_set_antialias(cover_image, false);
      lv_obj_set_pos(cover_image, cover_x, cover_y);
    } else {
      lv_obj_t *placeholder = lv_obj_create(panel);
      lv_obj_set_size(placeholder, cover_size, cover_size);
      lv_obj_set_pos(placeholder, cover_x, cover_y);
      lv_obj_set_style_bg_color(placeholder, lv_color_hex(COL_PANEL), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(placeholder, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(placeholder, lv_color_hex(COL_MUTED), LV_PART_MAIN);
      lv_obj_t *no_art = lv_label_create(placeholder);
      lv_label_set_text(no_art, "NO ART");
      lv_obj_set_style_text_color(no_art, lv_color_hex(COL_MUTED), LV_PART_MAIN);
      lv_obj_set_style_text_font(no_art, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_center(no_art);
    }

    const int action_height = ui_px(17);
    const int action_gap = ui_px(3);
    const int action_y = cover_size + ui_px(8);
    const int status_y = detail_h - ui_px(34);
    const int progress_y = status_y + ui_px(17);
    char version_text[96];
    char description_text[DETAIL_TEXT_BYTES];
    char metadata_text[256];
    if (g_detail_version[0] != '\0') {
      copy_text(version_text, sizeof(version_text), g_detail_is_remote ? "STORE " : "VERSION ");
      append_text(version_text, sizeof(version_text), g_detail_version);
      if (g_detail_is_remote && g_detail_is_installed && g_detail_local_version[0] != '\0') {
        append_text(version_text, sizeof(version_text), "  |  LOCAL ");
        append_text(version_text, sizeof(version_text), g_detail_local_version);
        append_text(version_text, sizeof(version_text),
                    g_detail_update_available ? "  |  UPDATE AVAILABLE" : "  |  CURRENT");
      } else if (g_detail_size > 0) {
        append_text(version_text, sizeof(version_text), "  |  ");
        append_uint(version_text, sizeof(version_text), g_detail_size / 1024U);
        append_text(version_text, sizeof(version_text), " KB");
      }
    } else {
      copy_text(version_text, sizeof(version_text), g_detail_is_installed ?
                "INSTALLED APP" : "NETWORK APP");
      if (g_detail_size > 0) {
        append_text(version_text, sizeof(version_text), "  |  ");
        append_uint(version_text, sizeof(version_text), g_detail_size / 1024U);
        append_text(version_text, sizeof(version_text), " KB");
      }
    }
    copy_text(description_text, sizeof(description_text), g_detail_description[0] != '\0' ?
              g_detail_description : "No description was provided for this app.");
    copy_text(metadata_text, sizeof(metadata_text), g_detail_category);
    if (g_detail_author[0] != '\0') {
      if (metadata_text[0] != '\0') append_text(metadata_text, sizeof(metadata_text), "  |  ");
      append_text(metadata_text, sizeof(metadata_text), g_detail_author);
    }
    lv_label_set_text(version_label, version_text);
    lv_label_set_text(metadata_label, metadata_text[0] != '\0' ? metadata_text : "App information");

    /* Wrap long titles, version/update status and author credits instead of
     * letting any header line run past the right edge. Measure the wrapped
     * rows so the scrollable details start below the complete header block. */
    lv_obj_update_layout(title);
    int header_y = lv_obj_get_height(title) + ui_px(1);
    lv_obj_set_pos(version_label, info_x, header_y);
    lv_obj_update_layout(version_label);
    header_y += lv_obj_get_height(version_label) + ui_px(1);
    lv_obj_set_pos(metadata_label, info_x, header_y);
    lv_obj_update_layout(metadata_label);
    const int info_y = header_y + lv_obj_get_height(metadata_label) + ui_px(6);

    lv_obj_t *info_view = lv_obj_create(panel);
    g_detail_info_view = info_view;
    lv_obj_remove_style_all(info_view);
    lv_obj_set_pos(info_view, info_x, info_y);
    lv_obj_set_size(info_view, info_width,
                    /* Keep status/progress clear while using nearly all the
                     * right column as the scrollable information document. */
                    status_y - info_y - ui_px(12));
    lv_obj_set_style_bg_opa(info_view, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(info_view, 0, LV_PART_MAIN);
    /* Make the detail text independently touch-scrollable. The panel itself
     * intentionally stays fixed so its header, artwork, and action buttons
     * remain on-screen while long descriptions move underneath. */
    lv_obj_add_flag(info_view, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(info_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(info_view, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(info_view, detail_info_scroll_event, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(info_view, detail_info_scroll_event, LV_EVENT_SCROLL_END, NULL);
    lv_obj_t *about_heading = lv_label_create(info_view);
    lv_label_set_text(about_heading, "ABOUT");
    lv_obj_set_style_text_color(about_heading, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(about_heading, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_clear_flag(about_heading, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *info_label = lv_label_create(info_view);
    lv_label_set_text(info_label, g_detail_description[0] != '\0' ?
                      g_detail_description : "No description was provided for this app.");
    lv_label_set_long_mode(info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info_label, info_width - ui_px(6));
    lv_obj_set_style_text_color(info_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_14, LV_PART_MAIN);
    /* The scroll container owns pointer gestures; keep its wrapped label
     * passive so a press on the text cannot be mistaken for a child action. */
    lv_obj_clear_flag(info_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(info_label);
    int content_y = lv_obj_get_height(about_heading) + ui_px(4);
    lv_obj_set_pos(info_label, 0, content_y);
    content_y += lv_obj_get_height(info_label) + ui_px(9);

    lv_obj_t *package_info=lv_label_create(info_view);
    lv_label_set_text(package_info,g_detail_file_info);
    lv_label_set_long_mode(package_info,LV_LABEL_LONG_WRAP);
    lv_obj_set_width(package_info,info_width-ui_px(6));
    lv_obj_set_style_text_color(package_info,lv_color_hex(COL_MUTED),LV_PART_MAIN);
    lv_obj_set_style_text_font(package_info,&lv_font_montserrat_14,LV_PART_MAIN);
    lv_obj_set_pos(package_info,0,content_y);
    lv_obj_clear_flag(package_info,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(package_info);content_y+=lv_obj_get_height(package_info)+ui_px(9);

    lv_obj_t *files_heading = lv_label_create(info_view);
    lv_label_set_text(files_heading, "FILES ON THE CARD");
    lv_obj_set_style_text_color(files_heading, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(files_heading, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(files_heading, 0, content_y);
    lv_obj_clear_flag(files_heading, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(files_heading);
    content_y += lv_obj_get_height(files_heading) + ui_px(3);
    if (g_detail_file_count == 0) {
      lv_obj_t *no_files = lv_label_create(info_view);
      lv_label_set_text(no_files, "No card files are required by this app.");
      lv_obj_set_style_text_color(no_files, lv_color_hex(COL_OK), LV_PART_MAIN);
      lv_obj_set_style_text_font(no_files, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_pos(no_files, 0, content_y);
      lv_obj_clear_flag(no_files, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_update_layout(no_files);
      content_y += lv_obj_get_height(no_files) + ui_px(6);
    } else {
      for (int i = 0; i < g_detail_file_count; i++) {
        const detail_file_t *required = &g_detail_files[i];
        const char *state = required->found ? "FOUND" :
                            required->download ? "NOT DOWNLOADED" :
                            required->optional ? "OPTIONAL - NOT FOUND" : "MISSING REQUIRED FILE";
        const uint32_t color = required->found ? COL_OK :
                               (required->optional || required->download) ? COL_WARN : COL_ERROR;
        lv_obj_t *file_state = lv_label_create(info_view);
        g_detail_file_state_labels[i] = file_state;
        lv_label_set_text(file_state, state);
        lv_obj_set_style_text_color(file_state, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_text_font(file_state, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_pos(file_state, 0, content_y);
        lv_obj_clear_flag(file_state, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_update_layout(file_state);
        content_y += lv_obj_get_height(file_state) + ui_px(1);

        lv_obj_t *file_path = lv_label_create(info_view);
        lv_label_set_text(file_path, required->path);
        lv_label_set_long_mode(file_path, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(file_path, info_width - ui_px(6));
        lv_obj_set_style_text_color(file_path, lv_color_hex(COL_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(file_path, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_pos(file_path, 0, content_y);
        lv_obj_clear_flag(file_path, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_update_layout(file_path);
        content_y += lv_obj_get_height(file_path) + ui_px(2);

        if (required->note[0] != '\0' || required->download) {
          lv_obj_t *file_note = lv_label_create(info_view);
          if (required->download && required->note[0] != '\0') {
            char note[192];
            copy_text(note, sizeof(note), required->note);
            append_text(note, sizeof(note), required->found ?
                        " · Downloaded by the store" : " · Store-managed download");
            lv_label_set_text(file_note, note);
          } else if (required->download) {
            lv_label_set_text(file_note, required->found ?
                              "Downloaded by the store" : "Store-managed download");
          } else {
            lv_label_set_text(file_note, required->note);
          }
          lv_label_set_long_mode(file_note, LV_LABEL_LONG_WRAP);
          lv_obj_set_width(file_note, info_width - ui_px(6));
          lv_obj_set_style_text_color(file_note, lv_color_hex(COL_MUTED), LV_PART_MAIN);
          lv_obj_set_style_text_font(file_note, &lv_font_montserrat_14, LV_PART_MAIN);
          lv_obj_set_pos(file_note, 0, content_y);
          lv_obj_clear_flag(file_note, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
          lv_obj_update_layout(file_note);
          content_y += lv_obj_get_height(file_note) + ui_px(3);
        }
      }
    }

    if (g_detail_controls[0] != '\0') {
      content_y += ui_px(4);
      lv_obj_t *controls_heading = lv_label_create(info_view);
      lv_label_set_text(controls_heading, "CONTROLS");
      lv_obj_set_style_text_color(controls_heading, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
      lv_obj_set_style_text_font(controls_heading, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_pos(controls_heading, 0, content_y);
      lv_obj_clear_flag(controls_heading, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_update_layout(controls_heading);
      content_y += lv_obj_get_height(controls_heading) + ui_px(3);

      lv_obj_t *controls_label = lv_label_create(info_view);
      lv_label_set_text(controls_label, g_detail_controls);
      lv_label_set_long_mode(controls_label, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(controls_label, info_width - ui_px(6));
      lv_obj_set_style_text_color(controls_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
      lv_obj_set_style_text_font(controls_label, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_pos(controls_label, 0, content_y);
      lv_obj_clear_flag(controls_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
      /* On LVGL 9.5, a wrapped label's object height can still be a single
       * line immediately after creation. Measure the actual multiline text
       * explicitly so the following LICENSE section cannot overlap it. */
      lv_point_t controls_size = {};
      lv_text_get_size(&controls_size, g_detail_controls, &lv_font_montserrat_14,
                       0, 0, info_width - ui_px(6), LV_TEXT_FLAG_NONE);
      size_t controls_lines = 1;
      for (const char *p = g_detail_controls; *p != '\0'; p++)
        if (*p == '\n') controls_lines++;
      const int32_t line_height = lv_font_get_line_height(&lv_font_montserrat_14);
      const int32_t explicit_lines_height =
          (int32_t)controls_lines * line_height + ui_px(2);
      const int32_t controls_height = controls_size.y > explicit_lines_height ?
                                      controls_size.y : explicit_lines_height;
      lv_obj_set_height(controls_label, controls_height);
      content_y += controls_height + ui_px(5);
    }
    if (g_detail_license[0] != '\0') {
      lv_obj_t *license_heading = lv_label_create(info_view);
      lv_label_set_text(license_heading, "LICENSE");
      lv_obj_set_style_text_color(license_heading, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
      lv_obj_set_style_text_font(license_heading, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_pos(license_heading, 0, content_y);
      lv_obj_clear_flag(license_heading, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_update_layout(license_heading);
      content_y += lv_obj_get_height(license_heading) + ui_px(3);

      lv_obj_t *license_text = lv_label_create(info_view);
      lv_label_set_text(license_text, g_detail_license);
      lv_label_set_long_mode(license_text, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(license_text, info_width - ui_px(6));
      lv_obj_set_style_text_color(license_text, lv_color_hex(COL_MUTED), LV_PART_MAIN);
      lv_obj_set_style_text_font(license_text, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_pos(license_text, 0, content_y);
      lv_obj_clear_flag(license_text, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_update_layout(info_view);
    lv_obj_update_layout(info_label);
    if (g_svc != NULL && g_svc->log_printf != NULL)
      g_svc->log_printf("launcher_ui detail info layout: view=%dx%d content=%dx%d scroll-bottom=%ld text-bytes=%u\n",
                        lv_obj_get_width(info_view), lv_obj_get_height(info_view),
                        lv_obj_get_width(info_label), lv_obj_get_height(info_label),
                        (long)lv_obj_get_scroll_bottom(info_view),
                        (unsigned)(strlen(description_text) + strlen(g_detail_controls)));

    const uint32_t action_icons[DETAIL_ACTION_COUNT] = {
      0xf04bU, 0xf021U, 0xf071U, 0xf019U, 0xf053U
    };
    for (int i = 0; i < g_detail_action_count; i++) {
      g_detail_buttons[i] = lv_button_create(panel);
      lv_obj_set_size(g_detail_buttons[i], action_width, action_height);
      lv_obj_set_pos(g_detail_buttons[i], action_x, action_y + i * (action_height + action_gap));
      lv_obj_set_style_radius(g_detail_buttons[i], 4, LV_PART_MAIN);
      style_button(g_detail_buttons[i], i == g_detail_focus);
      if ((i == 1 && g_detail_is_remote && !g_detail_install_valid) ||
          (i == 2 && !g_detail_is_installed) ||
            (mutations_busy() && i < g_detail_action_count-1))
        lv_obj_add_state(g_detail_buttons[i], LV_STATE_DISABLED);
      /* React only to the finished click; LV_EVENT_ALL can recurse through
       * style updates and exhaust the ESPHome loop task's stack. */
      lv_obj_add_event_cb(g_detail_buttons[i], detail_button_event, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      const uint32_t action_icon = (!g_detail_extract_recipe.valid && i == 3) ?
                                   0xf053U : action_icons[i];
      lv_obj_t *label = create_fa_label(g_detail_buttons[i], action_icon,
                                        lv_color_hex(COL_TEXT));
      lv_obj_align(label, LV_ALIGN_LEFT_MID, ui_px(5), 0);
      lv_obj_t *button_text = lv_label_create(g_detail_buttons[i]);
      g_detail_button_labels[i] = button_text;
      lv_label_set_text(button_text, detail_action_text(i));
      lv_label_set_long_mode(button_text, LV_LABEL_LONG_CLIP);
      lv_obj_set_width(button_text, action_width - ui_px(28));
      lv_obj_set_style_text_color(button_text, lv_color_hex(COL_TEXT), LV_PART_MAIN);
      lv_obj_set_style_text_font(button_text, &lv_font_montserrat_12, LV_PART_MAIN);
      lv_obj_align(button_text, LV_ALIGN_LEFT_MID, ui_px(22), 0);
    }

    g_detail_progress = lv_bar_create(panel);
    lv_obj_set_size(g_detail_progress, info_width, ui_px(4));
    lv_obj_set_pos(g_detail_progress, info_x, progress_y);
    lv_bar_set_range(g_detail_progress, 0, 100);
    lv_bar_set_value(g_detail_progress, g_install_total == 0 ? 0 :
                     (int)(((uint64_t)g_install_done * 100U) / g_install_total), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_detail_progress, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_detail_progress, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    g_detail_status_label = lv_label_create(panel);
    lv_label_set_text(g_detail_status_label, g_install_status);
    lv_label_set_long_mode(g_detail_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_detail_status_label, info_width);
    lv_obj_set_style_text_color(g_detail_status_label, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_detail_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(g_detail_status_label, info_x, status_y);

    lv_obj_t *status = lv_label_create(screen);
    lv_label_set_text(status, "APP DETAILS   UP/DOWN ACTION   A SELECT   B BACK");
    lv_obj_set_style_text_color(status, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -ui_px(8));
    /* The panel is created after the bars; raise them last so taps in the
     * top controls can never land on a detail action underneath. */
    lv_obj_move_foreground(topbar);
    lv_obj_move_foreground(header);
    if (g_rom_picker_active) (void)build_rom_picker_overlay(screen);
    lv_obj_invalidate(screen);
    tooltip_configure();
    return 0;
  }

  g_list = lv_obj_create(screen);
  const int list_y = status_h + header_h + ui_px(5);
  const int keyboard_height = ui_px(100);
  const int keyboard_y = g_canvas_h - keyboard_height - ui_px(4);
  const int list_bottom = g_keyboard_open ? keyboard_y - ui_px(4) :
      (g_source == SOURCE_ARCHIVE ? g_canvas_h - ui_px(34) : g_canvas_h - status_h - ui_px(5));
  const int list_h = list_bottom > list_y ? list_bottom - list_y : ui_px(80);
  const int list_pad = ui_px(5);
  const int list_gap = ui_px(4);
  /* Keep the launcher dense when there are many apps, but let a short list
   * use the available panel instead of leaving a large empty block below it. */
  const int row_h = (list_h - (2 * list_pad) - (VISIBLE_ROWS - 1) * list_gap) / VISIBLE_ROWS;
  g_list_touch_row_step = (row_h > ui_px(25) ? row_h : ui_px(25)) + list_gap;
  lv_obj_set_pos(g_list, margin, list_y);
  lv_obj_set_size(g_list, g_canvas_w - 2 * margin, list_h);
  lv_obj_set_style_bg_color(g_list, lv_color_hex(COL_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_list, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_list, lv_color_hex(COL_MUTED), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_list, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g_list, list_pad, LV_PART_MAIN);
  if (g_view_count > VISIBLE_ROWS)
    lv_obj_set_style_pad_right(g_list, list_pad + ui_px(16), LV_PART_MAIN);
  lv_obj_set_style_pad_row(g_list, list_gap, LV_PART_MAIN);
  lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
  /* The five visible rows are virtualized. Native LVGL scrolling cannot
   * reveal hidden rows and was fighting the drag-to-selection mapping. */
  lv_obj_clear_flag(g_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

  const int row_count = VISIBLE_ROWS;
  for (int row = 0; row < row_count; row++) {
    const int view_index = g_first_visible + row;
    const int index = view_index < g_view_count ? g_visible_indices[view_index] : -1;
    g_buttons[row] = lv_button_create(g_list);
    if (index < 0) lv_obj_add_flag(g_buttons[row], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(g_buttons[row], LV_PCT(100));
    lv_obj_set_height(g_buttons[row], row_h > ui_px(25) ? row_h : ui_px(25));
    lv_obj_set_style_radius(g_buttons[row], 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(g_buttons[row], ui_px(8), LV_PART_MAIN);
    style_button(g_buttons[row], index >= 0 && view_index == g_selected);
    /* Restrict this to clicks: restyling from an LV_EVENT_ALL callback can
     * recursively trigger style events until the loopTask stack overflows. */
    lv_obj_add_event_cb(g_buttons[row], app_button_event, LV_EVENT_CLICKED,
                        (void *)(intptr_t)row);
    /* The row handler maps finger travel to one logical row at a time. */
    lv_obj_add_event_cb(g_buttons[row], list_touch_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_buttons[row], list_touch_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(g_buttons[row], list_touch_event, LV_EVENT_RELEASED, NULL);
    lv_obj_t *label = lv_label_create(g_buttons[row]);
    g_labels[row] = label;
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_t *details = lv_label_create(g_buttons[row]);
    g_list_detail_labels[row] = details;
    lv_label_set_long_mode(details, LV_LABEL_LONG_DOT);
    lv_obj_set_height(details, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(details, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_align(details, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    update_list_row_text(row, index, index >= 0 ? host_names[row] : "");
  }
  /* A separate track keeps native scrolling disabled for recycled rows. */
  const int scrollbar_width = ui_px(10);
  g_list_scrollbar_height = list_h - 2 * list_pad;
  g_list_scrollbar = lv_obj_create(screen);
  lv_obj_remove_style_all(g_list_scrollbar);
  lv_obj_set_pos(g_list_scrollbar, g_canvas_w - margin - list_pad - scrollbar_width,
                 list_y + list_pad);
  lv_obj_set_size(g_list_scrollbar, scrollbar_width, g_list_scrollbar_height);
  lv_obj_set_style_bg_color(g_list_scrollbar, lv_color_hex(COL_MUTED), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_list_scrollbar, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_radius(g_list_scrollbar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_clear_flag(g_list_scrollbar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_list_scrollbar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_list_scrollbar, list_scrollbar_event, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(g_list_scrollbar, list_scrollbar_event, LV_EVENT_PRESSING, NULL);
  g_list_scrollbar_thumb = lv_obj_create(g_list_scrollbar);
  lv_obj_remove_style_all(g_list_scrollbar_thumb);
  lv_obj_set_width(g_list_scrollbar_thumb, scrollbar_width);
  lv_obj_set_style_bg_color(g_list_scrollbar_thumb, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_list_scrollbar_thumb, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(g_list_scrollbar_thumb, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_clear_flag(g_list_scrollbar_thumb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  update_list_scrollbar();
  ensure_keyboard(screen);
  {
    g_empty_label = lv_label_create(g_list);
    const char *message = g_catalog_failed ? (g_source==SOURCE_NETWORK ? "PAX SUPPLY UNAVAILABLE - START SERVER OR CHECK URL" : "SEARCH FAILED - CHECK STATUS OR REFRESH") :
                          (g_favorites_mode ? "NO FAVORITES YET - PRESS Y ON AN APP" :
                          (g_search_text[0] != '\0' ? "NO SEARCH MATCHES" :
                          (g_source == SOURCE_STORAGE ? "NO INSTALLED APPS FOUND" : "CATALOG EMPTY")));
    /* Keep the empty-state copy concise and avoid an extra nested layout. */
    lv_label_set_text(g_empty_label, message);
    lv_obj_set_style_text_color(g_empty_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_center(g_empty_label);
    if (g_view_count > 0) lv_obj_add_flag(g_empty_label, LV_OBJ_FLAG_HIDDEN);
  }

  if (g_source == SOURCE_ARCHIVE) {
    g_archive_status_label = lv_label_create(screen);
    lv_label_set_text(g_archive_status_label, g_archive_status);
    lv_label_set_long_mode(g_archive_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_archive_status_label, g_canvas_w - 2 * margin);
    lv_obj_set_style_text_color(g_archive_status_label, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_archive_status_label, font, LV_PART_MAIN);
    lv_obj_align(g_archive_status_label, LV_ALIGN_BOTTOM_LEFT, margin, -ui_px(18));
    g_archive_progress = lv_bar_create(screen);
    lv_obj_set_size(g_archive_progress, g_canvas_w - 2 * margin, ui_px(5));
    lv_obj_align(g_archive_progress, LV_ALIGN_BOTTOM_MID, 0, -ui_px(5));
    lv_bar_set_range(g_archive_progress, 0, 100);
    const int percent = g_install_total == 0 ? 0 :
        (int)(((uint64_t)g_install_done * 100U) / g_install_total);
    lv_bar_set_value(g_archive_progress, percent > 100 ? 100 : percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_archive_progress, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_archive_progress, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  } else {
    lv_obj_t *status = lv_label_create(screen);g_footer_label=status;
    lv_label_set_text_fmt(status, "%s   L/R SOURCE   Y STAR   START FAV   SELECT SETTINGS",
                          g_favorites_mode ? "FAVORITES" : source_label(g_source));
    lv_obj_set_style_text_color(status, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -ui_px(8));
  }
  sync_keyboard_layout();
  if (g_rom_picker_active) (void)build_rom_picker_overlay(screen);
  /* Ensure the first worker-side timer pass has a dirty screen to render. */
  lv_obj_invalidate(screen);
  tooltip_configure();
  return 0;
}

static int pump_ui_on_host(void *arg)
{
  (void)arg;
  if (g_display == NULL) return -1;
  uint64_t start=timing_now();
  tooltip_poll();
  g_next_handler_ms = lv_timer_handler();
  timing_record(&g_pump_time,timing_now()-start);
  if (g_next_handler_ms > 20) g_next_handler_ms = 20;
  if (g_next_handler_ms == 0) g_next_handler_ms = 1;
  return 0;
}

static lv_obj_t *settings_text(lv_obj_t *parent, const char *text, int x, int y,
                               int width, const lv_font_t *font, bool muted)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(label, width);
  lv_obj_set_height(label, ui_px(18));
  lv_obj_set_style_text_color(label, lv_color_hex(muted ? COL_MUTED : COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, x, y + (ui_px(22) - ui_px(18)) / 2);
  return label;
}

static lv_obj_t *settings_button(lv_obj_t *parent, int focus, int x, int y,
                                 int width, int height, int action)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_size(button, width, height);
  lv_obj_set_pos(button, x, y + (ui_px(22) - height) / 2);
  lv_obj_set_style_pad_hor(button, ui_px(5), LV_PART_MAIN);
  style_button(button, g_settings_focus == focus);
  lv_obj_add_event_cb(button, toolbar_button_event, LV_EVENT_CLICKED,
                      (void *)(intptr_t)action);
  lv_obj_add_event_cb(button, settings_focus_event, LV_EVENT_PRESSED,
                      (void *)(intptr_t)focus);
  if (focus >= 0 && focus < SETTINGS_FOCUS_COUNT) g_settings_focus_widgets[focus] = button;
  return button;
}

static int teardown_ui_on_host(void *arg)
{
  (void)arg;
  tooltip_reset();g_tooltips_value=NULL;
  if (g_indev != NULL) {
    lv_indev_delete(g_indev);
    g_indev = NULL;
  }
  if (g_mouse_indev != NULL) {
    lv_indev_delete(g_mouse_indev);
    g_mouse_indev = NULL;
  }
  g_mouse_cursor = NULL;g_background_widget=NULL;g_work_label=NULL;
  if (g_display != NULL) {
    lv_display_delete(g_display);
    g_display = NULL;
  }
  g_count_label = NULL;
  g_list = NULL;
  g_list_scrollbar = NULL;
  g_list_scrollbar_thumb = NULL;
  g_settings_panel = NULL;
  g_detail_info_view = NULL;
  __atomic_store_n(&g_mouse_wheel_pending, 0, __ATOMIC_RELAXED);
  g_empty_label = NULL;
  g_detail_status_label = NULL;
  g_detail_progress = NULL;
  g_search_area = NULL;
  g_keyboard = NULL;g_footer_label=NULL;
  g_volume_slider = NULL;
  g_brightness_slider = NULL;
  g_volume_value = NULL;
  g_brightness_value = NULL;
  g_settings_back_button = NULL;
  g_app_details_switch = NULL;
  g_list_switches[0]=g_list_switches[1]=NULL;
  g_theme_value_label=NULL;
  g_sort_value_label=NULL;g_size_value_label=NULL;
  g_auto_brightness_button = NULL;
  g_archive_collection_button = NULL;
  g_background_button = NULL;
  g_background_dropdown = NULL;
  g_wifi_dropdown = NULL;g_wifi_scan_message_label=NULL;
  g_network_status_label = NULL;
  g_network_dhcp_button = NULL;
  g_network_dhcp_label = NULL;
  g_network_apply_button = NULL;
  for(int i=0;i<4;i++){g_network_extra_labels[i]=NULL;g_c6_labels[i]=NULL;}
  for(int i=0;i<3;i++)g_firmware_labels[i]=NULL;
  g_firmware_confirm=false;
  g_firmware_last_state=UINT32_MAX;
  g_c6_progress=NULL;
  g_settings_message_label = NULL;
  for (int i = 0; i < SETTINGS_FOCUS_COUNT; i++) g_settings_focus_widgets[i] = NULL;
  for (int i = 0; i < 4; i++) g_network_value_labels[i] = NULL;
  for (int i = 0; i < PAX_STORAGE_COUNT; i++) {
    g_storage_status_labels[i] = NULL;
    g_storage_buttons[i] = NULL;
    g_storage_button_labels[i] = NULL;
  }
  g_status_clock_icon = NULL;
  g_status_clock = NULL;
  g_status_network_icon = NULL;
  g_status_battery_icon = NULL;
  g_status_battery_level = NULL;
  g_archive_status_label = NULL;
  g_archive_progress = NULL;
  for (int i = 0; i < DETAIL_ACTION_COUNT; i++) {
    g_detail_buttons[i] = NULL;
    g_detail_button_labels[i] = NULL;
  }
  for (int i = 0; i < ROM_PICKER_VISIBLE_ROWS; i++) {
    g_rom_picker_rows[i] = NULL;
    g_rom_picker_row_labels[i] = NULL;
  }
  g_rom_picker_status = NULL;
  for (int i = 0; i < MAX_DETAIL_FILES; i++) g_detail_file_state_labels[i] = NULL;
  for (int row = 0; row < VISIBLE_ROWS; row++) {
    g_buttons[row] = NULL;
    g_labels[row] = NULL;
    g_list_detail_labels[row] = NULL;
  }
  lv_deinit();
  return 0;
}

static bool poll_controller(void)
{
  pax_gamepad_state_t pad;
  g_svc->input_gamepad_read(&pad);
  const bool up = pad.values[PAX_INPUT_UP] != 0 || g_keyboard_nav_down[0] ||
                  g_keyboard_nav_pending[0];
  const bool right = pad.values[PAX_INPUT_RIGHT] != 0 || g_keyboard_nav_down[1] ||
                     g_keyboard_nav_pending[1];
  const bool down = pad.values[PAX_INPUT_DOWN] != 0 || g_keyboard_nav_down[2] ||
                    g_keyboard_nav_pending[2];
  const bool left = pad.values[PAX_INPUT_LEFT] != 0 || g_keyboard_nav_down[3] ||
                    g_keyboard_nav_pending[3];
  const bool a = pad.values[PAX_INPUT_A] != 0 || g_keyboard_enter_down ||
                 g_keyboard_enter_pending;
  const bool b = pad.values[PAX_INPUT_B] != 0;
  const bool x = pad.values[PAX_INPUT_X] != 0;
  const bool y = pad.values[PAX_INPUT_Y] != 0;
  const bool select = pad.values[PAX_INPUT_SELECT] != 0;
  /* USB Enter is also exposed as gamepad Start by the shared input layer.
   * In this UI, treat it only as the standard focused-item accept key. */
  const bool start = pad.values[PAX_INPUT_START] != 0 &&
                     !g_keyboard_enter_down && !g_keyboard_enter_pending;
  const bool l = pad.values[PAX_INPUT_L] != 0;
  const bool r = pad.values[PAX_INPUT_R] != 0;
  const bool l3 = g_svc->input_l3_read != NULL && g_svc->input_l3_read() != 0;
  const uint32_t now_ms = (uint32_t)(g_svc->get_time_us() / 1000U);
  const bool up_pressed = pax_input_repeat_step(&g_nav_repeat[0], up, now_ms);
  const bool right_pressed = pax_input_repeat_step(&g_nav_repeat[1], right, now_ms);
  const bool down_pressed = pax_input_repeat_step(&g_nav_repeat[2], down, now_ms);
  const bool left_pressed = pax_input_repeat_step(&g_nav_repeat[3], left, now_ms);
  const bool page_up_pressed = pax_input_repeat_step(&g_nav_repeat[4], l, now_ms);
  const bool page_down_pressed = pax_input_repeat_step(&g_nav_repeat[5], r, now_ms);
  for (int i = 0; i < 4; i++) g_keyboard_nav_pending[i] = false;
  g_keyboard_enter_pending = false;
  bool changed = false;
  const int previous_action=g_open_index;
  const bool toolbar_was_active = g_toolbar_focus >= 0;
  if(g_settings_mode && g_wifi_dropdown_open && !g_keyboard_open){
    int count=(g_wifi_scan.state==2 ? (int)g_wifi_scan.count : 0)+2;
    if(up_pressed || down_pressed){g_wifi_choice=(g_wifi_choice+count+(up_pressed ? -1 : 1))%count;changed=true;}
    if(a && !g_pad_last[PAX_INPUT_A]){g_wifi_dropdown_open=false;g_open_index=wifi_choice_action();}
    if((b && !g_pad_last[PAX_INPUT_B]) || (x && !g_pad_last[PAX_INPUT_X]) || l3){g_wifi_dropdown_open=false;changed=true;}
    if(changed || !g_wifi_dropdown_open)(void)g_svc->ui_call_sync(wifi_dropdown_state_on_host,NULL,1000);
  }else if (g_rom_picker_active) {
    if (!g_rom_prepare_busy && up_pressed && g_rom_picker_count > 0) {
      g_rom_picker_selected = (g_rom_picker_selected + g_rom_picker_count - 1) % g_rom_picker_count;
      changed = true;
    } else if (!g_rom_prepare_busy && down_pressed && g_rom_picker_count > 0) {
      g_rom_picker_selected = (g_rom_picker_selected + 1) % g_rom_picker_count;
      changed = true;
    }
    if (changed) {
      if (g_rom_picker_selected < g_rom_picker_first)
        g_rom_picker_first = g_rom_picker_selected;
      else if (g_rom_picker_selected >= g_rom_picker_first + g_rom_picker_rows_shown)
        g_rom_picker_first = g_rom_picker_selected - g_rom_picker_rows_shown + 1;
      (void)g_svc->ui_call_sync(update_rom_picker_on_host, NULL, 1000);
    }
    if (!g_rom_prepare_busy && a && !g_pad_last[PAX_INPUT_A] && g_rom_picker_count > 0) g_open_index = -60;
    if ((b && !g_pad_last[PAX_INPUT_B]) || (x && !g_pad_last[PAX_INPUT_X]) || l3)
      g_open_index = -61;
  } else if (g_detail_index >= 0) {
    if (g_toolbar_focus >= 0 && g_toolbar_focus < SOURCE_COUNT) {
      if (left_pressed) {
        g_toolbar_focus = (g_toolbar_focus + SOURCE_COUNT - 1) % SOURCE_COUNT;
        changed = true;
      } else if (right_pressed) {
        g_toolbar_focus = (g_toolbar_focus + 1) % SOURCE_COUNT;
        changed = true;
      }
      if ((down_pressed) || (b && !g_pad_last[PAX_INPUT_B])) {
        g_toolbar_focus = -1;
        changed = true;
      }
      if (a && !g_pad_last[PAX_INPUT_A]) select_source(g_toolbar_focus);
    } else {
      const char *detail_direction = NULL;
      if (up_pressed) detail_direction = "up";
      else if (down_pressed) detail_direction = "down";
      else if (left_pressed) detail_direction = "left";
      else if (right_pressed) detail_direction = "right";
      if (up_pressed) {
        if (g_detail_focus > 0) {
          g_detail_focus--;
          changed = true;
        } else {
          g_toolbar_focus = g_source;
          changed = true;
        }
      } else if (down_pressed) {
        if (g_detail_focus + 1 < g_detail_action_count) {
          g_detail_focus++;
          changed = true;
        }
      }
      if (detail_direction != NULL && g_svc->log_printf != NULL)
        g_svc->log_printf("launcher_ui detail dpad=%s action=%d/%d%s\n",
                          detail_direction, g_detail_focus + 1, g_detail_action_count,
                          (strcmp(detail_direction, "left") == 0 ||
                           strcmp(detail_direction, "right") == 0) ? " (horizontal ignored)" : "");
      if (page_up_pressed)
        (void)g_svc->ui_call_sync(detail_scroll_on_host, (void *)(intptr_t)-4, 1000);
      else if (page_down_pressed)
        (void)g_svc->ui_call_sync(detail_scroll_on_host, (void *)(intptr_t)4, 1000);
      if (a && !g_pad_last[PAX_INPUT_A] && (!mutations_busy() || g_detail_focus==g_detail_action_count-1)) g_open_index = g_detail_focus;
      if (b && !g_pad_last[PAX_INPUT_B]) g_open_index = g_detail_action_count - 1;
    }
    if ((x && !g_pad_last[PAX_INPUT_X]) || l3) g_open_index = -2;
  } else if (g_settings_mode && !g_keyboard_open) {
    if (page_up_pressed) {
      if (g_settings_focus == 3 && g_background_dropdown_open) {
        const int option_count = g_background_count + 1;
        const int jump = VISIBLE_ROWS % option_count;
        g_background_choice = (g_background_choice + option_count - jump) % option_count;
        (void)g_svc->ui_call_sync(background_dropdown_choice_on_host,
                                  (void *)(intptr_t)g_background_choice, 1000);
      } else {
        g_settings_focus = settings_focus_step(g_settings_focus, -VISIBLE_ROWS);
      }
      changed = true;
    } else if (page_down_pressed) {
      if (g_settings_focus == 3 && g_background_dropdown_open) {
        const int option_count = g_background_count + 1;
        g_background_choice = (g_background_choice + 5) % option_count;
        (void)g_svc->ui_call_sync(background_dropdown_choice_on_host,
                                  (void *)(intptr_t)g_background_choice, 1000);
      } else {
        g_settings_focus = settings_focus_step(g_settings_focus, VISIBLE_ROWS);
      }
      changed = true;
    }
    if (up_pressed) {
      if (g_settings_focus == 3 && g_background_dropdown_open) {
        const int option_count = g_background_count + 1;
        g_background_choice = (g_background_choice + option_count - 1) % option_count;
        (void)g_svc->ui_call_sync(background_dropdown_choice_on_host,
                                  (void *)(intptr_t)g_background_choice, 1000);
        changed = true;
      } else {
        g_settings_focus = settings_focus_step(g_settings_focus, -1);
        changed = true;
      }
    } else if (down_pressed) {
      if (g_settings_focus == 3 && g_background_dropdown_open) {
        const int option_count = g_background_count + 1;
        g_background_choice = (g_background_choice + 1) % option_count;
        (void)g_svc->ui_call_sync(background_dropdown_choice_on_host,
                                  (void *)(intptr_t)g_background_choice, 1000);
        changed = true;
      } else {
        g_settings_focus = settings_focus_step(g_settings_focus, 1);
        changed = true;
      }
    }
    const int direction = (right_pressed) ? 1 :
                          ((left_pressed) ? -1 : 0);
    if (direction != 0 && g_settings_focus == 0) {
      g_volume_level += direction * 5;
      if (g_volume_level < 0) g_volume_level = 0;
      if (g_volume_level > 100) g_volume_level = 100;
      g_settings_dirty = true;
      changed = true;
    } else if (direction != 0 && g_settings_focus == 1) {
      g_brightness_level += direction * 5;
      if (g_brightness_level < 5) g_brightness_level = 5;
      if (g_brightness_level > 100) g_brightness_level = 100;
      g_settings_dirty = true;
      changed = true;
    } else if (direction != 0 && g_settings_focus == 2) {
      set_auto_brightness(direction > 0);
      changed = true;
    } else if (direction != 0 && g_settings_focus == 12) {
      g_network_config.dhcp_enabled = direction > 0;g_network_config_dirty=true;
      copy_text(g_settings_message, sizeof(g_settings_message), "DHCP mode staged; select APPLY NETWORK");
      changed = true;
    }
    if(a && !g_pad_last[PAX_INPUT_A] && g_settings_focus==36){
      g_wifi_dropdown_open=true;g_wifi_choice=0;
      (void)g_svc->ui_call_sync(wifi_dropdown_state_on_host,NULL,1000);
      if(g_wifi_scan.state==0)g_open_index=-80;
    }
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 2) {
      toggle_auto_brightness();
      changed = true;
    }
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 3) {
      if (g_background_dropdown_open) {
        g_background_index = g_background_choice - 1;
        g_background_dropdown_open = false;
        g_open_index = -10;
      } else {
        g_background_choice = g_background_index + 1;
        g_background_dropdown_open = true;
        g_open_index = -17;
      }
    }
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 4) g_open_index = -13;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 5) g_open_index = -40;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 6) g_open_index = -41;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 7) g_open_index = -42;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 8) g_open_index = -44;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 9) g_open_index = -45;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 10) g_open_index = -46;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 11) g_open_index = -47;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 12) {
      g_network_config.dhcp_enabled = !g_network_config.dhcp_enabled;g_network_config_dirty=true;
      copy_text(g_settings_message, sizeof(g_settings_message), "DHCP mode staged; select APPLY NETWORK");
      changed = true;
    }
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus >= 13 && g_settings_focus <= 16)
      g_open_index = -30 - (g_settings_focus - 13);
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus>=25 && g_settings_focus<=28)g_open_index=-62-(g_settings_focus-25);
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 17 && !g_settings_scan_busy && !g_settings_scan_pending && !mutations_busy()) g_open_index = -35;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 18 && !g_settings_scan_busy && !g_settings_scan_pending && !mutations_busy()) g_open_index = -36;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 19 && !g_settings_scan_busy && !g_settings_scan_pending && !mutations_busy()) g_open_index = -37;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 20) {
      g_app_details_enabled = !g_app_details_enabled;
      g_open_index = -43;
    }
    if(g_settings_focus==30 && ((left && !g_pad_last[PAX_INPUT_LEFT]) || (right && !g_pad_last[PAX_INPUT_RIGHT]) || (a && !g_pad_last[PAX_INPUT_A]))){
      g_size_direction=left ? -1 : 1;g_open_index=-68;
    }
    if(g_settings_focus==29 && ((left && !g_pad_last[PAX_INPUT_LEFT]) || (right && !g_pad_last[PAX_INPUT_RIGHT]) || (a && !g_pad_last[PAX_INPUT_A]))){
      g_sort_direction=left ? -1 : 1;g_open_index=-67;
    }
    if(g_settings_focus==24 && ((left && !g_pad_last[PAX_INPUT_LEFT]) || (right && !g_pad_last[PAX_INPUT_RIGHT]) || (a && !g_pad_last[PAX_INPUT_A]))){
      g_theme_direction=left ? -1 : 1;g_open_index=-49;
    }
    if(a && !g_pad_last[PAX_INPUT_A] && (g_settings_focus==22 || g_settings_focus==23)){
      if(g_settings_focus==22)g_hide_pax_extension=!g_hide_pax_extension;
      else g_list_file_details=!g_list_file_details;
      g_open_index=-48;
    }
    if(a && !g_pad_last[PAX_INPUT_A] && g_settings_focus>=34 && g_settings_focus<=35)g_open_index=-73-(g_settings_focus-34);
    if(a && !g_pad_last[PAX_INPUT_A] && g_settings_focus>=31 && g_settings_focus<=33)g_open_index=-70-(g_settings_focus-31);
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 37) g_open_index=-75;
    if (a && !g_pad_last[PAX_INPUT_A] && g_settings_focus == 21) g_open_index = -9;
    if ((b && !g_pad_last[PAX_INPUT_B]) || (x && !g_pad_last[PAX_INPUT_X]) || l3) {
      if (g_background_dropdown_open) {
        g_background_dropdown_open = false;
        g_open_index = -17;
      } else {
        g_open_index = -9;
      }
    }
  } else if (g_keyboard_open) {
    if (left_pressed) { move_keyboard_focus(-1, 0); changed = true; }
    else if (right_pressed) { move_keyboard_focus(1, 0); changed = true; }
    else if (up_pressed) { move_keyboard_focus(0, -1); changed = true; }
    else if (down_pressed) { move_keyboard_focus(0, 1); changed = true; }
    if (a && !g_pad_last[PAX_INPUT_A])
      (void)g_svc->ui_call_sync(keyboard_activate_on_host, NULL, 1000);
    if ((b && !g_pad_last[PAX_INPUT_B]) || (x && !g_pad_last[PAX_INPUT_X]) || l3) {
      g_keyboard_open = false;
      g_open_index = -3;
    }
    if (changed) (void)g_svc->ui_call_sync(keyboard_focus_on_host, NULL, 1000);
  } else if (g_toolbar_focus >= 0) {
    if (left_pressed) {
      g_toolbar_focus = toolbar_focus_step(g_toolbar_focus, -1);
      changed = true;
    } else if (right_pressed) {
      g_toolbar_focus = toolbar_focus_step(g_toolbar_focus, 1);
      changed = true;
    }
    if (down_pressed) {
      g_toolbar_focus = g_toolbar_focus < 6 ? 7 : -1;
      changed = true;
    }
    if (up_pressed && g_toolbar_focus >= 6) {
      g_toolbar_focus = g_toolbar_focus == 6 ? (g_favorites_mode ? 4 : g_source) :
                        (g_toolbar_focus == 11 ? SOURCE_ARCHIVE : 5);
      changed = true;
    }
    if (b && !g_pad_last[PAX_INPUT_B]) {
      g_toolbar_focus = -1;
      changed = true;
    }
    if (a && !g_pad_last[PAX_INPUT_A]) {
      if (g_toolbar_focus < SOURCE_COUNT) {
        if (g_toolbar_focus == SOURCE_ARCHIVE && g_source == SOURCE_ARCHIVE &&
            g_archive_level != ARCHIVE_LEVEL_SEARCH)
          g_open_index = -15;
        else if (g_toolbar_focus == g_source && g_toolbar_focus == SOURCE_ARCHIVE)
          g_open_index = -3;
        else
          select_source(g_toolbar_focus);
      } else if (g_toolbar_focus == 4) g_open_index = -6;
      else if (g_toolbar_focus == 5) g_open_index = -7;
      else if (g_toolbar_focus == 6) g_open_index = -5;
      else if (g_toolbar_focus == 7) g_open_index = -8;
      else if (g_toolbar_focus == 8) g_open_index = -12;
      else if (g_toolbar_focus == 9) g_open_index = -8;
      else if (g_toolbar_focus == 10) g_open_index = -3;
      else if (g_toolbar_focus == 11 && g_source == SOURCE_ARCHIVE &&
               g_archive_level != ARCHIVE_LEVEL_SEARCH) g_open_index = -15;
    }
  } else if (g_view_count > 0) {
    if (page_up_pressed) {
      g_selected -= VISIBLE_ROWS;
      if (g_selected < 0) g_selected = 0;
      changed = true;
    } else if (page_down_pressed) {
      g_selected += VISIBLE_ROWS;
      if (g_selected >= g_view_count) g_selected = g_view_count - 1;
      changed = true;
    }
    if (up_pressed && g_selected == 0) {
      g_toolbar_focus = g_favorites_mode ? 4 : g_source;
      changed = true;
    } else if (up_pressed) { g_selected--; changed = true; }
    if (left_pressed) { g_selected--; changed = true; }
    if (down_pressed) { g_selected++; changed = true; }
    if (right_pressed) { g_selected++; changed = true; }
    if (g_selected < 0) g_selected = g_view_count - 1;
    if (g_selected >= g_view_count) g_selected = 0;
    if (g_selected < g_first_visible)
      g_first_visible = g_selected;
    else if (g_selected >= g_first_visible + VISIBLE_ROWS)
      g_first_visible = g_selected - VISIBLE_ROWS + 1;
    const int max_first = g_view_count > VISIBLE_ROWS ? g_view_count - VISIBLE_ROWS : 0;
    if (g_first_visible > max_first) g_first_visible = max_first;
  } else if (up_pressed) {
    /* Empty lists still need a route to sources, settings and search. */
    g_toolbar_focus = g_favorites_mode ? 4 : g_source;
    changed = true;
  } else if (page_up_pressed) {
    select_source((g_source + SOURCE_COUNT - 1) % SOURCE_COUNT);
  } else if (page_down_pressed) {
    select_source((g_source + 1) % SOURCE_COUNT);
  }
  if (g_detail_index < 0 && !g_rom_picker_active) {
    if (!g_settings_mode && !g_keyboard_open && !toolbar_was_active && g_toolbar_focus < 0 &&
        a && !g_pad_last[PAX_INPUT_A]) {
      const int record = selected_record_index();
      if (record >= 0)
        g_open_index = g_item_source[record] == SOURCE_ARCHIVE ? -14 :
                       (g_app_details_enabled ? -18 : record);
    }
    if (!g_settings_mode && !g_keyboard_open && !toolbar_was_active && g_toolbar_focus < 0 &&
        y && !g_pad_last[PAX_INPUT_Y])
      g_open_index = -5;
    if (!g_settings_mode && !g_keyboard_open && !toolbar_was_active && g_toolbar_focus < 0 &&
        start && !g_pad_last[PAX_INPUT_START])
      g_open_index = -6;
    if (!g_settings_mode && !g_keyboard_open && !toolbar_was_active && g_toolbar_focus < 0 &&
        select && !g_pad_last[PAX_INPUT_SELECT])
      g_open_index = -7;
    if (!g_settings_mode && !g_keyboard_open && !toolbar_was_active && g_toolbar_focus < 0 &&
        ((b && !g_pad_last[PAX_INPUT_B]) || (x && !g_pad_last[PAX_INPUT_X]) || l3))
      g_open_index = g_source == SOURCE_ARCHIVE && g_archive_level != ARCHIVE_LEVEL_SEARCH ? -15 : -2;
  }
  if(changed || previous_action!=g_open_index)mark_ui_action();
  const bool accept_pressed = a && !g_pad_last[PAX_INPUT_A];
  const bool back_pressed = b && !g_pad_last[PAX_INPUT_B];
  if (changed) {
    if (g_settings_mode && g_settings_focus == 0 &&
        ((right_pressed) || (left_pressed)))
      play_ui_sound(PAX_UI_SOUND_VOLUME, (uint8_t)g_volume_level);
    else
      play_ui_sound(PAX_UI_SOUND_CLICK, 0);
  }
  if (accept_pressed && !g_keyboard_open)
    play_ui_sound(PAX_UI_SOUND_CONFIRM, 0);
  else if (back_pressed)
    play_ui_sound(PAX_UI_SOUND_BACK, 0);
  g_pad_last[PAX_INPUT_UP] = up;
  g_pad_last[PAX_INPUT_RIGHT] = right;
  g_pad_last[PAX_INPUT_DOWN] = down;
  g_pad_last[PAX_INPUT_LEFT] = left;
  g_pad_last[PAX_INPUT_A] = a;
  g_pad_last[PAX_INPUT_B] = b;
  g_pad_last[PAX_INPUT_X] = x;
  g_pad_last[PAX_INPUT_Y] = y;
  g_pad_last[PAX_INPUT_SELECT] = select;
  g_pad_last[PAX_INPUT_START] = start;
  g_pad_last[PAX_INPUT_L] = l;
  g_pad_last[PAX_INPUT_R] = r;
  return changed;
}

static bool poll_usb_keyboard(void)
{
  if (g_svc->input_keyboard_read == NULL) return false;
  pax_keyboard_event_t event;
  bool updated = false;
  /* Drain a small bounded burst so held/rapid typing feels responsive without
   * starving display refresh or controller polling. */
  for (int i = 0; i < 8 && g_svc->input_keyboard_read(&event); i++) {
    int direction = -1;
    if (event.key == PAX_KEY_UP) direction = 0;
    else if (event.key == PAX_KEY_RIGHT) direction = 1;
    else if (event.key == PAX_KEY_DOWN) direction = 2;
    else if (event.key == PAX_KEY_LEFT) direction = 3;
    if (direction >= 0) {
      static const char *const direction_names[4] = {"up", "right", "down", "left"};
      if (event.down && g_svc->log_printf != NULL)
        g_svc->log_printf("launcher_ui USB keyboard arrow: %s (key=%d)\n",
                          direction_names[direction], event.key);
      if (event.down && !g_keyboard_nav_down[direction])
        g_keyboard_nav_pending[direction] = true;
      g_keyboard_nav_down[direction] = event.down != 0;
      continue;
    }
    if (event.key == PAX_KEY_ENTER) {
      if (event.down && !g_keyboard_enter_down)
        g_keyboard_enter_pending = true;
      g_keyboard_enter_down = event.down != 0;
      continue;
    }
    if (!event.down) continue;
    if ((event.key >= 32 && event.key <= 126) || event.key == 8 || event.key == 127) {
      g_pending_key = event.key;
      g_pending_key_valid = true;
      (void)g_svc->ui_call_sync(usb_text_on_host, NULL, 1000);
      updated = true;
    }
  }
  return updated;
}

static void wait_for_input_release(void)
{
  pax_gamepad_state_t pad;
  for (int sample = 0; sample < 100; sample++) {
    g_svc->input_gamepad_read(&pad);
    bool held = false;
    for (int i = 0; i < PAX_INPUT_MAX; i++) {
      if (pad.values[i] != 0) { held = true; break; }
    }
    if (!held) break;
    g_svc->delay_ms(10);
  }
  if (g_svc->input_keyboard_read != NULL) {
    pax_keyboard_event_t event;
    for (int sample = 0; sample < 32 && g_svc->input_keyboard_read(&event); sample++) {}
  }
  for (int i = 0; i < PAX_INPUT_MAX; i++) g_pad_last[i] = false;
  for (int i = 0; i < 4; i++) {
    g_keyboard_nav_down[i] = false;
    g_keyboard_nav_pending[i] = false;
  }
  memset(g_nav_repeat, 0, sizeof(g_nav_repeat));
  g_keyboard_enter_down = false;
  g_keyboard_enter_pending = false;
}

/* Entry-task publication: a displayed row never changes its mapping between
 * a metadata arrival and the matching LVGL update. Input forces publication. */
static bool flush_metadata_updates(bool force)
{
  if(!g_metadata_pending || g_catalog_loading || g_settings_mode || g_detail_index>=0 || g_open_index!=-1)return false;
  uint32_t now=(uint32_t)(g_svc->get_time_us()/1000U);
  if(!force && (uint32_t)(now-g_metadata_last_flush)<150U)return false;
  const int selected=selected_record_index();
  apply_filter();
  for(int i=0;i<g_view_count;i++)if(g_visible_indices[i]==selected){g_selected=i;break;}
  if(g_selected<g_first_visible)g_first_visible=g_selected;
  if(g_selected>=g_first_visible+VISIBLE_ROWS)g_first_visible=g_selected-VISIBLE_ROWS+1;
  g_metadata_pending=false;g_metadata_last_flush=now;g_metadata_flushes++;
  (void)g_svc->ui_call_sync(update_selection_on_host,NULL,1000);
  return true;
}

static void io_ui_pulse(void)
{
  static uint32_t last_ui,last_status,last_transfer,last_rom;
  if(!g_display)return;
  uint32_t now=(uint32_t)(g_svc->get_time_us()/1000U);
  if((uint32_t)(now-last_ui)<16)return;
  last_ui=now;
  poll_wifi_scan();
  poll_settings_connection();
  if(g_rom_prepare_busy && (uint32_t)(now-last_rom)>=250){
    (void)g_svc->ui_call_sync(update_rom_picker_on_host,NULL,1000);last_rom=now;
  }
  if(g_transfer_busy && (uint32_t)(now-last_transfer)>=250){poll_install_status();last_transfer=now;}
  bool changed=poll_usb_keyboard();changed=poll_controller() || changed;
  const int wheel=__atomic_exchange_n(&g_mouse_wheel_pending,0,__ATOMIC_ACQ_REL);
  if(wheel)(void)g_svc->ui_call_sync(mouse_scroll_on_host,(void *)(intptr_t)wheel,1000);
  if(g_filter_dirty && !g_catalog_loading){g_filter_dirty=false;apply_filter();changed=true;}
  if(!flush_metadata_updates(changed) && changed)(void)g_svc->ui_call_sync(update_selection_on_host,NULL,1000);
  if((uint32_t)(now-last_status)>=1000){
    (void)g_svc->ui_call_sync(update_device_status_on_host,NULL,1000);last_status=now;
  }
  (void)g_svc->ui_call_sync(pump_ui_on_host,NULL,1000);
}

/* Distinct screens have different object trees; only update matching ones. */
static unsigned ui_layout_kind(void)
{
  return (g_settings_mode ? 2U : 0U) |
         (g_detail_index>=0 ? 4U : 0U) |
         (g_source==SOURCE_ARCHIVE && !g_favorites_mode ? 8U : 0U) |
         (g_rom_picker_active ? 16U : 0U);
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
  if (svc == NULL || svc->abi_version != PAX_ABI_VERSION ||
      svc->ui_call_sync == NULL || svc->display_write_frame_custom == NULL ||
      svc->display_write_rect == NULL || svc->display_flush == NULL ||
      svc->input_gamepad_read == NULL || svc->mem_caps_alloc == NULL || svc->mem_free == NULL)
    return -1;
  svc->log_printf("launcher_ui startup: services validated\n");
  g_svc = svc;
  g_discard_io=false;
  if(!io_start()){svc->log_printf("launcher_ui: I/O worker unavailable\n");return -4;}
  poll_install_status(); /* Adopt an install still running after launcher relaunch. */
  refresh_archive_auth_state();
  svc->log_printf("launcher_ui startup: archive auth read\n");
  load_archive_collection();
  svc->log_printf("launcher_ui startup: collection read\n");
  load_archive_credentials();
  svc->log_printf("launcher_ui startup: account credentials read\n");
  load_archive_s3_keys();
  svc->log_printf("launcher_ui startup: S3 credentials read\n");
  /* The newer loader offers the active panel size without requiring an app
   * to know the LCD model. The launcher is intentionally native-resolution:
   * it is unloaded before an emulator starts, so the extra framebuffer is
   * reclaimed at hand-off and cannot slow the game PAX. Older loaders leave
   * the appended callback NULL and retain the 800x480 fallback. */
  int canvas_w = DEFAULT_PANEL_W;
  int canvas_h = DEFAULT_PANEL_H;
  if (svc->display_get_size != NULL && svc->display_set_canvas != NULL) {
    int offered_w = 0, offered_h = 0;
    svc->log_printf("launcher_ui startup: querying offered canvas\n");
    svc->display_get_size(&offered_w, &offered_h);
    svc->log_printf("launcher_ui startup: offered canvas=%dx%d\n", offered_w, offered_h);
    int canvas_result = -1;
    if (offered_w >= 320 && offered_h >= 240 && (offered_w % 2) == 0 && (offered_h % 2) == 0) {
      svc->log_printf("launcher_ui startup: selecting offered canvas\n");
      canvas_result = svc->display_set_canvas(offered_w, offered_h);
      svc->log_printf("launcher_ui startup: canvas callback returned %d\n", canvas_result);
    }
    if (canvas_result == 0) {
      canvas_w = offered_w;
      canvas_h = offered_h;
      svc->log_printf("launcher_ui startup: offered canvas selected\n");
    } else {
      svc->log_printf("launcher_ui: offered canvas %dx%d could not be selected; using %dx%d\n",
                      offered_w, offered_h, DEFAULT_PANEL_W, DEFAULT_PANEL_H);
    }
  } else {
    svc->log_printf("launcher_ui: canvas selection API unavailable; using legacy %dx%d\n",
                    DEFAULT_PANEL_W, DEFAULT_PANEL_H);
  }
  g_render_scale = 1.0f;
  g_panel_w = canvas_w;
  g_panel_h = canvas_h;
  g_canvas_w = canvas_w;
  g_canvas_h = canvas_h;
  g_mouse_x = (lv_coord_t)(g_canvas_w / 2);
  g_mouse_y = (lv_coord_t)(g_canvas_h / 2);
  g_mouse_buttons = 0;
  g_mouse_cursor_seen = false;
  g_mouse_last_activity_ms = 0;
  const float sx = (float)g_canvas_w / (float)DEFAULT_CANVAS_W;
  const float sy = (float)g_canvas_h / (float)DEFAULT_CANVAS_H;
  g_ui_scale = sx < sy ? sx : sy;
  if (g_ui_scale < 0.75f) g_ui_scale = 0.75f;
  svc->log_printf("launcher_ui canvas selected=%dx%d scale=%.2f ui=%.2f\n",
                  g_canvas_w, g_canvas_h,
                  (double)g_render_scale, (double)g_ui_scale);
  g_canvas = (uint16_t *)svc->mem_caps_alloc((size_t)g_canvas_w * g_canvas_h * sizeof(uint16_t),
                                             PAX_MEM_CAP_SPIRAM | PAX_MEM_CAP_DMA);
  if (g_canvas == NULL) {io_finish();return -2;}
  for (int i = 0; i < MAX_APPS; i++) {
    g_names[i][0] = '\0';
    g_titles[i][0] = '\0';
    g_paths[i][0] = '\0';
  }

  g_selected = 0;
  g_first_visible = 0;
  g_source = SOURCE_STORAGE;
  g_rom_picker_active = false;
  g_rom_picker_rom_selected = false;
  g_rom_picker_count = 0;
  g_rom_picker_selected = 0;
  g_rom_picker_first = 0;
  g_rom_picker_app_index = -1;
  g_favorites_mode = false;
  g_settings_mode = false;
  g_keyboard_open = false;
  load_network_url();
  load_themes();
  load_app_details_preference();
  load_tooltips_preference();
  load_list_preferences();
  svc->log_printf("launcher_ui: App Details on Select=%s (missing preference defaults on)\n",
                  g_app_details_enabled ? "ON" : "OFF");
  g_filter_dirty = false;
  g_settings_dirty = false;
  g_search_text[0] = '\0';
  g_empty_label = NULL;
  g_favorite_count = 0;
  load_favorites();
  load_backgrounds();
  build_background_options();
  g_volume_level = svc->settings_volume_get != NULL ? (int)svc->settings_volume_get() : 100;
  g_brightness_level = svc->settings_brightness_get != NULL ? (int)svc->settings_brightness_get() : 100;
  g_auto_brightness_enabled = svc->settings_auto_brightness_get != NULL &&
                              svc->settings_auto_brightness_get() != 0;
  if (g_volume_level < 0) g_volume_level = 0;
  if (g_volume_level > 100) g_volume_level = 100;
  if (g_brightness_level < 5) g_brightness_level = 5;
  if (g_brightness_level > 100) g_brightness_level = 100;
  restore_launcher_resume_state();
  for (int i = 0; i < PAX_INPUT_MAX; i++) g_pad_last[i] = false;
  int chosen = -2;
  bool detail_mode = false;
  bool launch_disc_installer = false;
  bool built_plain_list=false;
  unsigned built_layout=~0U;
  for (;;) {
    g_open_index = -1;
    g_discard_io=false;
    if(g_display && built_layout!=ui_layout_kind())(void)svc->ui_call_sync(teardown_ui_on_host,NULL,2000);
    if (!detail_mode && !g_settings_mode && g_catalog_stale) {
      g_detail_index = -1;
      g_catalog_loading=true;g_app_count=0;g_view_count=0;
      if(!g_display){
        if(svc->ui_call_sync(build_ui_on_host,NULL,25000)!=0){
          (void)svc->ui_call_sync(teardown_ui_on_host,NULL,2000);
          io_finish();svc->mem_free(g_canvas);g_canvas=NULL;return -3;
        }
        built_layout=ui_layout_kind();
        built_plain_list=built_layout==0;
      }
      else (void)svc->ui_call_sync(update_selection_on_host,NULL,1000);
      load_background_image();
      load_apps();
      g_catalog_loading=false;
      g_catalog_stale=g_discard_io;
      if(!g_discard_io)(void)svc->ui_call_sync(update_selection_on_host,NULL,1000);
      if (!g_discard_io && g_resume_detail_record >= 0) {
        const int resume_record = g_resume_detail_record;
        g_resume_detail_record = -1;
        if (resume_record < g_app_count &&
            g_item_source[resume_record] == g_source) {
          (void)fetch_app_detail(resume_record);
          g_detail_index = g_discard_io ? -1 : resume_record;
          g_detail_focus = 3;
          detail_mode = !g_discard_io;
          svc->log_printf("launcher_ui: resuming app information page for %s\n",
                          g_detail_name);
        }
      }
    }
    svc->log_printf("launcher_ui source=%s apps=%d visible=%d favorites=%d\n",
                    g_favorites_mode ? "FAVORITES" : source_label(g_source),
                    g_app_count, g_view_count, g_favorite_count);
    for (int i = 0; !detail_mode && i < g_view_count && i < VISIBLE_ROWS; i++) {
      const int record = g_visible_indices[i];
      svc->log_printf("launcher_ui loaded item %d: name='%s' path='%s'\n",
                      i, g_names[record], g_paths[record]);
    }
    wait_for_input_release();

    bool list_ui=!detail_mode && !g_settings_mode && !g_rom_picker_active && g_source!=SOURCE_ARCHIVE;
    if(g_display && built_layout!=ui_layout_kind())(void)svc->ui_call_sync(teardown_ui_on_host,NULL,2000);
    int result = g_display ? svc->ui_call_sync(update_selection_on_host,NULL,1000) : svc->ui_call_sync(build_ui_on_host, NULL, 25000);
    if (result != 0) {
      svc->log_printf("launcher_ui: LVGL setup failed: %d\n", result);
      (void)svc->ui_call_sync(teardown_ui_on_host,NULL,2000);
      io_finish();svc->mem_free(g_canvas);g_canvas=NULL;return -3;
    }
    built_layout=ui_layout_kind();
    built_plain_list=list_ui;
    load_background_image();
    if(g_settings_mode && g_settings_scan_pending && !g_discard_io){
      g_settings_scan_pending=false;bool reload_network=g_settings_reload_network;g_settings_reload_network=false;refresh_settings_snapshot(reload_network);
    }
    svc->log_printf("launcher_ui: LVGL %s screen created through loader bridge\n",
                    source_label(g_source));

    uint32_t last_progress_update_ms = 0;
    uint32_t last_status_update_ms = 0;
  while (g_open_index == -1) {
      if(!g_settings_mode && !detail_mode && load_visible_metadata())g_metadata_pending=true;
      (void)flush_metadata_updates(false);
      if(g_open_index!=-1)break;
      const bool keyboard_changed = poll_usb_keyboard();
      const bool selection_changed = poll_controller();
      if(consume_c6_action())(void)svc->ui_call_sync(update_c6_progress_on_host,NULL,1000);
      if(consume_firmware_action())(void)svc->ui_call_sync(update_firmware_on_host,NULL,1000);
      const int wheel = __atomic_exchange_n(&g_mouse_wheel_pending, 0, __ATOMIC_ACQ_REL);
      if (wheel != 0)
        (void)svc->ui_call_sync(mouse_scroll_on_host, (void *)(intptr_t)wheel, 1000);
      poll_wifi_scan();
      poll_settings_connection();
      const uint32_t now_ms = (uint32_t)(svc->get_time_us() / 1000U);
      if (g_filter_dirty) {
        g_filter_dirty = false;
        apply_filter();
        (void)svc->ui_call_sync(update_selection_on_host, NULL, 1000);
      } else if (selection_changed || keyboard_changed) {
        if(!flush_metadata_updates(true))(void)svc->ui_call_sync(update_selection_on_host, NULL, 1000);
      }
      if (g_source == SOURCE_ARCHIVE && !g_favorites_mode &&
          !g_settings_mode && !detail_mode && g_archive_search_pending &&
          (uint32_t)(now_ms - g_archive_search_changed_ms) >= 550U) {
        g_archive_search_pending = false;
        archive_search_load();
        apply_filter();
        (void)svc->ui_call_sync(update_selection_on_host, NULL, 1000);
      }
      if (g_settings_dirty) {
        if (svc->settings_volume_set != NULL) svc->settings_volume_set(g_volume_level);
        if (svc->settings_brightness_set != NULL) svc->settings_brightness_set(g_brightness_level);
        g_settings_dirty = false;
      }
      if ((uint32_t)(now_ms - last_status_update_ms) >= 1000) {
        (void)svc->ui_call_sync(update_device_status_on_host, NULL, 1000);
        (void)svc->ui_call_sync(update_c6_progress_on_host,NULL,1000);
        (void)svc->ui_call_sync(update_firmware_on_host,NULL,1000);
        last_status_update_ms = now_ms;
      }
      refresh_pending_settings_scan();
      if(g_open_index!=-1)break;
      if(g_transfer_verify_pending){
        g_transfer_verify_pending=false;
        if(g_detail_index>=0 && g_detail_is_remote && strcmp(g_detail_name,g_transfer_name)==0){
          g_detail_is_installed=detail_package_present(g_detail_name,g_detail_local_path,sizeof(g_detail_local_path));
          g_detail_uninstall_confirm=false;
          (void)svc->ui_call_sync(update_selection_on_host,NULL,1000);
        }
      }
      if (g_transfer_busy && g_svc->store_install_status != NULL) {
        if ((uint32_t)(now_ms - last_progress_update_ms) >= 250) {
          poll_install_status();
          (void)svc->ui_call_sync(update_selection_on_host, NULL, 1000);
          last_progress_update_ms = now_ms;
        }
      }
      if (g_archive_download_active &&
          (uint32_t)(now_ms - last_progress_update_ms) >= 250U) {
        poll_archive_download();
        (void)svc->ui_call_sync(update_selection_on_host, NULL, 1000);
        last_progress_update_ms = now_ms;
      }
      const int pump_result=svc->ui_call_sync(pump_ui_on_host,NULL,3000);
      if(pump_result==-2 || pump_result==-3){
        /* A busy host/SD/RPC task can delay a queued UI call. Keep the
         * launcher alive and retry instead of treating that as app exit. */
        svc->delay_ms(20);
        continue;
      }
      if(pump_result!=0){
        svc->log_printf("launcher_ui: UI pump failed: %d\n",pump_result);
        break;
      }
      /* C6 jobs are asynchronous. Consume their actions here rather than
       * tearing down and recreating every LVGL object during verification. */
      if(consume_c6_action())(void)svc->ui_call_sync(update_c6_progress_on_host,NULL,1000);
      if(consume_firmware_action())(void)svc->ui_call_sync(update_firmware_on_host,NULL,1000);
      svc->delay_ms((int)g_next_handler_ms);
    }

    chosen = g_open_index;
 dispatch_action: ;
    if (g_settings_dirty) {
      if (svc->settings_volume_set != NULL) svc->settings_volume_set(g_volume_level);
      if (svc->settings_brightness_set != NULL) svc->settings_brightness_set(g_brightness_level);
      g_settings_dirty = false;
    }
    if(mutations_busy() && ((!detail_mode && (chosen>=0 || chosen==-60 || chosen==-35 || chosen==-36 || chosen==-37 || chosen==-65 || chosen==-71 || chosen==-72)) ||
       (detail_mode && chosen>=0 && chosen<g_detail_action_count-1))){
      copy_text(g_settings_message,sizeof(g_settings_message),"A transfer is active; wait for it to finish");
      wait_for_input_release();continue;
    }
    bool reuse_list_ui=built_plain_list && !detail_mode && !g_settings_mode && !g_keyboard_open && !g_rom_picker_active && g_source!=SOURCE_ARCHIVE && chosen==-3;
    const bool loading_detail=built_plain_list && !detail_mode && (chosen==-18 || chosen==-4);
    const bool retain_install=g_display && detail_mode && chosen==1 && g_detail_is_remote;
    const bool retain_page=g_display && !detail_mode && !g_rom_picker_active && chosen<=-3 && chosen!=-49;
    const bool preparing_rom=g_rom_picker_active && chosen==-60;
    if(!preparing_rom && !retain_install && !retain_page && !reuse_list_ui && !loading_detail)(void)svc->ui_call_sync(teardown_ui_on_host, NULL, 2000);
    if (g_rom_picker_active && chosen == -61) {
      g_rom_picker_active = false;
      wait_for_input_release();
      continue;
    }
    if (g_rom_picker_active && chosen == -60) {
      if (!rom_picker_set_selected_game()) {
        if(g_discard_io){chosen=g_open_index;g_open_index=-1;g_discard_io=false;goto dispatch_action;}
        /* Keep the picker and error visible so another ROM can be selected. */
        wait_for_input_release();
        continue;
      }
      g_rom_picker_active = false;
      chosen = g_rom_picker_app_index;
      detail_mode = false;
      g_detail_index = -1;
      release_detail_cover();
    }
    if (chosen == -3) {
      if(g_keyboard_layout==(int)g_keyboard_open)g_catalog_stale=true;
      if (g_settings_mode && g_archive_auth_edit_field >= 0 && !g_keyboard_open) {
        commit_archive_auth_edit();
        wait_for_input_release();
        continue;
      }
      if (g_settings_mode && g_network_edit_field >= 0 && !g_keyboard_open) {
        (void)commit_network_edit();
        wait_for_input_release();
        continue;
      }
      detail_mode = false;
      g_detail_index = -1;
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -40) {
      begin_archive_auth_edit(0);
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -41) {
      begin_archive_auth_edit(1);
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -44) {
      begin_archive_auth_edit(2);
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -45) {
      begin_archive_auth_edit(3);
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -46) {
      archive_s3_check_action();
      if(g_discard_io){chosen=g_open_index;g_open_index=-1;g_discard_io=false;goto dispatch_action;}
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -47) {
      clear_archive_s3_keys();
      copy_text(g_settings_message, sizeof(g_settings_message),
                has_prefix(g_archive_s3_status, "Keys cleared") ?
                "Archive S3 keys cleared from SD card" :
                "Could not remove saved Archive S3 keys");
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -42) {
      archive_account_action();
      if(g_discard_io){chosen=g_open_index;g_open_index=-1;g_discard_io=false;goto dispatch_action;}
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -17) {
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -14) {
      g_catalog_stale=true;
      archive_open_selected();
      if (g_open_index == -3) {
        wait_for_input_release();
        continue;
      }
      /* A file transfer runs independently. Rebuild the same archive page so
       * the user can see its byte progress and continue browsing. */
      g_open_index = -1;
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -15) {
      g_catalog_stale=true;
      if (g_archive_level == ARCHIVE_LEVEL_ZIP)
        g_archive_level = ARCHIVE_LEVEL_ITEM;
      else if (g_archive_level == ARCHIVE_LEVEL_ITEM)
        g_archive_level = ARCHIVE_LEVEL_SEARCH;
      g_selected = 0;
      g_first_visible = 0;
      g_toolbar_focus = -1;
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -5) {
      g_toolbar_focus = -1;
      toggle_selected_favorite();
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -6) {
      g_toolbar_focus = -1;
      g_favorites_mode = !g_favorites_mode;
      g_catalog_stale=true;
      g_settings_mode = false;
      g_selected = 0;
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -7) {
      g_toolbar_focus = -1;
      g_settings_mode = true;
      g_settings_focus = 18;
      g_keyboard_open = false;
      g_network_edit_field = -1;
      g_settings_message[0] = '\0';
      load_archive_credentials();
      load_archive_s3_keys();
      g_settings_scan_pending=true;g_settings_reload_network=true;
      wait_for_input_release();
      continue;
    }
    if(!detail_mode && chosen==-75){
      g_tooltips_enabled=!g_tooltips_enabled;
      copy_text(g_settings_message,sizeof(g_settings_message),save_tooltips_preference() ? "Tooltip preference saved on SD card" : "Tooltip preference applied; SD unavailable for saving");
      g_settings_focus=37;wait_for_input_release();continue;
    }
    if(!detail_mode && chosen==-49){
      g_theme_index=(g_theme_index+g_theme_direction+g_theme_count)%g_theme_count;
      g_theme=g_themes[g_theme_index];g_theme_direction=1;
      copy_text(g_settings_message,sizeof(g_settings_message),save_theme() ? "Theme saved; Left/Right changes theme" : "Theme applied; SD card unavailable for saving");
      g_settings_focus=24;wait_for_input_release();continue;
    }
    if(!detail_mode && chosen==-68){
      g_size_unit=(g_size_unit+g_size_direction+PAX_SIZE_COUNT)%PAX_SIZE_COUNT;g_size_direction=1;
      copy_text(g_settings_message,sizeof(g_settings_message),save_list_preferences() ? "Size units saved (KB = 1000 bytes)" : "Size units applied; SD unavailable for saving");
      g_settings_focus=30;wait_for_input_release();continue;
    }
    if(!detail_mode && chosen==-67){
      g_sort_mode=(g_sort_mode+g_sort_direction+PAX_SORT_COUNT)%PAX_SORT_COUNT;g_sort_direction=1;
      g_selected=0;g_first_visible=0;apply_filter();
      copy_text(g_settings_message,sizeof(g_settings_message),save_list_preferences() ? "List sort saved; Left/Right changes order" : "Sort applied; SD card unavailable for saving");
      g_settings_focus=29;wait_for_input_release();continue;
    }
    if(!detail_mode && chosen==-48){
      copy_text(g_settings_message,sizeof(g_settings_message),save_list_preferences() ? "App list preference saved on SD card" : "Could not save preference; SD card may be unavailable");
      wait_for_input_release();continue;
    }
    if (!detail_mode && chosen == -43) {
      copy_text(g_settings_message, sizeof(g_settings_message),
                save_app_details_preference() ? "App details preference saved on SD card" :
                                                "Could not save preference; SD card may be unavailable");
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -8) {
      g_keyboard_open = !g_keyboard_open;
      g_toolbar_focus = g_keyboard_open ? 7 : -1;
      g_keyboard_focus = 0;
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -9) {
      g_catalog_stale=true;
      g_settings_mode = false;
      secure_clear(g_archive_password, sizeof(g_archive_password));
      g_archive_auth_edit_field = -1;
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -13) {
      g_settings_mode = false;
      g_favorites_mode = false;
      g_source = SOURCE_ARCHIVE;
      g_catalog_stale=true;
      g_archive_level = ARCHIVE_LEVEL_SEARCH;
      copy_text(g_search_text, sizeof(g_search_text), "collection:");
      g_selected = 0;
      g_first_visible = 0;
      g_toolbar_focus = -1;
      g_keyboard_open = true;
      g_keyboard_focus = 0;
      g_archive_search_pending = false;
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -11) {
      toggle_auto_brightness();
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -10) {
      save_background_preference();
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen <= -30 && chosen >= -33) {
      begin_network_edit(-30 - chosen);
      wait_for_input_release();
      continue;
    }
    if(!detail_mode && chosen==-80){
      request_wifi_scan();
      if(g_discard_io){chosen=g_open_index;g_open_index=-1;g_discard_io=false;goto dispatch_action;}
      wait_for_input_release();continue;
    }
    if(!detail_mode && (chosen==-81 || (chosen<=-1000 && chosen>-1000-(int)PAX_WIFI_SCAN_MAX))){
      g_wifi_dropdown_open=false;(void)svc->ui_call_sync(wifi_dropdown_state_on_host,NULL,1000);
      if(chosen==-81){g_wifi_needs_password=false;begin_network_edit(5);}
      else if(select_wifi_network(-1000-chosen)){
        if(g_wifi_needs_password)begin_network_edit(6);
        else (void)svc->ui_call_sync(update_selection_on_host,NULL,1000);
      }
      wait_for_input_release();continue;
    }
    if(!detail_mode && chosen<=-62 && chosen>=-64){begin_network_edit(4+(-62-chosen));wait_for_input_release();continue;}
    if(!detail_mode && chosen==-70){begin_network_edit(7);wait_for_input_release();continue;}
    if(!detail_mode && chosen==-71){request_c6_update(PAX_C6_PREPARE);wait_for_input_release();continue;}
    if(!detail_mode && chosen==-72){request_c6_update(PAX_C6_INSTALL);wait_for_input_release();continue;}
    if(!detail_mode && chosen==-65){apply_wifi_settings();if(g_discard_io){chosen=g_open_index;g_open_index=-1;g_discard_io=false;goto dispatch_action;}wait_for_input_release();continue;}
    if (!detail_mode && chosen == -34) {
      g_network_config.dhcp_enabled = !g_network_config.dhcp_enabled;g_network_config_dirty=true;
      copy_text(g_settings_message, sizeof(g_settings_message), "DHCP mode staged; select APPLY NETWORK");
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -35) {
      if (svc->network_config_set == NULL) {
        copy_text(g_settings_message, sizeof(g_settings_message), "Network configuration service unavailable");
      } else {
        pax_network_config_t request=g_network_config;
        const int result = run_action(network_apply_job,&request,"Applying network... Please wait");
        if (result == 0) {
          copy_text(g_settings_message, sizeof(g_settings_message),
                    "Network config saved; the connection may briefly restart");
          g_network_config_dirty=false;
          if(!g_discard_io)refresh_settings_snapshot(false);
        } else {
          copy_text(g_settings_message, sizeof(g_settings_message),
                    result == -2 ? "Network interface is not available" :
                    (result == -3 ? "Network applied, but could not save it" :
                                    "Network settings rejected; check the IPv4 values"));
        }
      }
      if(g_discard_io){chosen=g_open_index;g_open_index=-1;g_discard_io=false;goto dispatch_action;}
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && (chosen == -36 || chosen == -37)) {
      const int storage = chosen == -36 ? PAX_STORAGE_SD : PAX_STORAGE_USB;
      if (svc->storage_set_mounted == NULL) {
        copy_text(g_settings_message, sizeof(g_settings_message), "Storage controls unavailable");
      } else {
        const int should_mount = !g_storage_status[storage].mounted;
        MountJob request={storage,should_mount};
        const int result = run_action(mount_job,&request,should_mount ? "Mounting storage... Please wait" : "Safely ejecting storage... Please wait");
        g_catalog_stale=true;g_settings_scan_pending=true;
        if (result == 0) {
          copy_text(g_settings_message, sizeof(g_settings_message),
                    should_mount ? "Storage mounted" : "Storage safely ejected");
          if(!g_discard_io){g_settings_scan_pending=false;refresh_settings_snapshot(false);}
        } else {
          copy_text(g_settings_message, sizeof(g_settings_message),
                    should_mount ? (storage == PAX_STORAGE_USB ?
                                    (result == -3 ? "USB drive detected but mount failed; check its filesystem" :
                                                   "Mount failed; connect a USB drive first") :
                                    "SD mount failed; check the card and filesystem") :
                                   "Could not safely eject storage");
          if(result==-4)copy_text(g_settings_message,sizeof(g_settings_message),"Storage is busy with a transfer or firmware update");
          if(!g_discard_io){g_settings_scan_pending=false;refresh_settings_snapshot(false);}
        }
      }
      if(g_discard_io){chosen=g_open_index;g_open_index=-1;g_discard_io=false;goto dispatch_action;}
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && chosen == -12) {
      g_search_text[0] = '\0';
      g_filter_dirty = false;
      if (g_source == SOURCE_ARCHIVE && g_archive_level == ARCHIVE_LEVEL_SEARCH) {
        g_archive_search_pending = true;
        g_archive_search_changed_ms = (uint32_t)(svc->get_time_us() / 1000U);
      }
      apply_filter();
      g_toolbar_focus = 8;
      wait_for_input_release();
      continue;
    }
    if (!detail_mode && (chosen == -18 || chosen == -4)) {
      const int selected_record = selected_record_index();
      if (selected_record < 0 || g_item_source[selected_record] == SOURCE_ARCHIVE) continue;
      /* Keep the list interactive while reading details. Detail navigation
       * becomes active only after its model is ready. */
      g_detail_index = -1;
      g_detail_focus = 0;
      g_open_index = -1; /* A second row click must be a new action. */
      (void)fetch_app_detail(selected_record);
      if(g_discard_io){
        detail_mode=false;chosen=g_open_index;g_discard_io=false;
        g_open_index=-1;
        goto dispatch_action;
      }
      g_detail_index=selected_record;
      detail_mode = true;
      wait_for_input_release();
      continue;
    }
    if (detail_mode && chosen == 1) {
      if (!g_detail_is_remote) {
        /* Keep update installs on the existing Store detail path so the
         * package hash and size are still checked before writing anything. */
        g_source = SOURCE_STORE;g_catalog_stale=true;
        g_favorites_mode = false;
        g_settings_mode = false;
        copy_text(g_search_text, sizeof(g_search_text), g_detail_name);
        g_selected = 0;
        g_first_visible = 0;
        detail_mode = false;
        g_detail_index = -1;
        release_detail_cover();
        wait_for_input_release();
        continue;
      } else if (!g_detail_install_valid || svc->store_install_start == NULL) {
        copy_text(g_install_status, sizeof(g_install_status), "This listing cannot be installed safely.");
      } else {
        InstallJob request={0};
        copy_text(request.url,sizeof(request.url),g_paths[g_detail_index]);
        copy_text(request.name,sizeof(request.name),g_detail_name);
        copy_text(request.title,sizeof(request.title),g_detail_title);
        copy_text(request.version,sizeof(request.version),g_detail_version);
        copy_text(request.sha256,sizeof(request.sha256),g_detail_sha256);request.size=g_detail_size;
        const int install_result=run_action(install_start_job,&request,"Starting install... Please wait");
        if (install_result == 0) {
          g_install_state = 1;g_transfer_busy=true;copy_text(g_transfer_name,sizeof(g_transfer_name),request.name);
          copy_text(g_transfer_message,sizeof(g_transfer_message),"Preparing install...");work_status(NULL);
          copy_text(g_install_status, sizeof(g_install_status), "Install started; preparing files...");
        } else {
          g_install_state = 3;
          copy_text(g_install_status, sizeof(g_install_status), install_result == -2 ?
                    "Another install is already running." : "Install could not be started.");
        }
      }
      if(g_discard_io){chosen=g_open_index;g_open_index=-1;g_discard_io=false;goto dispatch_action;}
      wait_for_input_release();
      continue;
    }
    if (detail_mode && chosen == 2) {
      if (!g_detail_is_installed) {
        copy_text(g_install_status, sizeof(g_install_status), "This app is not installed locally.");
      } else if (!g_detail_uninstall_confirm) {
        g_detail_uninstall_confirm = true;
        copy_text(g_install_status, sizeof(g_install_status),
                  "Select CONFIRM REMOVE again to delete the app package and its JSON listing.");
      } else if (!remove_detail_package()) {
        g_detail_uninstall_confirm = false;
        copy_text(g_install_status, sizeof(g_install_status), "Could not remove app; check storage status.");
      }
      wait_for_input_release();
      continue;
    }
    if (detail_mode && chosen == 3 && g_detail_extract_recipe.valid) {
      pax_file_stat_t installer_info = {0};
      if (svc->app_open == NULL || svc->file_stat == NULL ||
          svc->file_stat(DISC_INSTALLER_PAX, &installer_info) != 0 ||
          installer_info.is_dir || installer_info.size == 0) {
        copy_text(g_install_status, sizeof(g_install_status),
                  "recipe.pax is missing from /sd/roms/pax.");
      } else if (g_detail_recipe_path[0] == '\0') {
        copy_text(g_install_status, sizeof(g_install_status),
                  "This recipe is not available as a local JSON file.");
      } else {
        launch_disc_installer = true;
        chosen = -100;
        break;
      }
      wait_for_input_release();
      continue;
    }
    if (detail_mode && chosen == g_detail_action_count - 1) {
      g_catalog_stale=true;
      detail_mode = false;
      g_detail_index = -1;
      release_detail_cover();
      wait_for_input_release();
      continue;
    }
    if (detail_mode && chosen == 0) {
      if (g_detail_index >= 0 && g_detail_index < g_app_count &&
          pax_rom_profile_for_app(g_paths[g_detail_index]) != NULL) {
        rom_picker_load_games(g_paths[g_detail_index]);
        g_rom_picker_app_index = g_detail_index;
        g_rom_picker_active = true;
        svc->log_printf("launcher_ui: opening ROM picker (%d local games)\n",
                        g_rom_picker_count);
        wait_for_input_release();
        continue;
      }
      chosen = g_detail_index;
      detail_mode = false;
      g_detail_index = -1;
      release_detail_cover();
    }
    if (!detail_mode && !g_rom_picker_active && !g_rom_picker_rom_selected &&
        chosen >= 0 && chosen < g_app_count &&
        pax_rom_profile_for_app(g_paths[chosen]) != NULL) {
      rom_picker_load_games(g_paths[chosen]);
      g_rom_picker_app_index = chosen;
      g_rom_picker_active = true;
      svc->log_printf("launcher_ui: opening ROM picker (%d local games)\n",
                      g_rom_picker_count);
      wait_for_input_release();
      continue;
    }
    break;
  }
  if(g_display)(void)svc->ui_call_sync(teardown_ui_on_host,NULL,2000);
  if (g_background_pixels != NULL) {
    svc->mem_free(g_background_pixels);
    g_background_pixels = NULL;
  }
  release_detail_cover();
  io_finish();
  log_timing("pump",&g_pump_time);log_timing("flush",&g_flush_time);
  log_timing("action_to_flush",&g_action_time);log_timing("image_job",&g_image_time);log_timing("storage_scan",&g_scan_time);
  svc->log_printf("launcher_ui render: builds=%u rows=%u labels=%u styles=%u keyboard=%u metadata=%u\n",g_ui_builds,g_row_updates,g_label_updates,g_button_updates,g_keyboard_transitions,g_metadata_flushes);
  svc->log_printf("launcher_ui I/O: %u jobs, %u cancelled\n",g_io_jobs,g_io_cancelled);
  if (launch_disc_installer && svc->app_open != NULL) {
    save_launcher_resume_state();
    if (svc->app_open(DISC_INSTALLER_PAX, g_detail_recipe_path, 1) == 0)
      svc->log_printf("launcher_ui: installer returned; reopening %s information\n",
                      g_detail_name);
    else
      svc->log_printf("launcher_ui: could not open %s\n", DISC_INSTALLER_PAX);
  } else if (chosen >= 0 && chosen < g_app_count && svc->app_open != NULL) {
    save_launcher_resume_state();
    if (svc->app_open(g_paths[chosen], g_paths[chosen], 1) == 0)
      svc->log_printf("launcher_ui: opening %s and returning here\n", g_paths[chosen]);
    else
      svc->log_printf("launcher_ui: could not open %s\n", g_paths[chosen]);
  } else {
    svc->log_printf("launcher_ui: returning to dashboard\n");
  }
  svc->display_clear(0x0000);
  svc->display_flush();
  svc->mem_free(g_canvas);
  g_canvas = NULL;
  return 0;
}
