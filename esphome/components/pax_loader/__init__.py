from pathlib import Path
import json
import os
from esphome.core import CORE
from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, display, esp32, lvgl, number, sd_mmc_card, sensor, socket, speaker, touchscreen, usb_msc
from esphome.components.lvgl import helpers as lvgl_helpers
from esphome.components.mipi_dsi.display import MIPI_DSI

from esphome.const import CONF_AUTOMATION_ID, CONF_ID, CONF_NAME, CONF_PATH, CONF_THEN, CONF_TRIGGER_ID, CONF_URL

DEPENDENCIES = ["network"]
AUTO_LOAD = ["binary_sensor", "json", "number", "sensor", "speaker", "touchscreen"]


CONF_AUTOSTART = "autostart"
CONF_CATALOG_URL = "catalog_url"
CONF_CATALOGS = "catalogs"
CONF_ARCHIVE_SOURCES = "archive_sources"
CONF_ARCHIVE_SEARCH_COLLECTION = "archive_search_collection"
CONF_DESTINATION = "destination"
CONF_EXTENSIONS = "extensions"
CONF_DEFAULT_CATALOG = "default_catalog"
CONF_LIBRARY_STYLE = "library_style"
CONF_INSTALL_DIR = "install_dir"
CONF_ACTIONS = "actions"
CONF_LABEL = "label"
CONF_REPORT_URL = "report_url"
CONF_REPORT_LOG_BYTES = "report_log_bytes"
CONF_DATA_ROOT = "data_root"
CONF_DOWNLOAD_DATA = "download_data"
CONF_DATA_SEARCH = "data_search"
CONF_DISPLAY_ID = "display_id"
CONF_MIPI_DSI_ID = "mipi_dsi_id"
CONF_TOUCHSCREEN_ID = "touchscreen_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_BRIGHTNESS_NUMBER = "brightness_number"
CONF_AMBIENT_LIGHT_SENSOR = "ambient_light_sensor"
CONF_TOGGLE_BUTTON = "toggle_button"
CONF_LAUNCH_BUTTON = "launch_button"
CONF_USB_HIDX_ID = "usb_hidx_id"
CONF_USB_MIDI_ID = "usb_midi_id"
CONF_SD_MMC_ID = "sd_mmc_id"
CONF_USB_MSC_ID = "usb_msc_id"
CONF_LVGL_ID = "lvgl_id"
CONF_LVGL_KEYPAD_NAVIGATION = "lvgl_keypad_navigation"
CONF_FIRE_BUTTON = "fire_button"
CONF_TOUCH_BUTTON = "touch_button"
CONF_ADC_BUTTON_SENSOR = "adc_button_sensor"
CONF_LEFT_STICK_X_SENSOR = "left_stick_x_sensor"
CONF_LEFT_STICK_Y_SENSOR = "left_stick_y_sensor"
CONF_RIGHT_STICK_X_SENSOR = "right_stick_x_sensor"
CONF_RIGHT_STICK_Y_SENSOR = "right_stick_y_sensor"
CONF_CANVAS_WIDTH = "canvas_width"
CONF_CANVAS_HEIGHT = "canvas_height"
CONF_PANEL_WIDTH = "panel_width"
CONF_PANEL_HEIGHT = "panel_height"

# The canvas apps draw on (psram_app.h display_get_size / display_set_canvas):
# apps that never ask stay at 800x480; these limits match pax_canvas.h.
CANVAS_MIN_WIDTH = 320
CANVAS_MIN_HEIGHT = 240
LEGACY_CANVAS = (800, 480)
MAX_SIDE = 4096

