#pragma once
#include "pax_launcher_regions.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_timer.h"
#include "esphome/core/hal.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/display/display.h"
#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/touchscreen/touchscreen.h"
#ifdef PAX_LOADER_USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif
#ifdef PAX_LOADER_USE_USB_HIDX
#include "esphome/components/usb_hidx/usb_hidx.h"
#endif
#ifdef PAX_LOADER_USE_USB_MIDI
#include "esphome/components/usb_midi/usb_midi.h"
#endif
#ifdef PAX_LOADER_USE_USB_MSC
#include "esphome/components/usb_msc/usb_msc.h"
#endif
#ifdef PAX_LOADER_USE_SD_MMC
#include "esphome/components/sd_mmc_card/sd_mmc_card.h"
#endif

#include "pax_canvas.h"
#include "pax_sound.h"
#include "pax_data.h"
#include "pax_files.h"
#include "pax_handoff.h"
#include "psram_app.h"

namespace esphome::mipi_dsi {
class MIPI_DSI;
}

namespace esphome {
namespace pax_loader {

// A button in a library source's side menu (YAML catalogs: actions:).
class CatalogActionTrigger : public Trigger<> {};

class PaxLoader : public Component {
 public:
  static constexpr uint8_t BUTTON_COUNT = PAX_INPUT_MAX;

