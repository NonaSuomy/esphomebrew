# Bluetooth settings

The three-button interface offers **NETWORK SETUP → BLUETOOTH DEVICES**.
Launcher UI 0.6.2 offers **Settings → Network → BLUETOOTH DEVICES**.
Put an input device into pairing mode, choose **SCAN NEARBY**, select it, then
choose **CONNECT**. A keyboard pairing code is shown when the keyboard requires
one; type it on that keyboard and press Enter.

The companion reports its capabilities. ESP32-C6 provides BLE. An original
ESP32-WROOM companion can provide Classic and BLE with the corresponding hosted
Bluetooth build and board wiring. The P4 settings/command protocol supports both;
the AIO C6 updater remains restricted to approved C6 images and must not be used
to flash a WROOM module. A WROOM AIO module/wiring profile and hardware validation
are still needed. A module marketed as WROOM is not by itself a guarantee of
Classic support; the underlying chip determines the transports.

Discovery also lists nearby non-HID advertisements/inquiry results, marked
**Nearby only**. CONNECT is restricted to supported HID devices. Discovery does
not mean that a device supports a useful connection profile. Bluetooth audio,
sensors and other GATT/vendor profiles need separate integrations. The reference
p4-panel-workspace Elecrow configuration uses a USB Realtek Bluetooth adapter
with its own Classic/audio stack; those audio functions cannot run on a C6 radio.

Explicit scans are bounded; bonded devices reconnect using periodic searches.
New devices do not connect merely because they advertise nearby. Bonds reside in
the companion's NVS. Input reports keep the existing ABI-1 file/gamepad/keyboard/
mouse services; radio link loss releases held keyboard keys and mouse buttons.
The versioned custom-data protocol carries discovery separately from the legacy
BP32/BP3K/BP3M input packets.

## AIO C6 image

The stock ESP-Hosted 2.12.12 image and its approved hash remain available.
`c6/2.12.12/aio-spi-bluetooth/network_adapter.bin` is the optional local Bluedroid
HID variant. Its independent approved hash is recorded in
`components/pax_loader/c6-firmware.json`. It retains AIO full-duplex SPI mode 3:
MOSI 20, MISO 21, CLK 18, CS 19, handshake 23, data-ready 22 on the C6.
Both original 1920 KiB OTA slots are retained; only the application is updated.
A Bluetooth initialization error is logged without aborting the Wi-Fi service.

Copy that image to `/sd/firmware/c6/aio-spi-bluetooth.bin`, select that path in the
existing C6 updater, verify it, then install. An explicit Bluetooth variant
change is permitted at the same upstream networking version. Arbitrary images,
incorrect wiring profiles and incompatible versions remain rejected. The web
API accepts the same explicit path/URL in `X-PAX-C6-Source` on both its check and
install requests; the token, profile confirmation and integrity checks still
apply. Default board-recommended checks continue to use the stock image.

`pax_bluetooth_setup` and `pax_bluetooth_scan` are ESPHome API actions for opening
the native settings and requesting a scan. Scan progress/counts are logged.

## Rebuilding the companion

Use `pax-recipe/firmware/bluedroid-hid/prepare-hosted.py` with the pinned
ESP-Hosted 2.12.12 component and a fresh work directory. It verifies the source
receipt, creates a separate project, retains the transport/partition layout,
and adds the local Bluedroid backend. Use ESP-IDF 6.1 with its GCC 15.2 tools.
For direct CMake configuration set `ESP_IDF_VERSION=6.1`; normal IDF tooling
should use its matching environment. The previous 2.12.3 import remains intact.
Review a rebuilt binary's effective configuration and update its approval hash
before installing it. Do not flash a standalone HID image as an ESP-Hosted slave.

Software checks include C6 combined and original ESP32 Classic+BLE builds,
parser/report replays, discovery/input loss tests, real LVGL panel tests, and C6
image/OTA fault tests. Physical pairing remains untested without an available
input device. Hardware scan results are recorded in workspace validation data.