pax_loader_ns = cg.esphome_ns.namespace("pax_loader")
PaxLoader = pax_loader_ns.class_("PaxLoader", cg.Component)
LaunchUrlAction = pax_loader_ns.class_("LaunchUrlAction", automation.Action)
LaunchRomAction = pax_loader_ns.class_("LaunchRomAction", automation.Action)
CatalogActionTrigger = pax_loader_ns.class_("CatalogActionTrigger", automation.Trigger.template())
USBHIDXComponent = cg.esphome_ns.namespace("usb_hidx").class_("USBHIDXComponent")
UsbMidi = cg.esphome_ns.namespace("usb_midi").class_("UsbMidi")
UsbMsc = cg.esphome_ns.namespace("usb_msc").class_("UsbMsc", cg.Component)
LvglComponent = cg.esphome_ns.namespace("lvgl").class_("LvglComponent")

BUTTON_FIELDS = {
    "up": 0,
    "right": 1,
    "down": 2,
    "left": 3,
    "select": 4,
    "start": 5,
    "a": 6,
    "b": 7,
    "x": 8,
    "y": 9,
    "l": 10,
    "r": 11,
    "menu": 12,
    "volume": 13,
}

BUTTON_SCHEMAS = {
    cv.Optional(f"button_{name}"): cv.use_id(binary_sensor.BinarySensor)
    for name in BUTTON_FIELDS
}

def validate_data_root(value):
    """An absolute folder such as /sd (the SD card) or /usb0, without a trailing slash."""
    value = cv.string_strict(value).rstrip("/")
    if not value.startswith("/") or "//" in value or any(part in (".", "..") for part in value.split("/")):
        raise cv.Invalid("data_root must be an absolute folder such as /sd or /usb0")
    return value


def validate_catalog_url(value):
    """An HTTP(S) catalog page, or an absolute folder of .pax files such as /sd/roms/pax/."""
    value = cv.string_strict(value)
    if value.startswith(("http://", "https://")):
        return cv.url(value)
    if value.startswith("/") and ".." not in value.split("/"):
        return value
    raise cv.Invalid("a catalog url is an http(s):// catalog page or a folder such as /sd/roms/pax/")


# A button in the store view's side menu for this source, e.g. Mount / Eject
# for a storage folder.
CATALOG_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CatalogActionTrigger),
        cv.GenerateID(CONF_AUTOMATION_ID): cv.declare_id(automation.Automation),
        cv.Required(CONF_LABEL): cv.All(cv.string_strict, cv.Length(min=1, max=32)),
        cv.Required(CONF_THEN): automation.validate_action_list,
    }
)

CATALOG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=24)),
        cv.Required(CONF_URL): validate_catalog_url,
        cv.Optional(CONF_ACTIONS, default=[]): cv.All(cv.ensure_list(CATALOG_ACTION_SCHEMA), cv.Length(max=6)),
    }
)

ARCHIVE_SOURCE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=24)),
        cv.Required(CONF_URL): cv.url,
        cv.Required(CONF_DESTINATION): validate_data_root,
        cv.Optional(CONF_EXTENSIONS, default=".nes|.zip"): cv.All(cv.string_strict, cv.Length(max=96)),
    }
)


def even_pixels(minimum):
    """A canvas side: an even number of pixels from `minimum` to 4096."""

    def validator(value):
        value = cv.int_range(min=minimum, max=MAX_SIDE)(value)
        if value % 2:
            raise cv.Invalid(f"{value} is odd; canvas sizes are even numbers of pixels")
        return value

    return validator


def validate_canvas(config):
    """canvas_width/height and panel_width/height come in pairs; the canvas fits the panel."""
    for width, height in ((CONF_CANVAS_WIDTH, CONF_CANVAS_HEIGHT), (CONF_PANEL_WIDTH, CONF_PANEL_HEIGHT)):
        if (width in config) != (height in config):
            raise cv.Invalid(f"set both {width} and {height}, or neither")
    if CONF_CANVAS_WIDTH in config and CONF_PANEL_WIDTH in config:
        if config[CONF_CANVAS_WIDTH] > config[CONF_PANEL_WIDTH] or config[CONF_CANVAS_HEIGHT] > config[CONF_PANEL_HEIGHT]:
            raise cv.Invalid("the canvas must fit on the panel (canvas_width/height at most panel_width/height)")
    return config