  void set_path(const std::string &path) { this->path_ = path; }
  void set_catalog_url(const std::string &url) { this->catalog_url_ = url; }
  void refresh_catalog();
  // Named library sources (YAML `catalogs:`): an HTTP catalog page, or a folder
  // such as /sd/roms/pax/ whose .pax files are listed from every data
  // search root (/sd, /usb0, ...). With more than one, the library list starts
  // with a "◀ name ▶" row; tap it, or press left/right, to switch.
  void add_catalog(const std::string &name, const std::string &url) { this->catalogs_.push_back({name, url}); }
  // Show the catalog with this name (case-insensitive). Unknown names are ignored.
  void select_catalog(const std::string &name);
  void select_catalog_index(size_t index);
  // Step through the catalogs: +1 next, -1 previous (wraps around).
  void next_catalog(int step = 1);
  // Native Archive.org ZIP-member sources. The URL is the Archive.org ZIP
  // URL shown by "View Contents"; the loader fetches its indexed member list
  // and saves a selected member below the destination on the SD card.
  void add_archive_source(const std::string &name, const std::string &url,
                          const std::string &destination, const std::string &extensions) {
    this->archive_sources_.push_back({name, url, destination, extensions});
  }
  void refresh_archive();
  void set_archive_search_collection(const std::string &collection) { this->archive_search_collection_ = collection; }
  void show_archive_search();
  void show_archive_systems();
  void open_archive_system(size_t system_index);
  void select_archive_result(size_t result_index);
  void open_archive_for_current_rom_selector();
  void back_from_archive();
  // The archive page opens on collection search. This helper keeps the YAML
  // page from resetting an in-progress search when LVGL calls on_load().
  bool is_archive_in_games_mode() const { return this->archive_mode_ != ARCHIVE_VIEW_SYSTEMS; }
  // ESPHOMEBREW store view (YAML `library_style: grid`): the library shows
  // icon tiles instead of a list, and tapping an app opens its detail page
  // (about, controls, sizes, Stream / Install / Launch / Update). App info
  // comes from the <app>.json next to each .pax (see docs/building.md).
  void set_store_ui(bool enabled) { this->store_ui_ = enabled; }
  // Where Install puts apps (as <name>.pax plus its <name>.json).
  void set_install_dir(const std::string &dir) { this->install_dir_ = dir; }
#ifdef PAX_LOADER_USE_SD_MMC
  // Install can retry a card mount after reboot, eject, or late insertion.
  void set_sd_mmc(sd_mmc_card::SdMmc *card) { this->sd_mmc_ = card; }
#endif
  // The store view's side menu, which slides out from the right edge: Refresh
  // plus these buttons for the source on screen (YAML `actions:` of a catalog).
  void add_catalog_action(size_t catalog, const std::string &label, Trigger<> *trigger) {
    if (catalog < this->catalogs_.size())
      this->catalogs_[catalog].actions.emplace_back(label, trigger);
  }
  void set_side_menu(bool open);
  void toggle_side_menu() { this->set_side_menu(!this->side_menu_open_); }
  // The catalog shown first (YAML `default_catalog`); set before setup().
  void set_initial_catalog(size_t index) {
    if (index < this->catalogs_.size()) {
      this->catalog_index_ = index;
      this->catalog_url_ = this->catalogs_[index].url;
    }
  }
  // When set, every app run ends with a JSON test report POSTed here: the app,
  // how it ended, its return code or load error, runtime and the tail of its log.
  void set_report_url(const std::string &url) { this->report_url_ = url; }
  void set_report_log_bytes(size_t bytes) { this->report_log_bytes_ = bytes; }
  // App data: before a network launch the loader reads <app>.files next to the
  // .pax and downloads every listed file that is missing under data_root.
  // Files already on the card are never replaced.
  void set_data_root(const std::string &root) { this->data_root_ = root; }
  void set_download_data(bool download) { this->download_data_ = download; }
  // Storage roots (for example /sd, /usb0) where the user may already have an
  // app's files. A file found under any of them is not downloaded, and an app
  // reading /sd/<path> that is not on the card gets <root>/<path> instead.
  void add_data_search_root(const std::string &root) { this->data_search_.push_back(root); }
  // Launch progress for a UI: true while a network app (and its data) loads,
  // a fraction 0..1 (-1 when unknown or idle), and a one-line status that
  // stays after a failure until the next launch ("" when there is nothing to say).
  bool is_loading() const { return this->pax_loading_; }
  float get_load_progress();
  std::string get_load_status();
  // Called by the download code on the loader task; thread safe.
  void set_progress_(bool active, uint32_t done, uint32_t total, const char *format, ...)
      __attribute__((format(printf, 5, 6)));
  // The web component reserves storage on the main loop before doing I/O on its worker.
  bool begin_web_transfer();
  void end_web_transfer();
  // Worker-only snapshot; caller owns the returned heap_caps allocation.
  bool capture_screenshot(uint16_t *&copy, uint16_t &width, uint16_t &height);
  using WebRead = int (*)(void *, uint8_t *, size_t);
  esp_err_t receive_web_file(const std::string &path, uint32_t size, const std::string &sha256,
                             WebRead read, void *context);
  esp_err_t download_web_file(const data::DataFile &file);
  std::atomic<bool> web_storage_busy_{false};
  void request_launch(const std::string &path) {
    if (this->web_storage_busy_) {
      ESP_LOGW("pax_loader", "Wait for the SD transfer before launching");
      return;
    }
    if (path.empty()) {
      ESP_LOGW("pax_loader", "Ignoring empty PAX launch request");
      return;
    }
    if (this->launched_) {
      ESP_LOGW("pax_loader", "Ignoring PAX launch while another app is running: %s", path.c_str());
      return;
    }
    if (this->launch_pending_) {
      ESP_LOGW("pax_loader", "Ignoring duplicate PAX launch request: %s", path.c_str());
      return;
    }
    this->path_ = path;
    this->chain_launch_ = false;  // no argument; an app hand-off chain is forgotten
    this->launch_pending_ = true;
    ESP_LOGI("pax_loader", "Launch requested from menu: %s", path.c_str());
  }
  void request_launch_url(const std::string &url) { this->request_launch(url); }
  // Remote app arguments use the same hand-off ABI as the launcher, without
  // adding a return chain. Runs on the ESPHome main loop like request_launch.
  void request_launch_with_arg(const std::string &path, const std::string &arg) {
    if (arg.size() >= PAX_APP_ARG_MAX || this->launched_ || this->launch_pending_ || this->web_storage_busy_)
      return;
    this->request_launch(path);
    if (!this->launch_pending_) return;
    if (this->handoff_mutex_) xSemaphoreTake(this->handoff_mutex_, portMAX_DELAY);
    this->chain_.reset();
    this->next_arg_ = arg;
    this->chain_launch_ = true;
    if (this->handoff_mutex_) xSemaphoreGive(this->handoff_mutex_);
  }
  // Select a local emulator PAX and a ROM/game path from ESPHome's native API
  // or a template text entity. The ROM path must be under /sd or /usb0; the
  // loader stores it in the shared PAX setting before starting the emulator.
  void request_launch_rom(const std::string &emulator, const std::string &rom_path);
  // Download a PAX into install_dir without requiring a store listing. The
  // caller supplies the expected size and SHA-256 so a local HTTP helper can
  // safely stage a build onto the SD card before launching it from Storage.
  void request_install_url(const std::string &url, const std::string &name, uint32_t size,
                           const std::string &sha256);
  // Download any regular file into a validated path under /sd. The transfer is
  // staged as <path>.part and only becomes visible after the supplied size and
  // SHA-256 have been verified. For native Archive.org members both metadata
  // arguments may be zero/empty; the response length is used when available.
  // Refused while an app or another storage operation runs.
  void request_upload_url(const std::string &url, const std::string &path, uint32_t size,
                          const std::string &sha256);
  // Same staged writer, narrowly allowed while launcher_ui.pax is active.
  void request_archive_download_url(const std::string &url, const std::string &path);
  void request_upload_url_(const std::string &url, const std::string &path, uint32_t size,
                           const std::string &sha256, bool allow_launcher);
  void request_close() {
    if (!this->launched_) {
      ESP_LOGW("pax_loader", "Ignoring PAX close request because no app is running");
      return;
    }
    this->begin_close_();
    ESP_LOGI("pax_loader", "Close requested remotely");
  }
  void request_screen_stream() {
    this->stream_enabled_ = true;
    ESP_LOGI("pax_loader", "Screen stream enabled remotely");
  }
  // One full capture of the running PAX's canvas (800x480 unless the app
  // chose another size), sent to the next (or current) client of the
  // diagnostic stream on TCP port 3233 as a PAXSS01 packet. With no app
  // running it is the menu (or an empty 0x0 packet).
  void request_screenshot() {
    this->screenshot_requested_ = true;
    this->stream_enabled_ = true;
    ESP_LOGI("pax_loader", "Screenshot requested remotely");
  }
  // Test-only remote input for launcher_ui. Accepts taps, timed holds/swipes,
  // all launcher gamepad buttons, wheel scrolling, keyboard keys and text;
  // requests are ignored unless launcher_ui.pax is active.
  void request_remote_ui_input(const std::string &command);
  // One file from the SD card (or, for a path ending in '/', a directory
  // listing), sent to the diagnostic stream as a PAXFL01 packet. Only paths
  // under /sd/ are served (bridge `readfile`).
  void request_file(const std::string &path);
  // Replaces a small text file under /sd/ (bridge `writefile`), keeping the old
  // one as <path>.bak. Refused while an app runs, since the app may rewrite it.
  void write_file(const std::string &path, const std::string &data);
  // Removes one text or leftover file (.bak, .log) under /sd/ (bridge
  // `deletefile`). Never game data or apps; refused while an app runs.
  void delete_file(const std::string &path);
  void set_autostart(bool autostart) { this->autostart_ = autostart; }
  // ── Canvas size (pax_canvas.h) ──
  // The panel's size (YAML panel_width/panel_height); by default the
  // display's own. Frame buffers are allocated for a canvas this large.
  void set_panel_size(int width, int height) {
    this->panel_config_w_ = width;
    this->panel_config_h_ = height;
  }
  // The canvas offered to apps that ask for one (display_get_size) and have no
  // per-app setting (YAML canvas_width/canvas_height); by default the panel.
  // Apps that never ask keep 800x480.
  void set_default_canvas(int width, int height) {
    this->default_canvas_w_ = width;
    this->default_canvas_h_ = height;
  }
  int get_panel_width() const { return this->geometry_.panel_w; }
  int get_panel_height() const { return this->geometry_.panel_h; }
  int get_default_canvas_width() const { return this->default_canvas_w_; }
  int get_default_canvas_height() const { return this->default_canvas_h_; }
  // The per-app Screen setting ("1024x600", or "" for the device default), kept
  // in <install_dir>/settings.json by app name (canvas::app_key: the .pax's
  // file name without folders, ".pax" and a "-<version>" suffix). set returns
  // false for a size this device cannot show, or when the file can't be written.
  std::string get_app_canvas(const std::string &app);
  bool set_app_canvas(const std::string &app, const std::string &size);
  // The sizes the Screen setting offers after "Default" (canvas::choices): the
  // app's listed sizes that fit, or the panel, 1024x600, 800x480 and 640x480.
  std::vector<std::string> canvas_choices(const std::vector<std::string> &listed = {}) const {
    return canvas::choices(this->max_canvas_w_, this->max_canvas_h_, listed);
  }
  void set_display(display::Display *display) { this->display_ = display; }
  void set_mipi_dsi(mipi_dsi::MIPI_DSI *display) { this->mipi_dsi_ = display; }
  void set_native_mipi_dsi(mipi_dsi::MIPI_DSI *display) { this->native_mipi_dsi_ = display; }
  void set_touchscreen(touchscreen::Touchscreen *touchscreen) { this->touchscreen_ = touchscreen; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  // Master output volume shared by every PAX. The value is a percentage and
  // is applied immediately when an app is playing and again when its audio
  // stream is initialized.
  void set_master_volume(int32_t level);
  int32_t get_master_volume() const { return this->master_volume_; }
  // Display brightness shared by the launcher and PAX overlay. User-driven
  // changes use the same rate-limited UI feedback as the volume slider.
  void set_brightness_level(int32_t level);
  void set_auto_brightness_enabled(bool enabled);
  bool auto_brightness_enabled() const { return this->auto_brightness_enabled_; }
  void update_auto_brightness_();
  // Procedural UI sound effect playback (Nintendo Switch style clicks, snaps, chimes)
  void play_ui_sound(UiSound sound, uint8_t param = 0);
#ifdef PAX_LOADER_USE_LVGL
  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
  void set_lvgl_keypad_navigation(bool enabled) { this->lvgl_keypad_navigation_ = enabled; }
  void set_focus_tooltips(bool enabled);
  bool focus_tooltips_enabled() const { return this->focus_tooltips_enabled_; }
  void set_catalog_container(lv_obj_t *container) {
    this->catalog_container_ = container;
    this->favorites_page_active_ = false;
    this->catalog_ui_pending_ = true;
  }
  void set_catalog_search_widgets(lv_obj_t *search, lv_obj_t *clear) {
    this->catalog_search_widget_ = search;
    this->catalog_clear_search_widget_ = clear;
  }
  void set_catalog_refresh_button(lv_obj_t *button) {
    this->catalog_refresh_button_ = button;
  }
  // Optional page-specific back action placed in the pinned source/count bar.
  // Favorites uses this so its search, refresh, and back controls share one
  // fixed row above the scrolling list.
  void set_catalog_back_button(lv_obj_t *button) {
    this->catalog_back_button_ = button;
  }
  void set_archive_container(lv_obj_t *container) {
    this->archive_container_ = container;
    this->archive_ui_pending_ = true;
  }
  // The favorites page uses the same store grid and detail actions, but filters
  // the current catalog down to entries the user marked with the star action.
  void set_favorites_container(lv_obj_t *container) {
    this->catalog_container_ = container;
    this->favorites_page_active_ = true;
    this->catalog_ui_pending_ = true;
  }
  // Search text is kept separately for the app, ROM, and Archive.org pages.
  void set_catalog_search(const std::string &query);
  void set_rom_search(const std::string &query);
  void set_archive_search(const std::string &query);
  // Settings page status for the selected Archive.org collection and the
  // storage root where it is persisted.
  void set_archive_settings_label(lv_obj_t *label);
  // Background artwork is selected from /sd/backgrounds (and /usb0/backgrounds
  // when that root is configured). The selected virtual path is persisted next
  // to the launcher settings so it survives a reboot or page change.
  void set_background_controls(lv_obj_t *label, lv_obj_t *button) {
    this->background_label_ = label;
    this->background_next_button_ = button;
  }
  void refresh_backgrounds();
  void next_background();
  // Used by the YAML startup choice: favorites first, Store when none exist.
  bool has_favorites();
  // Optional ROM picker for emulator pages built in YAML. The picker scans
  // every configured data root (for example /sd and /usb0), and launching a
  // row sets the shared ROM-path service before starting the named PAX.
  void set_rom_selector_container(lv_obj_t *container) { this->rom_selector_container_ = container; }
  void back_from_rom_selector();
  void configure_rom_selector(const std::string &app, const std::string &folder,
                              const std::string &extensions) {
    this->rom_selector_app_ = app;
    this->rom_selector_folder_ = folder;
    this->rom_selector_extensions_ = extensions;
    this->rom_selector_source_.clear();
    this->rom_selector_sidecar_ = "/sd/roms/pax/" + app + ".rom";
  }
  // Open the shared ROM picker for a cartridge-style emulator PAX.  The
  // mapping mirrors the standalone RetroESP32-P4 launcher: it derives the
  // system from the .pax name, scans /sd and /usb0, and filters each list to
  // that emulator's accepted ROM extensions.  Returns false for data-driven
  // ports (Doom, Quake, ScummVM, etc.) which should launch directly.
  bool open_rom_selector_for_app(const std::string &source);
  bool supports_rom_selector(const std::string &source) const;
  void refresh_rom_selector();
  void set_rom_selector_favorites_button(lv_obj_t *button) { this->rom_selector_favorites_button_ = button; }
  void set_rom_selector_favorites_filter_button(lv_obj_t *button) {
    this->rom_selector_favorites_filter_button_ = button;
  }
  void set_rom_selector_position_label(lv_obj_t *label) { this->rom_selector_position_label_ = label; }
  void toggle_rom_favorites();
  void toggle_selected_rom_favorite();
  void select_rom(const std::string &path);
  void handle_launcher_controls_();
  void set_catalog_selection_(uint16_t index);
  void ensure_catalog_volume_control_();
  // The drawer is global to the launcher and must also be available when a
  // ROM selector is entered before the first catalog refresh has completed.
  void ensure_catalog_drawer();
  void raise_catalog_volume_control_();
  void set_catalog_volume_control_visible_(bool visible);
  void set_brightness_number(number::Number *number) { this->catalog_brightness_number_ = number; }
  void add_hardware_sensor(const std::string &name,const std::string &unit,sensor::Sensor *sensor);
  void add_hardware_binary(const std::string &name,const std::string &unit,binary_sensor::BinarySensor *sensor);
  void add_hardware_number(const std::string &name,const std::string &unit,number::Number *number);
  void set_ambient_light_sensor(sensor::Sensor *sensor) { this->ambient_light_sensor_ = sensor; }
  void toggle_catalog_volume_slider();
  void toggle_catalog_brightness_slider();
  void hide_catalog_sliders();
  void ensure_catalog_brightness_control_();
  void update_catalog_status_();
  // The launcher YAML supplies the Font Awesome font used by its icon labels;
  // the loader-created volume control needs the same font at runtime.
  void set_catalog_volume_font(const lv_font_t *font) {
    this->catalog_volume_font_ = font;
    if (this->catalog_volume_button_ != nullptr) {
      lv_obj_t *label = lv_obj_get_child(this->catalog_volume_button_, 0);
      if (label != nullptr)
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
  }
  // Store memory controls.  Low-memory mode keeps the catalog as a plain file
  // listing and skips all per-app JSON/icon fetches until the user turns the
  // richer store view back on.
  void set_store_thumbnails_enabled(bool enabled);
  bool store_thumbnails_enabled() const { return this->store_thumbnails_enabled_; }
  void set_store_info_enabled(bool enabled);
  bool store_info_enabled() const { return this->store_info_enabled_; }
  void set_store_low_memory_mode(bool enabled);
  bool store_low_memory_mode() const { return !this->store_thumbnails_enabled_ && !this->store_info_enabled_; }
  // Release launcher-owned LVGL/listing resources before a game starts. The
  // catalog is rebuilt after the game returns; settings and the selected
  // source remain resident so the launcher comes back to the same page.
  void release_launcher_runtime_();
  // Optional progress widgets the loader keeps up to date while an app and
  // its data download: `fill` is an object inside a track object; its width is
  // set to the percentage done and the track (its parent) is hidden when idle.
  // `label` shows the status line. Either may be null. Plain objects and a
  // label keep this independent of which LVGL widgets the config enables.
  void set_progress_widgets(lv_obj_t *fill, lv_obj_t *label) {
    this->progress_fill_ = fill;
    this->progress_label_ = label;
    this->progress_ui_seq_ = this->progress_seq_ - 1;  // redraw on the next loop
  }
#endif
  void set_toggle_button(binary_sensor::BinarySensor *sensor) { this->toggle_button_ = sensor; }
  void set_launch_button(binary_sensor::BinarySensor *sensor) { this->launch_button_ = sensor; }
  void set_fire_button(binary_sensor::BinarySensor *sensor) { this->fire_button_ = sensor; }
  void set_touch_button(binary_sensor::BinarySensor *sensor) { this->touch_button_ = sensor; }
  void set_adc_button_sensor(sensor::Sensor *sensor) { this->adc_button_sensor_ = sensor; }
  void set_left_stick_x_sensor(sensor::Sensor *sensor) { this->left_stick_x_sensor_ = sensor; }
  void set_left_stick_y_sensor(sensor::Sensor *sensor) { this->left_stick_y_sensor_ = sensor; }
  void set_right_stick_x_sensor(sensor::Sensor *sensor) { this->right_stick_x_sensor_ = sensor; }
  void set_right_stick_y_sensor(sensor::Sensor *sensor) { this->right_stick_y_sensor_ = sensor; }
  // Called by the USB HID text sensor. Events are queued only while a PAX is
  // running, so launcher/menu keyboard input cannot leak into an app.
  void enqueue_keyboard_text(const std::string &text);
  // Called by USB HIDX sensor callbacks. Mouse reports are accumulated here
  // because the PAX worker and USB host run concurrently.
  void enqueue_mouse_delta(float dx, float dy);
  void enqueue_mouse_wheel(float wheel);
#ifdef PAX_LOADER_USE_USB_HIDX
  void set_usb_hidx(usb_hidx::USBHIDXComponent *usb_hidx) { this->usb_hidx_ = usb_hidx; }
#endif
#ifdef PAX_LOADER_USE_USB_MIDI
  void set_usb_midi(usb_midi::UsbMidi *usb_midi) { this->usb_midi_ = usb_midi; }
#endif
#ifdef PAX_LOADER_USE_SD_MMC
  sd_mmc_card::SdMmc *sd_mmc_{nullptr};
#endif
#ifdef PAX_LOADER_USE_USB_MSC
  void set_usb_msc(usb_msc::UsbMsc *usb_msc) { this->usb_msc_ = usb_msc; }
  usb_msc::UsbMsc *usb_msc_{nullptr};
#endif
  void set_button(uint8_t index, binary_sensor::BinarySensor *sensor) {
    if (index < BUTTON_COUNT)
      this->buttons_[index] = sensor;
  }

  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  static PaxLoader *active() { return active_; }

  // Static callbacks installed in the C ABI table used by the PAX.
  static uint16_t *svc_display_get_framebuffer();
  static uint16_t *svc_display_get_emu_buffer();
  static void svc_display_flush();
  static void svc_display_emu_flush();
  static void svc_display_clear(uint16_t color);
  static void svc_display_set_scale(float sx, float sy);
  static void svc_display_write_frame_rgb565(const uint16_t *buffer);
  static void svc_display_write_frame_custom(const uint16_t *buffer, uint16_t in_w, uint16_t in_h,
                                             float scale, bool byte_swap);
  static void svc_display_write_rect(int x, int y, int w, int h, const uint16_t *data);
  static int svc_jpeg_decode_rgb565(const uint8_t *data, uint32_t size, uint16_t *out,
                                    uint32_t capacity, int width, int height, int stride);
  static int svc_hardware_count();
  static int svc_hardware_info(int index,pax_hardware_info_t *out);
  static int svc_hardware_read(const char *name,float *value);
  static int svc_hardware_write(const char *name,float value);
  void update_hardware_();
  struct HardwareChannel {
    pax_hardware_info_t info{};
    std::function<float()> read;
    number::Number *output=nullptr;
    float value=0;
    bool valid=false;
  };
  struct HardwareCommand { int index; float value; };
  std::vector<HardwareChannel> hardware_;
  HardwareCommand hardware_queue_[16]{};
  unsigned hardware_head_=0,hardware_tail_=0;
  portMUX_TYPE hardware_lock_=portMUX_INITIALIZER_UNLOCKED;
  static void svc_display_get_size(int *width, int *height);
  static int svc_display_set_canvas(int width, int height);
  static int svc_midi_read(uint8_t *buf, int len);
  static int svc_midi_write(const uint8_t *data, int len);
  static int svc_touch_read_points(pax_touch_point_t *points, int max);
  void snapshot_touches_();
  static int svc_sprite_blit(uint16_t *framebuf, uint32_t fb_w, uint32_t fb_h,
                             uint32_t x, uint32_t y, const uint16_t *sprite,
                             uint32_t sp_w, uint32_t sp_h, uint16_t colorkey);
  static int svc_fb_copy(const uint16_t *src, uint16_t *dst, uint32_t w, uint32_t h);
  static uint16_t *svc_png_load_rgb565(const char *path,
                                       uint16_t *out_w, uint16_t *out_h);
  static uint16_t *svc_png_load_rgb565_blended(const char *path,
                                               uint16_t *out_w, uint16_t *out_h,
                                               uint32_t background_bgr888);
  static void svc_display_lock();
  static void svc_display_unlock();
  static void svc_audio_init(int sample_rate);
  static void svc_audio_submit(short *stereo_buf, int frame_count);
  static void svc_input_gamepad_read(pax_gamepad_state_t *state);
  static int svc_input_l3_read();
  static int svc_input_mouse_read(int *dx, int *dy, int *buttons);
  static int svc_input_mouse_scroll_read(int *wheel);
  static int svc_input_keyboard_read(pax_keyboard_event_t *event);
  static int svc_touch_read(int *x, int *y);
  static int svc_net_udp_open(uint16_t port, int broadcast);
  static int svc_net_udp_send(int handle, const void *buf, int len, uint32_t ip, uint16_t port);
  static int svc_net_udp_recv(int handle, void *buf, int len, uint32_t *ip, uint16_t *port);
  static void svc_net_udp_close(int handle);
  static int svc_net_ipv4(uint32_t *ip, uint32_t *netmask);
  static int svc_network_config_get(pax_network_config_t *out);
  static int svc_network_config_set(const pax_network_config_t *config);
  static int svc_storage_status_get(int32_t storage, pax_storage_status_t *out);
  static int svc_storage_set_mounted(int32_t storage, int32_t mounted);
  static int svc_net_tcp_connect(uint32_t ip, uint16_t port);
  static int svc_net_tcp_listen(uint16_t port);
  static int svc_net_tcp_accept(int handle, uint32_t *ip, uint16_t *port);
  static int svc_net_tcp_send(int handle, const void *buf, int len);
  static int svc_net_tcp_recv(int handle, void *buf, int len);
  static int svc_net_poll(int handle);
  static int svc_net_resolve(const char *host, uint32_t *ip);
  static void close_app_sockets_();
  static int svc_net_tls_connect(const char *host, uint16_t port);
  static int svc_net_tls_status(int handle);
  static int svc_net_tls_send(int handle, const void *buf, int len);
  static int svc_net_tls_recv(int handle, void *buf, int len);
  static void svc_net_tls_close(int handle);
  static void close_app_tls_();
  // App hand-off (psram_app.h app_open / app_get_arg / app_set_resume_arg).
  static int svc_app_open(const char *target, const char *arg, int return_after);
  static int svc_app_get_arg(char *buf, int len);
  static int svc_app_set_resume_arg(const char *arg);
  static int svc_file_list_dir(const char *path, char *buf, int len);
  // File management (psram_app.h file_mkdir / file_remove / file_rename / file_stat).
  static int svc_file_mkdir(const char *path);
  static int svc_file_remove(const char *path);
  static int svc_file_rename(const char *from, const char *to);
  static int svc_file_stat(const char *path, pax_file_stat_t *out);
  static int svc_file_zip_extract_first(const char *archive, const char *extensions,
                                        const char *cache_dir, const char *cache_stem,
                                        char *out_path, size_t out_size);
  static int svc_file_zip_extract_first_progress(
      const char *archive, const char *extensions, const char *cache_dir,
      const char *cache_stem, char *out_path, size_t out_size,
      pax_file_progress_fn_t progress, void *context);
  static int svc_file_zip_extract_first_impl(
      const char *archive, const char *extensions, const char *cache_dir,
      const char *cache_stem, char *out_path, size_t out_size,
      pax_file_progress_fn_t progress, void *context);
  static int svc_ui_call_sync(pax_ui_call_fn_t callback, void *arg, uint32_t timeout_ms);
  static int svc_store_install_start(const char *url, const char *name, const char *title,
                                     const char *version, uint32_t size, const char *sha256);
  static int svc_store_install_status(pax_install_progress_t *out);
  static int svc_archive_download_start(const char *url, const char *path);
  static int svc_archive_auth_set(const char *logged_in_user, const char *logged_in_sig);
  static int svc_archive_auth_get(char *cookie_header, size_t capacity);
  static void svc_ui_sound(uint8_t sound, uint8_t param);
  static int svc_device_status_get(pax_device_status_t *out);
  static int svc_device_date_get(char *out, uint32_t capacity);
  static int32_t svc_settings_auto_brightness_get();
  static void svc_settings_auto_brightness_set(int32_t enabled);
  // The runtime (VFS) path for an app's path, or "" when files::clean_app_path
  // refuses it (outside /sd and the data roots, "..", ...). *root gets the
  // runtime path of the storage root it is on (/sdcard, /usb0).
  static std::string app_file_path_(const char *path, std::string *root = nullptr);
  static void *svc_file_open(const char *path, const char *mode);
  static int svc_file_close(void *stream);
  static size_t svc_file_read(void *ptr, size_t size, size_t nmemb, void *stream);
  static size_t svc_file_write(const void *ptr, size_t size, size_t nmemb, void *stream);
  static int svc_file_seek(void *stream, long offset, int whence);
  static long svc_file_tell(void *stream);
  static void *svc_mem_caps_alloc(size_t size, uint32_t caps);
  static void *svc_mem_alloc(size_t size);
  static void *svc_mem_calloc(size_t n, size_t size);
  static void *svc_mem_realloc(void *ptr, size_t size);
  static void svc_mem_free(void *ptr);
  static int svc_log_printf(const char *fmt, ...);
  static int svc_log_vprintf(const char *fmt, va_list args);
  static void svc_delay_ms(int ms);
  static int64_t svc_get_time_us();
  static char *svc_settings_rom_path_get();
  static void svc_settings_rom_path_set(const char *path);
  static int32_t svc_settings_volume_get();
  static void svc_settings_volume_set(int32_t level);
  static int32_t svc_settings_brightness_get();
  static void svc_settings_brightness_set(int32_t level);
  static int svc_task_create(void (*fn)(void *), const char *name, uint32_t stack_depth, void *arg,
                             int priority, void *out_handle, int core);
  static void svc_task_delete(void *handle);
  static void populate_services(app_services_t *services);
  static int svc_game_menu_register(const char *label, pax_game_menu_action_fn_t callback, void *context);
  static int svc_game_menu_set_page(const pax_game_menu_page_t *page);
  static void svc_game_menu_close();

 protected:
  bool launch_();
  bool start_loaded_app_(psram_app_handle_t app, const std::string &source);
  static void pax_task_entry_(void *arg);
  static void pax_load_task_entry_(void *arg);
  static void pax_catalog_task_entry_(void *arg);
  static void archive_task_entry_(void *arg);
  static void pax_info_task_entry_(void *arg);
  static void pax_install_task_entry_(void *arg);
  // Downloads the app's missing data files. Each one downloaded is recorded
  // in the install manifest of `app` (default: the .pax's name), so Uninstall
  // can offer to delete it.
  esp_err_t sync_app_data_(const std::string &pax_url, const std::string &app = std::string());
  std::string find_data_file_(const std::string &target) const;
  // data_root, then each data_search root not already listed.
  std::vector<std::string> data_roots_() const;
  esp_err_t download_data_file_(const data::DataFile &file, const std::string &path, uint32_t done_before,
                                uint32_t total, size_t index, size_t count);
  void finish_app_();
#ifdef PAX_LOADER_USE_LVGL
  void show_loading_message_();
  void hide_loading_message_();
#endif
  // ── App hand-off (pax_handoff.h) ──
  // The .pax for an app name: the installed copy, then each library source
  // (folder or HTTP catalog) in order; "" when none has it. Runs on the
  // calling app's task and may block while an HTTP catalog is fetched.
  std::string resolve_app_(const std::string &name);
  // The .pax files of a folder library source, under every data search root.
  std::vector<std::string> folder_paxs_(const std::string &folder) const;
  // Where a folder library source is looked for: (root label, folder) pairs,
  // the folder under every data search root, the configured one first.
  std::vector<std::pair<std::string, std::string>> catalog_folders_(const std::string &folder) const;
  // After an app ends or a launch fails: queue the next app of the hand-off
  // chain (the one asked for, or the one to go back to), if any.
  void continue_chain_();
  handoff::Chain chain_;        // guarded by handoff_mutex_
  SemaphoreHandle_t handoff_mutex_{nullptr};
  std::string running_source_;  // the running app's .pax, as it was launched
  std::string launch_arg_;      // what app_get_arg gives the running app
  std::string resume_arg_;      // what it gets back after an app it opened (handoff_mutex_)
  std::string next_arg_;        // the argument for the queued launch
  bool chain_launch_{false};    // the queued launch comes from chain_
  void update_catalog_ui_();
  void update_archive_ui_();
  void start_archive_search_();
  void update_archive_settings_ui_();
  void update_rom_selector_ui_();
  void set_rom_selector_selection_(uint16_t index);
  bool rom_selector_page_active_() const;
  void update_progress_ui_();
  void flush_framebuffer_();
  pax_launcher::DirtyRegion launcher_dirty_region_{};
  // Sends a panel-oriented (turned 180 degrees) frame to the display (plus
  // the remote-view sample and the close button). Most paths send the whole
  // canvas; the direct PPA path can send only its visible rectangle. The
  // caller holds display_mutex_.
  void send_display_buffer_(const uint16_t *display_buffer, int64_t start_us, int raw_x = -1, int raw_y = -1,
                            int width = -1, int height = -1, int source_stride = -1, bool panel_frame_buffer = false);
  // Canvas size. geometry_ is the panel and the running app's canvas; it only
  // changes under display_mutex_ (tasks that just hit-test touches read it
  // without the lock). Working buffers follow the selected canvas; maximum
  // dimensions remain the board limits across shrinking and growing.
  canvas::Geometry geometry_{};
  int max_canvas_w_{canvas::LEGACY_WIDTH};
  int max_canvas_h_{canvas::LEGACY_HEIGHT};
  size_t frame_alloc_bytes_{0};  // each of framebuffer_, rotated_framebuffer_, ppa_framebuffer_
  int panel_config_w_{0};  // YAML panel size; 0 = the display's
  int panel_config_h_{0};
  int default_canvas_w_{0};  // YAML default canvas; 0 = the panel
  int default_canvas_h_{0};
  // For the running app: the size display_get_size offers, and whether the
  // app has chosen a canvas (then display_get_size reports that one).
  int offered_canvas_w_{canvas::LEGACY_WIDTH};
  int offered_canvas_h_{canvas::LEGACY_HEIGHT};
  volatile bool canvas_chosen_{false};
  bool allocate_frame_buffers_(int width, int height);
  bool resize_frame_buffers_(int width, int height);
  void free_frame_buffers_();
  int set_canvas_(int width, int height);
  // Switches the canvas to width x height and blanks the frame buffers (and,
  // with blank_panel, the whole panel). The caller holds display_mutex_.
  bool apply_canvas_(int width, int height, bool blank_panel);
  // Back to 800x480 for the next app, and what display_get_size offers it.
  bool prepare_canvas_for_app_(const std::string &source);
  bool restore_legacy_canvas_();
  void blank_panel_();
  // The canvas size the app's listing recommends ("" for none): from the
  // store's listings, else the .json next to a .pax on the card.
  std::string listing_recommended_canvas_(const std::string &source) const;
  std::string settings_path_() const;
  std::string archive_settings_path_() const;
  std::string background_settings_path_() const;
  void ensure_background_();
  void update_background_label_();
  void apply_background_(const std::string &path);
  void load_saved_background_();
  void save_background_selection_();
  void load_archive_collection_();
  bool save_archive_collection_(const std::string &base_url, const std::string &name);
  void render_custom_(const uint16_t *buffer, uint16_t in_w, uint16_t in_h, float scale, bool byte_swap);
  void log_render_time_(int64_t render_start_us, uint16_t in_w, uint16_t in_h, float scale);
  // Where render_custom_ last put a frame in rotated_framebuffer_ (x, y, w, h):
  // the black border around it only needs clearing when that changes.
  uint16_t direct_frame_[4]{0, 0, 0, 0};
  // True when the newest frame is only in rotated_framebuffer_ (direct path),
  // not in framebuffer_; screenshots then read it from there.
  volatile bool last_frame_direct_{false};
  void render_emu_();
  void clear_(uint16_t color);
  void update_control_overlay_();
  void wake_control_overlay_();
  // Draw a control-state change immediately.  Slider drags use a throttled,
  // slider-only repaint; button/swipe state changes repaint the complete
  // toolbar so the controls appear without waiting for the next app frame.
  void redraw_control_overlay_now_(bool full = true);
  void draw_close_overlay_();
  void clear_close_overlay_(int slide = 0);
  void draw_game_menu_button_();
  void clear_game_menu_button_(int slide = 0);
  void open_game_menu_();
  void close_game_menu_();
  void poll_game_menu_();
  void draw_game_menu_();
  void build_game_menu_frame_();
  void activate_game_menu_item_();
  bool game_menu_touch_(int x, int y);
  void wait_for_game_menu_();
  void queue_game_menu_action_(uint32_t action, int32_t direction = 0);
  void back_game_menu_();
  uint16_t game_menu_buttons_() const;
  bool game_menu_capture_held_() const;
  void draw_reload_overlay_();
  void clear_reload_overlay_(int slide = 0);
  void draw_volume_overlay_();
  void draw_volume_slider_overlay_();
  void draw_brightness_overlay_();
  void draw_brightness_slider_overlay_();
  void clear_volume_slider_overlay_();
  void clear_brightness_slider_overlay_();
  void clear_volume_overlay_(int slide = 0);
  void clear_brightness_overlay_(int slide = 0);
  void restore_lvgl_();
  void begin_report_(const std::string &source);
  void append_report_log_(const char *line);
  void send_report_(const char *outcome, int result, const std::string &source);
  static void report_task_entry_(void *arg);
  bool right_stick_mouse_enabled_() const;
  void read_input_(pax_gamepad_state_t *state, bool include_hid = true);
  int read_mouse_(int *dx, int *dy, int *buttons);
  int read_mouse_scroll_(int *wheel);
  int read_keyboard_(pax_keyboard_event_t *event);
  void clear_keyboard_queue_();
  void clear_mouse_delta_();
  // Dispatch a short callback from the PAX worker onto this ESPHome loop
  // task.  LVGL and its display callbacks are single-threaded; executing
  // them here prevents the worker from racing the host LVGL component.
  void process_ui_call_();
  void enqueue_keyboard_event_(int key, bool down);
  void enqueue_keyboard_tap_(int key);
  void poll_close_button_();
  bool close_touch_(int x, int y);
  bool reload_touch_(int x, int y);
  bool volume_touch_(int x, int y);
  void poll_exit_combo_();
  int read_touch_(int *x, int *y);
  void read_remote_ui_input_(pax_gamepad_state_t *state);
  void audio_init_(int sample_rate);
  void audio_submit_(short *stereo_buf, int frame_count);
  static void screen_stream_task_entry_(void *arg);
  void screen_stream_task_();
  bool send_screenshot_(int client_fd);
  bool send_file_(int client_fd);

  static PaxLoader *active_;

  std::string path_;
  display::Display *display_{nullptr};
  mipi_dsi::MIPI_DSI *mipi_dsi_{nullptr};
  mipi_dsi::MIPI_DSI *native_mipi_dsi_{nullptr};
  const uint8_t *native_rgb888_frame_{nullptr};
  touchscreen::Touchscreen *touchscreen_{nullptr};
  speaker::Speaker *speaker_{nullptr};
#ifdef PAX_LOADER_USE_LVGL
  lvgl::LvglComponent *lvgl_{nullptr};
#endif
#ifdef PAX_LOADER_USE_USB_HIDX
  usb_hidx::USBHIDXComponent *usb_hidx_{nullptr};
#endif
#ifdef PAX_LOADER_USE_USB_MIDI
  usb_midi::UsbMidi *usb_midi_{nullptr};
#endif
  binary_sensor::BinarySensor *buttons_[BUTTON_COUNT]{};
  binary_sensor::BinarySensor *toggle_button_{nullptr};
  binary_sensor::BinarySensor *launch_button_{nullptr};
  binary_sensor::BinarySensor *fire_button_{nullptr};
  binary_sensor::BinarySensor *touch_button_{nullptr};
  binary_sensor::BinarySensor *game_menu_button_{nullptr};
  binary_sensor::BinarySensor game_menu_capture_fallback_{};
  volatile bool game_menu_capture_down_{false};
  volatile bool game_menu_toggle_pending_{false};
  volatile bool game_menu_open_app_options_pending_{false};
  int64_t game_menu_open_app_options_until_us_{0};
  portMUX_TYPE game_menu_button_lock_ = portMUX_INITIALIZER_UNLOCKED;
  // A capacitive pad can read ON for good (toggle mode latched, calibrated
  // while touched): after 10 s ON without a change it is ignored as A until it
  // turns OFF, so it cannot hold A down in apps or keep the launcher unarmed.
  int64_t touch_on_since_us_{0};
  bool touch_stuck_logged_{false};
  bool touch_button_stuck_() {
    const int64_t now = esp_timer_get_time();
    if (this->touch_on_since_us_ == 0)
      this->touch_on_since_us_ = now;
    const bool stuck = now - this->touch_on_since_us_ > 10000000;
    if (stuck && !this->touch_stuck_logged_) {
      ESP_LOGW("pax_loader", "Touch button ON for over 10 s: ignored as A until it is released");
      this->touch_stuck_logged_ = true;
    }
    return stuck;
  }
  void touch_button_released_() {
    this->touch_on_since_us_ = 0;
    this->touch_stuck_logged_ = false;
  }
  sensor::Sensor *adc_button_sensor_{nullptr};
  sensor::Sensor *left_stick_x_sensor_{nullptr};
  sensor::Sensor *left_stick_y_sensor_{nullptr};
  sensor::Sensor *right_stick_x_sensor_{nullptr};
  sensor::Sensor *right_stick_y_sensor_{nullptr};

  uint16_t *framebuffer_{nullptr};
  // PAX renders in its logical landscape orientation.  The Elecrow panel is
  // mounted 180 degrees around, so direct PAX flushes use this second PSRAM
  // buffer to avoid mutating the app-owned framebuffer in place.
  uint16_t *rotated_framebuffer_{nullptr};
  // Hardware SRM output used to rotate the PAX canvas without a CPU pixel
  // copy.  The software buffer remains as a fallback if PPA is unavailable.
  void *ppa_srm_client_{nullptr};
  uint16_t *ppa_framebuffer_{nullptr};
  // The MIPI-DSI driver's two scanout buffers are reused as PPA destinations.
  // Clear each once when first encountered; clearing a 1024x600 PSRAM buffer
  // on every 640x400 PAX frame needlessly costs tens of milliseconds.
  uint16_t *initialized_panel_framebuffers_[2]{nullptr, nullptr};
  uint16_t *emu_buffer_{nullptr};
  // Optional diagnostic framebuffer published to the host over TCP. The
  // stream is inactive unless a recorder connects to port 3233.
  uint16_t *stream_framebuffer_{nullptr};
  // Network writes can block. These snapshots keep stream_mutex_ confined to
  // the short producer/consumer copy instead of the TCP transfer.
  uint16_t *stream_frame_packet_{nullptr};
  uint8_t *stream_audio_buffer_{nullptr};
  uint8_t *stream_audio_packet_{nullptr};
  size_t stream_audio_read_{0};
  size_t stream_audio_write_{0};
  size_t stream_audio_available_{0};
  SemaphoreHandle_t stream_mutex_{nullptr};
  volatile bool stream_frame_ready_{false};
  volatile bool stream_client_connected_{false};
  volatile bool stream_enabled_{false};
  std::atomic_flag screenshot_capture_lock_ = ATOMIC_FLAG_INIT;
  volatile bool screenshot_requested_{false};
  // With no app running, a screenshot is the LVGL menu: the main loop (the
  // LVGL thread) snapshots the active screen into this RGB565 buffer and the
  // stream task sends it. Needs LV_USE_SNAPSHOT (e.g. -DLV_USE_SNAPSHOT=1).
  uint16_t *menu_shot_{nullptr};
  uint16_t menu_shot_w_{0};
  uint16_t menu_shot_h_{0};
  volatile bool menu_shot_wanted_{false};  // set by the stream task, taken by loop()
  volatile bool menu_shot_ready_{false};
  void take_menu_shot_();
  volatile bool file_requested_{false};
  char file_request_path_[128]{};
  uint32_t stream_frame_sequence_{0};
  TaskHandle_t stream_task_handle_{nullptr};
  float scale_x_{1.0f};
  float scale_y_{1.0f};
  bool autostart_{false};
  bool launch_pending_{false};
  bool launched_{false};
  // Guard: prevent play_ui_sound() from starting the speaker during setup()
  // before FreeRTOS tasks and the I2S DMA chain are fully initialized.
  // Set to true at the end of the first loop() iteration.
  bool ui_sounds_ready_{false};
  bool toggle_button_state_{false};
  bool toggle_wait_release_{false};
  bool toggle_close_requested_{false};
  bool launch_button_state_{false};
  bool launch_wait_release_{false};
  bool touch_active_{false};
  portMUX_TYPE remote_ui_input_lock_ = portMUX_INITIALIZER_UNLOCKED;
  volatile int remote_ui_touch_x_{0};
  volatile int remote_ui_touch_y_{0};
  volatile int remote_ui_touch_end_x_{0};
  volatile int remote_ui_touch_end_y_{0};
  volatile int64_t remote_ui_touch_start_us_{0};
  volatile int64_t remote_ui_touch_until_us_{0};
  volatile bool remote_ui_touch_release_pending_{false};
  volatile int remote_ui_button_{-1};
  volatile int64_t remote_ui_button_until_us_{0};
  // The fingers on the panel, copied in loop() (the touchscreen's own list is
  // not safe to read from the app task); guarded by touch_points_lock_.
  static constexpr int MAX_TOUCH_POINTS = 5;
  touchscreen::TouchPoint touch_points_[MAX_TOUCH_POINTS]{};
  int touch_point_count_{0};
  portMUX_TYPE touch_points_lock_ = portMUX_INITIALIZER_UNLOCKED;
  // Set by the on-screen close control.  It is deliberately independent of
  // the normal PAX X/action input so every loader-backed app gets the same
  // exit request, including apps that do not draw their own controls.
  volatile bool global_close_requested_{false};
  bool reload_after_close_{false};
  std::string reload_source_;
  // When a touch on a close control drawn over the canvas started (us, 0 =
  // none): that one closes only when held (close_touch_).
  int64_t close_hold_since_us_{0};
  // Prevent one held touch from stepping the master volume every loop.
  bool volume_touch_active_{false};
  bool volume_slider_visible_{false};
  bool brightness_touch_active_{false};
  bool brightness_slider_visible_{false};
  // The three in-app controls share a centered toolbar. It hides above the
  // panel after a short idle period; retain the last rendered position so an
  // old margin control can be cleared after the next game frame is drawn.
  int32_t control_overlay_offset_{0};
  int32_t control_overlay_rendered_offset_{0};
  int64_t control_overlay_last_touch_us_{0};
  int64_t control_overlay_last_redraw_us_{0};
  bool control_overlay_rendered_{false};
  // Ordinary one-finger game touches must not wake the hidden toolbar. The
  // swipe state suppresses the deliberate top-strip wake gesture from
  // reaching the running PAX as a normal touch.
  bool control_overlay_wake_gesture_active_{false};
  bool control_overlay_swipe_tracking_{false};
  int control_overlay_swipe_start_x_{0};
  int control_overlay_swipe_start_y_{0};
  int64_t control_overlay_swipe_start_us_{0};
  // A control-state change needs one complete game-frame repaint before the
  // new overlay is composited.  Without this, a partial MIPI transfer can
  // briefly expose the old black erase rectangle.
  bool control_overlay_repaint_pending_{false};
  volatile bool game_menu_open_{false};
  bool game_menu_touch_active_{false};
  volatile bool game_menu_quarantine_{false};
  bool game_menu_frame_dirty_{true};
  int64_t game_menu_quarantine_until_us_{0};
  uint8_t game_menu_selection_{0};
  uint8_t game_menu_item_count_{5};
  uint16_t game_menu_previous_buttons_{0};
  uint16_t *game_menu_frame_{nullptr};
  int game_menu_width_{0};
  int game_menu_height_{0};
  int game_menu_screen_x_{0};
  int game_menu_screen_y_{0};
  int game_menu_row_top_{0};
  int game_menu_row_height_{0};
  int game_menu_font_scale_{1};
  pax_game_menu_action_fn_t game_menu_callback_{nullptr};
  void *game_menu_context_{nullptr};
  TaskHandle_t game_menu_task_{nullptr};
  char game_menu_provider_label_[32]{};
  pax_game_menu_page_t game_menu_page_{};
  volatile bool game_menu_app_page_{false};
  volatile bool game_menu_busy_{false};
  bool game_menu_action_pending_{false};
  uint32_t game_menu_action_{0};
  int32_t game_menu_direction_{0};
  // Select+Start is the hardware-independent PAX exit shortcut. It is
  // deliberately a hold so a normal Select/Start press cannot quit an app.
  int64_t exit_combo_since_us_{0};
  bool exit_combo_triggered_{false};
  // When it was requested (esp_timer us): the buttons a close shows the app
  // follow a short sequence from then on (close_buttons_()).
  volatile int64_t close_requested_us_{0};
  void begin_close_();
  // The close buttons down right now: bit 0 Menu, bit 1 X, bit 2 L3.
  uint8_t close_buttons_() const;
  volatile uint32_t pax_frame_count_{0};
  int64_t pax_started_us_{0};
  int audio_sample_rate_{0};
  int32_t master_volume_{100};
  sensor::Sensor *ambient_light_sensor_{nullptr};
  bool auto_brightness_enabled_{false};
  int64_t next_auto_brightness_us_{0};
  // Heap diagnostics (log_heap in pax_loader.cpp): the running app's first
  // short audio write was logged; when loop() logs the heap next (esp_timer us).
  volatile bool audio_trouble_logged_{false};
  int64_t next_heap_log_us_{0};
  SemaphoreHandle_t display_mutex_{nullptr};
  SemaphoreHandle_t keyboard_mutex_{nullptr};
  SemaphoreHandle_t ui_call_done_{nullptr};
  volatile uint8_t ui_call_state_{0};  // idle, pending, executing, complete
  volatile int ui_call_result_{-1};
  volatile bool ui_call_cancelled_{false};
  pax_ui_call_fn_t ui_call_callback_{nullptr};
  void *ui_call_arg_{nullptr};
  portMUX_TYPE ui_call_lock_ = portMUX_INITIALIZER_UNLOCKED;
  portMUX_TYPE mouse_input_lock_ = portMUX_INITIALIZER_UNLOCKED;
  int32_t mouse_dx_accum_{0};
  int32_t mouse_dy_accum_{0};
  int32_t mouse_wheel_accum_{0};
  // Text reports are converted to press/release pairs. Keep enough room for
  // a short console command while Quake drains the queue on its worker task.
  // Until when (esp_timer us) an arrow that arrived only as text holds its
  // D-pad direction: up, right, down, left (the PAX_INPUT_UP.. order).
  static constexpr int64_t ARROW_TAP_US = 150000;
  volatile int64_t arrow_tap_until_us_[4]{0, 0, 0, 0};
  static constexpr size_t KEYBOARD_QUEUE_SIZE = 128;
  pax_keyboard_event_t keyboard_queue_[KEYBOARD_QUEUE_SIZE]{};
  size_t keyboard_head_{0};
  size_t keyboard_tail_{0};
  TaskHandle_t pax_task_handle_{nullptr};
  bool pax_task_uses_caps_{false};
  TaskHandle_t pax_load_task_handle_{nullptr};
  psram_app_handle_t app_handle_{nullptr};
  psram_app_handle_t pax_load_handle_{nullptr};
  std::string pax_load_source_;
  // Exact source of the active PAX; used to restrict store-install ABI
  // actions to the standalone launcher rather than running game binaries.
  std::string running_app_source_;
  volatile bool pax_loading_{false};
  // Warned once that LVGL woke on input while an app ran (loop()).
  bool lvgl_resume_warned_{false};
  volatile bool pax_load_done_{false};
  volatile int pax_load_result_{-1};
  volatile bool pax_task_done_{false};
  volatile int pax_task_result_{-1};
  volatile bool pax_load_data_failed_{false};
  // App data (data_root) and launch progress. The loader task writes the
  // progress under progress_lock_; the loop task reads it for the UI.
  std::string data_root_{"/sd"};
  bool download_data_{true};
  std::vector<std::string> data_search_;
  static constexpr size_t PROGRESS_STATUS_SIZE = 96;
  portMUX_TYPE progress_lock_ = portMUX_INITIALIZER_UNLOCKED;
  char progress_status_[PROGRESS_STATUS_SIZE]{};
  uint32_t progress_done_{0};
  uint32_t progress_total_{0};
  bool progress_active_{false};
  volatile uint32_t progress_seq_{0};
  uint32_t progress_ui_seq_{0};
  // Test reports (report_url). report_log_ holds the newest app log lines, up to
  // report_log_bytes_; the PAX worker appends and the loop task sends, so both
  // go through report_mutex_.
  std::string report_url_;
  size_t report_log_bytes_{4096};
  std::string report_log_;
  std::string report_source_;
  int64_t report_started_us_{0};
  SemaphoreHandle_t report_mutex_{nullptr};
  std::string catalog_url_;
  std::string catalog_html_;
  std::vector<std::pair<std::string, std::string>> catalog_entries_;
  struct CatalogSource {
    std::string name;
    std::string url;
    std::vector<std::pair<std::string, Trigger<> *>> actions;
  };
  bool side_menu_open_{false};
  std::vector<CatalogSource> catalogs_;
  size_t catalog_index_{0};
  // The URL the running fetch is for: a switch mid-fetch refetches afterwards.
  std::string catalog_fetch_url_;

  struct ArchiveSource {
    std::string name;
    std::string url;
    std::string destination;
    std::string extensions;
  };
  struct ArchiveEntry {
    std::string label;
    std::string url;
    std::string path;
    uint32_t size{0};
  };
  struct ArchiveSearchResult {
    std::string identifier;
    std::string title;
    std::string description;
  };
  std::vector<ArchiveSource> archive_sources_;
  size_t archive_index_{0};
  std::string archive_html_;
  std::vector<ArchiveEntry> archive_entries_;
  std::string archive_fetch_url_;
  std::vector<ArchiveSearchResult> archive_search_results_;
  bool archive_search_pending_{false};
  int64_t archive_search_due_us_{0};
  volatile bool archive_loading_{false};
  volatile bool archive_done_{false};
  volatile int archive_result_{-1};
  TaskHandle_t archive_task_handle_{nullptr};

  enum ArchiveViewMode : uint8_t {
    ARCHIVE_VIEW_SEARCH = 0,
    ARCHIVE_VIEW_SYSTEMS = 1,
    ARCHIVE_VIEW_GAMES = 2,
  };
  ArchiveViewMode archive_mode_{ARCHIVE_VIEW_SEARCH};
  bool archive_return_to_rom_selector_{false};
  std::string archive_system_name_{"NES"};
  std::string archive_system_dest_{"/sd/roms/nes"};
  std::string archive_system_exts_{".nes|.zip|"};
  std::string archive_search_query_;
  std::string archive_search_term_;
  std::string archive_search_collection_{"open_source_software"};
  std::string archive_collection_base_;
  std::string archive_collection_name_;
  std::string archive_settings_location_;

  // ── Store view (pax_store.cpp) ──
 public:
  // One app's listing, from its <app>.json (all fields optional).
  struct AppInfo {
    bool has_info{false};
    std::string name, title, version, author, category, license, about, changelog, upstream, source;
    std::vector<std::string> controls;
    // The listing's "canvas": true (any size) or the sizes the app can draw
    // ("1024x600", ...). Either one adds the Screen setting to its detail page.
    bool canvas_any{false};
    std::vector<std::string> canvas_sizes;
    bool supports_canvas() const { return this->canvas_any || !this->canvas_sizes.empty(); }
    // The size it recommends ("canvas_recommended", or "recommended" in an
    // object "canvas"): the Screen setting's default instead of the device's.
    std::string canvas_recommended;
    // What it needs on the card ("requires"), plus the data files the store
    // downloads for it (download = true). The detail page checks them.
    struct RequiredFile {
      std::string path;  // as the app sees it: /sd/roms/doom/doom1.wad, a folder/ or a pattern
      std::string note;
      bool optional{false};
      bool download{false};
    };
    std::vector<RequiredFile> required_files;
    uint32_t size{0}, data_size{0};
    std::string sha256;
    std::string sidecar;            // optional raw JSON; catalog entries refetch it when installing
    std::vector<uint8_t> icon_png;  // decoded from the listing's base64 icon
    std::shared_ptr<uint16_t> tile_icon;  // 64x64 RGB565 in PSRAM, decoded by the info task
  };

 protected:
  bool store_ui_{false};
  bool store_thumbnails_enabled_{true};
  bool store_info_enabled_{true};
  std::string install_dir_{"/sd/roms/pax"};
  std::string catalog_search_query_;
  // Info for catalog_entries_ (same order), fetched by a task after each listing.
  std::vector<AppInfo> app_info_;
  std::vector<AppInfo> info_result_;
  std::vector<std::string> info_urls_;
  uint32_t catalog_generation_{0};
  uint32_t info_generation_{0};
  TaskHandle_t info_task_handle_{nullptr};
  volatile bool info_loading_{false};
  volatile bool info_done_{false};
  // Store network work waits while a PAX loads or runs: TLS takes internal
  // DMA-capable RAM that the SD card and I2S drivers need at app start.
  volatile bool info_interrupted_{false};  // the info task stopped early for an app
  bool info_deferred_{false};              // listings to fetch once the app ends
  bool catalog_deferred_{false};           // a catalog refresh asked for during an app
  void start_info_fetch_();
  void poll_info_fetch_();
  // name -> installed version, from <install_dir>/*.json.
  std::vector<std::pair<std::string, std::string>> installed_;
  // The same, as found by the info and install tasks; the loop takes it over.
  std::vector<std::pair<std::string, std::string>> info_installed_;
  std::vector<std::pair<std::string, std::string>> install_installed_;
  static std::vector<std::pair<std::string, std::string>> scan_installed_(const std::string &install_dir);
  std::string installed_version_(const std::string &name) const;
  // Install/Update of one app: the .pax (checked against its size and
  // sha256), its data, then its listing, in a task.
  int install_index_{-1};
  AppInfo install_info_;  // copies for the task: app_info_ can be replaced meanwhile
  std::string install_url_;
  // Archive.org logged-in cookies are bearer credentials. Keep them only in
  // volatile loader memory, never NVS or the removable card; PAX auth survives
  // launcher hand-offs until reboot or explicit sign-out.
  char archive_auth_user_[512]{};
  char archive_auth_sig_[512]{};
  portMUX_TYPE archive_auth_lock_ = portMUX_INITIALIZER_UNLOCKED;
  bool direct_install_{false};  // API/tool upload, not a catalog Install/Update
  bool direct_file_upload_{false};  // API/tool upload of an arbitrary SD file
  std::string upload_path_;         // virtual path, validated under /sd
  TaskHandle_t install_task_handle_{nullptr};
  volatile bool install_done_{false};
  volatile esp_err_t install_result_{ESP_OK};
  void start_install_(int index);
  int start_install_from_pax_(const char *url, const char *name, const char *title,
                               const char *version, uint32_t size, const char *sha256);
  void poll_install_();
  // ── Install manifest and Uninstall (pax_store.cpp) ──
  // <install_dir>/<app>.installed.json lists every data file the store
  // downloaded for the app (path and size); Uninstall deletes only those.
  std::string manifest_path_(const std::string &app) const;
  void record_download_(const std::string &app, const std::string &path, uint32_t size, const std::string &sha256);
  // The manifest's files still on the card at their recorded size: what
  // Uninstall would delete (path, size).
  std::vector<std::pair<std::string, uint32_t>> downloaded_files_(const std::string &app) const;
  // Where a listing's required path is on the card (a data root's path to
  // it), or "" when it is on none of them.
  std::string find_required_(const std::string &path) const;

 public:
  // Removes an installed app: <install_dir>/<app>.pax and its listing, and
  // with delete_data the data files the store downloaded for it (listed in
  // its install manifest; files changed since, and everything else, stay).
  // Refused (false) while an app is loading or running or an install runs.
  bool uninstall_app(const std::string &app, bool delete_data);

 protected:
  void list_catalog_folder_();
  TaskHandle_t pax_catalog_task_handle_{nullptr};
  volatile bool catalog_loading_{false};
  volatile bool catalog_done_{false};
  volatile int catalog_result_{-1};
#ifdef PAX_LOADER_USE_LVGL
  lv_obj_t *catalog_container_{nullptr};
  lv_obj_t *archive_container_{nullptr};
  lv_obj_t *archive_settings_label_{nullptr};
  lv_obj_t *catalog_header_{nullptr};
  lv_obj_t *catalog_search_widget_{nullptr};
  lv_obj_t *catalog_clear_search_widget_{nullptr};
  lv_obj_t *catalog_refresh_button_{nullptr};
  lv_obj_t *catalog_back_button_{nullptr};
  lv_obj_t *catalog_status_label_{nullptr};
  lv_obj_t *catalog_status_time_label_{nullptr};
  lv_obj_t *catalog_status_network_label_{nullptr};
  lv_obj_t *catalog_status_battery_label_{nullptr};
  lv_obj_t *background_image_{nullptr};
  lv_obj_t *background_label_{nullptr};
  lv_obj_t *background_next_button_{nullptr};
  std::shared_ptr<uint16_t> background_pixels_;
  lv_image_dsc_t background_dsc_{};
  std::vector<std::string> background_paths_;
  std::string background_selection_;
  size_t background_index_{0};
  bool background_initialized_{false};
  lv_obj_t *catalog_volume_button_{nullptr};
  lv_obj_t *catalog_volume_slider_{nullptr};
  lv_obj_t *catalog_brightness_slider_{nullptr};
  // The toolbar control that opened a compact slider. B returns focus here.
  lv_obj_t *catalog_slider_return_focus_{nullptr};
  number::Number *catalog_brightness_number_{nullptr};
  const lv_font_t *catalog_volume_font_{nullptr};
  // Transparent LVGL hit-test shield. Direct-rendered PAXs do not create a
  // LVGL root object, so this prevents launcher widgets underneath them from
  // seeing a touch release while the PAX is active.
  lv_obj_t *touch_modal_shield_{nullptr};
  lv_obj_t *loading_message_{nullptr};
  bool catalog_ui_pending_{false};
  bool archive_ui_pending_{false};
  lv_obj_t *progress_fill_{nullptr};
  lv_obj_t *progress_label_{nullptr};
  // The loader's own progress panel on the top layer, used when the page on
  // screen has no progress widgets (e.g. a store source picked from another page).
  lv_obj_t *overlay_panel_{nullptr};
  lv_obj_t *overlay_fill_{nullptr};
  lv_obj_t *overlay_label_{nullptr};
  lv_obj_t *progress_ui_screen_{nullptr};
  bool focus_tooltips_enabled_{true};
  lv_obj_t *focus_tooltip_{nullptr};
  lv_obj_t *focus_tooltip_label_{nullptr};
  void ensure_progress_overlay_();
  void update_focus_tooltip_();
  void log_focus_(const char *reason);
  void restore_catalog_header_focus_();
  uint16_t catalog_selection_{0};
  // 1 when the list starts with the catalog switcher row, else 0.
  uint8_t catalog_header_rows_{0};
  bool catalog_header_focus_pending_{false};
  uint8_t launcher_direction_state_{0};
  // A held d-pad trigger repeats only on the ROM picker. The initial delay
  // keeps a tap distinct, then the short interval makes long lists quick to
  // scan without changing navigation speed on the rest of the launcher.
  uint8_t launcher_repeat_direction_{0};
  uint32_t launcher_repeat_next_ms_{0};
  bool launcher_a_state_{false};
  bool launcher_touch_state_{false};
  bool launcher_input_armed_{false};
  bool launcher_b_state_{false};
  bool launcher_l_state_{false};
  bool launcher_r_state_{false};
  // Store view widgets.
  struct AppIcon {
    std::shared_ptr<uint16_t> pixels;
    lv_image_dsc_t dsc{};
  };
  std::vector<AppIcon> tile_icons_;  // per app, 112x112
  std::vector<lv_obj_t *> catalog_tiles_;
  // Source indices represented by the visible tile order. In the normal grid
  // this is 0..catalog_entries_.size()-1; on Favorites it is a filtered view.
  std::vector<size_t> visible_catalog_indices_;
  bool favorites_page_active_{false};
  lv_obj_t *rom_selector_container_{nullptr};
  lv_obj_t *rom_selector_return_container_{nullptr};
  lv_obj_t *rom_selector_favorites_button_{nullptr};
  lv_obj_t *rom_selector_favorites_filter_button_{nullptr};
  lv_obj_t *rom_selector_position_label_{nullptr};
  std::vector<lv_obj_t *> rom_selector_buttons_;
  std::vector<std::string> rom_selector_paths_;
  std::string rom_selector_app_{"nes"};
  std::string rom_selector_folder_{"roms/nes"};
  std::string rom_selector_extensions_{".nes|.zip|"};
  // Exact local path or URL of the PAX that owns the selected ROM.  This is
  // needed when the picker was opened from a streamed store entry.
  std::string rom_selector_source_;
  std::string rom_selector_sidecar_{"/sd/roms/pax/nes.rom"};
  bool rom_selector_favorites_only_{false};
  uint16_t rom_selector_selection_{0};
  std::string rom_search_query_;
  uint16_t grid_columns_{1};
  lv_obj_t *detail_panel_{nullptr};
  AppIcon detail_icon_{};
  std::vector<lv_obj_t *> detail_buttons_;
  std::vector<uint8_t> detail_actions_;
  uint8_t detail_focus_{0};
  int detail_index_{-1};
  std::string detail_url_;  // reopened after the grid is rebuilt, if still listed
  // Side menu: a panel on the library page, parked off the right edge with its tab showing.
  lv_obj_t *drawer_{nullptr};
  lv_obj_t *drawer_volume_slider_{nullptr};
  lv_obj_t *drawer_brightness_slider_{nullptr};
  lv_obj_t *drawer_auto_brightness_button_{nullptr};
  lv_obj_t *drawer_auto_brightness_label_{nullptr};
  std::vector<lv_obj_t *> drawer_buttons_;
  std::vector<int> drawer_actions_;  // control action or an index into the source's actions
  uint8_t drawer_focus_{0};
  bool launcher_select_state_{false};
  // USB gamepad/keyboard navigation is owned by LVGL when enabled. The
  // loader still reads the physical ADC/touch/stick controls for fallback.
  bool lvgl_keypad_navigation_{false};
  void build_drawer_();
  void focus_drawer_button_(uint8_t index);
  void run_drawer_action_(int action);
  static void store_drawer_event_cb_(lv_event_t *event);
  static void store_tile_event_cb_(lv_event_t *event);
  static void store_button_event_cb_(lv_event_t *event);
  static void release_icon_(AppIcon *icon);
  static void set_icon_(AppIcon *icon, std::shared_ptr<uint16_t> pixels, uint16_t side);
  void free_icons_();
  void build_store_grid_();
  size_t store_source_index_(int visible_index) const;
  bool is_app_favorite_(const std::string &app) const;
  bool set_app_favorite_(const std::string &app, bool favorite);
  void toggle_app_favorite_(int index);
  bool is_rom_favorite_(const std::string &path) const;
  bool set_rom_favorite_(const std::string &path, bool favorite);
  void open_detail_(int index);
  void close_detail_();
  void focus_detail_button_(uint8_t index);
  void run_detail_action_(uint8_t action);
  // The detail page's Screen button: the app's settings key and label, and
  // one press (the next choice, saved).
  std::string detail_app_key_(int index) const;
  std::string screen_label_(int index);
  // The Screen button's choices for this app ("" = no saved setting) and the
  // recommended size that "" stands for ("" when there is none).
  std::vector<std::string> screen_options_(int index, std::string *recommended) const;
  void cycle_screen_setting_(int index);
  // Uninstall's confirmation, a dialog over the detail page: a title, a text,
  // a check box for the downloaded data (when there is any) and two buttons.
  lv_obj_t *dialog_{nullptr};
  std::vector<lv_obj_t *> dialog_items_;  // touch and d-pad targets, top to bottom
  std::vector<uint8_t> dialog_actions_;
  uint8_t dialog_focus_{0};
  bool dialog_delete_data_{false};
  std::string dialog_app_;  // the app it asks about
  void open_uninstall_dialog_(int index);
  void close_dialog_();
  void focus_dialog_item_(uint8_t index);
  void run_dialog_action_(uint8_t action);
  static void store_dialog_event_cb_(lv_event_t *event);
  // Move focus between the visible launcher controls using screen geometry.
  // This is the fallback/controller path; it also covers static toolbar
  // buttons which are not part of the dynamic catalog tile list.
  bool focus_store_spatial_(uint8_t direction);
  bool activate_store_focus_();
  bool handle_store_controls_(uint8_t newly_pressed, bool a_pressed, bool b_pressed, bool l_pressed, bool r_pressed,
                              bool select_pressed);
#endif
};

template<typename... Ts> class LaunchUrlAction : public Action<Ts...> {
 public:
  explicit LaunchUrlAction(PaxLoader *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, url)

  // ESPHome's Action::play signature changed from by-value to const-reference
  // arguments between supported releases.  Keep this action compatible with
  // both forms; Action::play_complex still dispatches it normally.
  void play(const Ts &...x) { this->parent_->request_launch_url(this->url_.value(x...)); }

 protected:
  PaxLoader *parent_;
};

template<typename... Ts> class LaunchRomAction : public Action<Ts...> {
 public:
  explicit LaunchRomAction(PaxLoader *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, emulator)
  TEMPLATABLE_VALUE(std::string, rom)

  void play(const Ts &...x) {
    this->parent_->request_launch_rom(this->emulator_.value(x...), this->rom_.value(x...));
  }

 protected:
  PaxLoader *parent_;
};

}  // namespace pax_loader
}  // namespace esphome
