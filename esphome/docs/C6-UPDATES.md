# C6 firmware updates from PAX settings

The active ESPHome P4 loader and `launcher_ui.pax` expose **C6 firmware path / URL**, **Check firmware**, and **Install & restart** in Settings. Install the updated P4 firmware and launcher together. Other targets show the controls as unavailable.

1. Copy a raw ESP-Hosted C6 application image to the card as `/firmware/network_adapter.bin`. Its PAX path is `/sd/firmware/network_adapter.bin`. Alternatively enter another `/sd/...` or `/usb0/...` path, or an HTTP(S) URL pointing directly to the `.bin` (including a server on your LAN). Use the on-screen keyboard and commit the field as for the network settings.
2. Choose **Check firmware**. All sources are copied to `/sd/roms/pax/.c6-update.bin`; an SD card with sufficient free space is required even for web/USB sources. Downloads support redirects, HTTP Content-Length and chunked transfer, with TLS certificate validation for HTTPS. The source field lasts for the launcher session.
3. Review the current and proposed C6 versions. Confirm that the image is built for your board's transport and pinout: Elecrow SPI and Waveshare SDIO images are different even though both target C6. The binary cannot prove its wiring matches your board.
4. Choose **Install & restart**. A graphical progress bar, percentage, byte counts and phase text refresh automatically while staging, verifying and writing. The Settings page stays open during these operations. Keep power connected. The P4 restarts after the C6 activates the new image.

Use an OTA application image (`network_adapter.bin`), not a merged/factory image, bootloader, partition table, P4 image, or standalone Bluetooth firmware. The PAX combined Bluedroid/ESP-Hosted C6 build is an eligible application image; using stock ESP-Hosted firmware replaces its custom Bluetooth functionality. Images must carry the ESP-IDF appended SHA-256 and the `network_adapter` project descriptor. Validation checks C6 chip ID, segment bounds, XOR checksum, full SHA-256, exact image size and a compatible ESP-Hosted version (same major, no newer than the P4 library). It runs again before writing. This is an integrity check; obtain firmware from a trusted source.

The P4 calls the same `esp_hosted_slave_ota_begin/write/end/activate` APIs used by ESPHome's `esp32_hosted` update component, using 1500-byte RPC chunks. Downloads finish before flashing starts, avoiding dependence on the C6 Wi-Fi link during installation. Failed writes/finalization never call activation. Mount/eject requests through PAX are rejected while staging or flashing. Images larger than the C6's actual OTA slot are rejected by its OTA implementation; the local staging limit is 4 MiB.

A responsive, compatible ESP-Hosted C6 with OTA partitions is required. This cannot perform an initial flash or recover a C6 that no longer communicates; use USB/UART for those cases. Copying a file to the card does not automatically flash it. ESPHome JSON manifests and automatic update checks are not accepted by this direct-image field.

Validation: configuration validation, P4 build and launcher build; host-side corruption, truncation and simulated begin/write/end/activate failures. These checks do not establish hardware OTA success; C6 OTA activation has not been tested.

Implementation reference: local ESPHome `dependencies/esphome/esphome/components/esp32_hosted/update/esp32_hosted_update.cpp`, and [ESPHome ESP32 Hosted update documentation](https://esphome.io/components/update/esp32_hosted/).

Verification does not activate or reboot the C6. A verified image may reach 100% while the status says it is checking the C6 connection. UI bridge delays are retried so a slow SD/RPC operation does not close the launcher. The IDF 6.1 hosted SPI adapter enables PSRAM DMA and allows alignment buffers; its patch is reapplied after CMake refreshes managed components.