def validate_catalogs(config):
    catalogs = config.get(CONF_CATALOGS, [])
    names = [catalog[CONF_NAME].lower() for catalog in catalogs]
    if len(set(names)) != len(names):
        raise cv.Invalid("catalog names must be unique")
    default = config.get(CONF_DEFAULT_CATALOG)
    if default is not None and default.lower() not in names:
        raise cv.Invalid(f"default_catalog '{default}' is not one of the catalogs")
    return config


def validate_hardware(channels):
    names = set()
    for channel in channels:
        name = channel[CONF_NAME]
        if name in names or not name or len(name.encode("utf-8")) > 39:
            raise cv.Invalid("hardware channel names must be unique and 1..39 bytes")
        names.add(name)
    if len(channels) > 48:
        raise cv.Invalid("at most 48 hardware channels")
    return channels


HARDWARE_SCHEMA = cv.All(
    cv.Schema({
        cv.Required(CONF_NAME): cv.string_strict,
        cv.Optional("unit", default=""): cv.All(cv.string_strict, cv.Length(max=15)),
        cv.Optional("sensor_id"): cv.use_id(sensor.Sensor),
        cv.Optional("binary_sensor_id"): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional("number_id"): cv.use_id(number.Number),
    }),
    cv.has_exactly_one_key("sensor_id", "binary_sensor_id", "number_id"),
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PaxLoader),
        cv.Required(CONF_PATH): cv.string,
        cv.Optional(CONF_AUTOSTART, default=False): cv.boolean,
        cv.Optional(CONF_CATALOG_URL): cv.string,
        # Several library sources (catalog pages or .pax folders), switched from
        # the library list; see README "Library sources".
        cv.Optional(CONF_CATALOGS): cv.All(cv.ensure_list(CATALOG_SCHEMA), cv.Length(min=1, max=8)),
        # Archive.org ZIP-member sources are browsed and downloaded by the
        # ESP32 loader itself; no host-side helper is involved.
        cv.Optional(CONF_ARCHIVE_SOURCES): cv.All(
            cv.ensure_list(ARCHIVE_SOURCE_SCHEMA), cv.Length(max=8)
        ),
        # Archive.org's category/collection to search, e.g. the free software
        # collection at archive.org/details/open_source_software.
        cv.Optional(CONF_ARCHIVE_SEARCH_COLLECTION, default="open_source_software"):
            cv.All(cv.string_strict, cv.Length(min=1, max=96)),
        cv.Optional(CONF_DEFAULT_CATALOG): cv.string_strict,
        # "grid": the ESPHOMEBREW store view (icons, badges, a detail page with
        # Stream / Install) instead of a plain list; see README "Store view".
        cv.Optional(CONF_LIBRARY_STYLE, default="list"): cv.one_of("list", "grid", lower=True),
        # Where Install puts store apps (and their listings, which mark them installed).
        cv.Optional(CONF_INSTALL_DIR, default="/sd/roms/pax"): validate_data_root,
        # POST a JSON test report here after every app run (see docs/feedback.md).
        cv.Optional(CONF_REPORT_URL): cv.url,
        cv.Optional(CONF_REPORT_LOG_BYTES, default=4096): cv.int_range(min=256, max=32768),
        # Store apps can list data files (game WADs etc.) next to their .pax;
        # missing ones are downloaded here before launch (docs/esphome-store.md).
        cv.Optional(CONF_DATA_ROOT, default="/sd"): validate_data_root,
        cv.Optional(CONF_DOWNLOAD_DATA, default=True): cv.boolean,
        # Where the user may already keep app files (SD card, USB drive, ...):
        # found files are not downloaded, and app reads of /sd/... fall back here.
        cv.Optional(CONF_DATA_SEARCH, default=["/sd", "/usb0"]): cv.ensure_list(validate_data_root),
        cv.Required(CONF_DISPLAY_ID): cv.use_id(display.Display),
        # Optional MIPI DSI fast path: render directly into an idle panel
        # framebuffer instead of copying each full PAX frame into scanout.
        cv.Optional(CONF_MIPI_DSI_ID): cv.use_id(MIPI_DSI),
        # The panel's size; by default the display's own (1024x600 on the Elecrow).
        # The frame buffers are sized for a canvas this large.
        cv.Optional(CONF_PANEL_WIDTH): even_pixels(CANVAS_MIN_WIDTH),
        cv.Optional(CONF_PANEL_HEIGHT): even_pixels(CANVAS_MIN_HEIGHT),
        # The canvas offered to apps that choose their size (and have no per-app
        # Screen setting); by default the whole panel. Apps that never ask keep
        # 800x480. See docs/esphome-store.md "Canvas size".
        cv.Optional(CONF_CANVAS_WIDTH): even_pixels(CANVAS_MIN_WIDTH),
        cv.Optional(CONF_CANVAS_HEIGHT): even_pixels(CANVAS_MIN_HEIGHT),
        cv.Optional(CONF_TOUCHSCREEN_ID): cv.use_id(touchscreen.Touchscreen),
        cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_BRIGHTNESS_NUMBER): cv.use_id(number.Number),
        cv.Optional("hardware", default=[]): cv.All(cv.ensure_list(HARDWARE_SCHEMA), validate_hardware),
        cv.Optional(CONF_AMBIENT_LIGHT_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_LVGL_ID): cv.use_id(LvglComponent),
        # When enabled, USB HID keys are delivered to LVGL's focus group.
        # Physical ADC/touch controls remain handled by the loader's legacy
        # path so both kinds of input can be used together without double
        # activating a widget.
        cv.Optional(CONF_LVGL_KEYPAD_NAVIGATION, default=False): cv.boolean,
        cv.Optional(CONF_TOGGLE_BUTTON): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_LAUNCH_BUTTON): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_FIRE_BUTTON): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_TOUCH_BUTTON): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_ADC_BUTTON_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_LEFT_STICK_X_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_LEFT_STICK_Y_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_RIGHT_STICK_X_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_RIGHT_STICK_Y_SENSOR): cv.use_id(sensor.Sensor),
        # Optional because network loading and the PAX runtime can be used
        # without USB HIDX. When present, bind to the existing USBHIDX ID.
        cv.Optional(CONF_USB_HIDX_ID): cv.use_id(USBHIDXComponent),
        # A usb_midi device for apps (midi_read / midi_write).
        cv.Optional(CONF_USB_MIDI_ID): cv.use_id(UsbMidi),
        # Retry the SD mount when a card was inserted or remounted after boot.
        cv.Optional(CONF_SD_MMC_ID): cv.use_id(sd_mmc_card.SdMmc),
        # Expose the attached USB mass-storage volume to launcher settings.
        cv.Optional(CONF_USB_MSC_ID): cv.use_id(UsbMsc),
        **BUTTON_SCHEMAS,
    }
).extend(cv.COMPONENT_SCHEMA)
CONFIG_SCHEMA = cv.All(
    CONFIG_SCHEMA, cv.has_at_most_one_key(CONF_CATALOG_URL, CONF_CATALOGS), validate_catalogs, validate_canvas
)


def _require_runtime_lvgl_widgets(config):
    # The PAX store builds its search textarea and keyboard dynamically in
    # C++; they are not represented by YAML widget declarations, so LVGL's
    # normal usage scan would otherwise compile their APIs out.
    if config.get(CONF_LVGL_ID):
        lvgl_helpers.add_lv_use("keyboard", "textarea")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _require_runtime_lvgl_widgets)


def _require_tls_hashes(config):
    # ESPHome turns SHA-384/512 off on ESP-IDF 6 in the esp32 component's own
    # to_code, which runs before ours: asking from our to_code is too late.
    # Ask while validating. Let's Encrypt's current ECDSA chains (YE1, Root YE,
    # ISRG Root X2) are all signed ecdsa-with-SHA384.
    if hasattr(esp32, "require_mbedtls_sha512"):
        try:
            esp32.require_mbedtls_sha512()
        except KeyError:
            pass  # esp32 data not set up yet; to_code sets the options itself
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _require_tls_hashes)
# Room in lwIP's socket pool for the apps' TLS sessions (net_tls_*, at most
# APP_TLS_MAX in pax_loader.cpp).
APP_TLS_SESSIONS = 4
if hasattr(socket, "consume_sockets"):
    CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, socket.consume_sockets(APP_TLS_SESSIONS, "pax_loader TLS"))


async def to_code(config):
    registry_path = Path(__file__).resolve().parents[2] / "installer-profiles.json"
    registry = json.loads(registry_path.read_text()) if registry_path.is_file() else {}
    filename = Path(CORE.config_path).name
    aliases = {"esp32-p4-elecrow-pax-3button.yaml": "elecrow",
               "esp32-p4-waveshare-pax-3button.yaml": "waveshare",
               "esp32-p4-waveshare-7b-pax-3button.yaml": "waveshare7b"}
    profile = next((key for key, value in registry.items() if value == filename), aliases.get(filename, ""))
    build_id = os.environ.get("PAX_FIRMWARE_BUILD_ID", os.environ.get("GITHUB_RUN_ID", "0"))
    if not build_id.isdecimal() or int(build_id) >= 2**64:
        raise ValueError("Firmware build ID must be an unsigned 64-bit integer")
    cg.add_define("PAX_FIRMWARE_PROFILE", profile)
    cg.add_define("PAX_FIRMWARE_BUILD_ID", cg.RawExpression(build_id + "ULL"))
    # CMake can refresh ESP-Hosted after pre scripts run. Apply the DMA
    # adaptation after framework configuration, before object signatures.
    hosted_post = Path(__file__).resolve().parents[2] / "tools" / "hosted_spi_post.py"
    if hosted_post.is_file():
        cg.add_platformio_option("extra_scripts", [f"post:{hosted_post}"])
    # esp_http_client is intentionally omitted from ESPHome's default P4
    # component set. Pull it in here because network PAX loading uses it
    # directly, and enable the IDF TLS component for HTTPS URLs.
    esp32.include_builtin_idf_component("esp_driver_jpeg")
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.include_builtin_idf_component("esp-tls")
    # ESPHome renamed this helper between releases. Either variant enables
    # the IDF CA bundle used by the HTTPS path; HTTP remains available too.
    if hasattr(esp32, "require_certificate_bundle"):
        esp32.require_certificate_bundle()
    elif hasattr(esp32, "require_full_certificate_bundle"):
        esp32.require_full_certificate_bundle()
    # Apps' TLS connections (net_tls_*) verify servers against the same bundle.
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    # And set them here as well: this runs after the esp32 component's to_code,
    # so it wins over its "off" even on ESPHome versions whose require helper
    # is missing or read too early (_require_tls_hashes). Without SHA-384 the
    # handshake fails with X509 verify errors (-0x2700) on those chains.
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_SHA384_C", True)
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_SHA512_C", True)
    # Archive.org currently presents a cross-signed GoDaddy chain. Let the
    # ESP-IDF bundle build a synthetic trust anchor for that chain instead of
    # rejecting it when the server includes the older cross-signed root.
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY", True)
    # With SNTP configured by the device YAML, also validate certificate
    # validity windows rather than relying only on the CA and hostname.
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_HAVE_TIME_DATE", True)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_path(config[CONF_PATH]))
    cg.add(var.set_autostart(config[CONF_AUTOSTART]))
    if catalog_url := config.get(CONF_CATALOG_URL):
        cg.add(var.set_catalog_url(catalog_url))
    catalogs = config.get(CONF_CATALOGS, [])
    for index, catalog in enumerate(catalogs):
        cg.add(var.add_catalog(catalog[CONF_NAME], catalog[CONF_URL]))
        for action in catalog[CONF_ACTIONS]:
            trigger = cg.new_Pvariable(action[CONF_TRIGGER_ID])
            await automation.build_automation(trigger, [], action)
            cg.add(var.add_catalog_action(index, action[CONF_LABEL], trigger))
    for source in config.get(CONF_ARCHIVE_SOURCES, []):
        cg.add(
            var.add_archive_source(
                source[CONF_NAME],
                source[CONF_URL],
                source[CONF_DESTINATION],
                source[CONF_EXTENSIONS],
            )
        )
    cg.add(var.set_archive_search_collection(config[CONF_ARCHIVE_SEARCH_COLLECTION]))
    if catalogs:
        default = config.get(CONF_DEFAULT_CATALOG, catalogs[0][CONF_NAME]).lower()
        index = next(i for i, catalog in enumerate(catalogs) if catalog[CONF_NAME].lower() == default)
        cg.add(var.set_initial_catalog(index))
    cg.add(var.set_store_ui(config[CONF_LIBRARY_STYLE] == "grid"))
    cg.add(var.set_install_dir(config[CONF_INSTALL_DIR]))
    if report_url := config.get(CONF_REPORT_URL):
        cg.add(var.set_report_url(report_url))
        cg.add(var.set_report_log_bytes(config[CONF_REPORT_LOG_BYTES]))
    cg.add(var.set_data_root(config[CONF_DATA_ROOT]))
    cg.add(var.set_download_data(config[CONF_DOWNLOAD_DATA]))
    for root in config[CONF_DATA_SEARCH]:
        cg.add(var.add_data_search_root(root))

    display_var = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(display_var))
    if mipi_dsi_id := config.get(CONF_MIPI_DSI_ID):
        cg.add(var.set_mipi_dsi(await cg.get_variable(mipi_dsi_id)))
    if CONF_PANEL_WIDTH in config:
        cg.add(var.set_panel_size(config[CONF_PANEL_WIDTH], config[CONF_PANEL_HEIGHT]))
    if CONF_CANVAS_WIDTH in config:
        cg.add(var.set_default_canvas(config[CONF_CANVAS_WIDTH], config[CONF_CANVAS_HEIGHT]))

    if touchscreen_id := config.get(CONF_TOUCHSCREEN_ID):
        touchscreen_var = await cg.get_variable(touchscreen_id)
        cg.add(var.set_touchscreen(touchscreen_var))

    if speaker_id := config.get(CONF_SPEAKER_ID):
        speaker_var = await cg.get_variable(speaker_id)
        cg.add(var.set_speaker(speaker_var))

    if brightness_number_id := config.get(CONF_BRIGHTNESS_NUMBER):
        brightness_number_var = await cg.get_variable(brightness_number_id)
        cg.add(var.set_brightness_number(brightness_number_var))

    for channel in config["hardware"]:
        for key, method in (
            ("sensor_id", "add_hardware_sensor"),
            ("binary_sensor_id", "add_hardware_binary"),
            ("number_id", "add_hardware_number"),
        ):
            if key in channel:
                entity = await cg.get_variable(channel[key])
                cg.add(getattr(var, method)(channel[CONF_NAME], channel["unit"], entity))
    if ambient_light_sensor_id := config.get(CONF_AMBIENT_LIGHT_SENSOR):
        ambient_light_sensor_var = await cg.get_variable(ambient_light_sensor_id)
        cg.add(var.set_ambient_light_sensor(ambient_light_sensor_var))

    if lvgl_id := config.get(CONF_LVGL_ID):
        cg.add_define("PAX_LOADER_USE_LVGL")
        lvgl_var = await cg.get_variable(lvgl_id)
        cg.add(var.set_lvgl(lvgl_var))
    cg.add(var.set_lvgl_keypad_navigation(config[CONF_LVGL_KEYPAD_NAVIGATION]))

    if toggle_button_id := config.get(CONF_TOGGLE_BUTTON):
        toggle_button_var = await cg.get_variable(toggle_button_id)
        cg.add(var.set_toggle_button(toggle_button_var))

    if launch_button_id := config.get(CONF_LAUNCH_BUTTON):
        launch_button_var = await cg.get_variable(launch_button_id)
        cg.add(var.set_launch_button(launch_button_var))

    if fire_button_id := config.get(CONF_FIRE_BUTTON):
        fire_button_var = await cg.get_variable(fire_button_id)
        cg.add(var.set_fire_button(fire_button_var))

    if touch_button_id := config.get(CONF_TOUCH_BUTTON):
        touch_button_var = await cg.get_variable(touch_button_id)
        cg.add(var.set_touch_button(touch_button_var))

    for key, setter in (
        (CONF_ADC_BUTTON_SENSOR, "set_adc_button_sensor"),
        (CONF_LEFT_STICK_X_SENSOR, "set_left_stick_x_sensor"),
        (CONF_LEFT_STICK_Y_SENSOR, "set_left_stick_y_sensor"),
        (CONF_RIGHT_STICK_X_SENSOR, "set_right_stick_x_sensor"),
        (CONF_RIGHT_STICK_Y_SENSOR, "set_right_stick_y_sensor"),
    ):
        if sensor_id := config.get(key):
            sensor_var = await cg.get_variable(sensor_id)
            cg.add(getattr(var, setter)(sensor_var))

    if usb_hidx_id := config.get(CONF_USB_HIDX_ID):
        cg.add_define("PAX_LOADER_USE_USB_HIDX")
        usb_hidx_var = await cg.get_variable(usb_hidx_id)
        cg.add(var.set_usb_hidx(usb_hidx_var))

    if usb_midi_id := config.get(CONF_USB_MIDI_ID):
        cg.add_define("PAX_LOADER_USE_USB_MIDI")
        cg.add(var.set_usb_midi(await cg.get_variable(usb_midi_id)))

    if sd_mmc_id := config.get(CONF_SD_MMC_ID):
        cg.add_define("PAX_LOADER_USE_SD_MMC")
        cg.add(var.set_sd_mmc(await cg.get_variable(sd_mmc_id)))

    if usb_msc_id := config.get(CONF_USB_MSC_ID):
        cg.add_define("PAX_LOADER_USE_USB_MSC")
        cg.add(var.set_usb_msc(await cg.get_variable(usb_msc_id)))

    for name, index in BUTTON_FIELDS.items():
        key = f"button_{name}"
        if sensor_id := config.get(key):
            sensor_var = await cg.get_variable(sensor_id)
            cg.add(var.set_button(index, sensor_var))


@automation.register_action(
    "pax_loader.launch_url",
    LaunchUrlAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PaxLoader),
            cv.Required("url"): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def pax_loader_launch_url_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    action = cg.new_Pvariable(action_id, template_arg, parent)
    url = await cg.templatable(config["url"], args, cg.std_string)
    cg.add(action.set_url(url))
    return action


@automation.register_action(
    "pax_loader.launch_rom",
    LaunchRomAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PaxLoader),
            cv.Required("emulator"): cv.templatable(cv.string),
            cv.Required("rom"): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def pax_loader_launch_rom_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    action = cg.new_Pvariable(action_id, template_arg, parent)
    emulator = await cg.templatable(config["emulator"], args, cg.std_string)
    rom = await cg.templatable(config["rom"], args, cg.std_string)
    cg.add(action.set_emulator(emulator))
    cg.add(action.set_rom(rom))
    return action
