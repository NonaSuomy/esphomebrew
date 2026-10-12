# Hosted Wi-Fi with PAX

For the ESP32-P4 with an ESP32-C6 SPI radio, set `use_psram: true` in the `esp32_hosted` block alongside the board-specific SPI pins. The display and launcher consume internal RAM; the default transport mempool can otherwise fail at boot with `spi_mempool_create` and “no mem”. This option enables `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` using the workspace’s hosted driver support.

Keep Wi-Fi passwords in local configuration or ESPHome secrets, outside committed files. Wi-Fi builds containing local credentials stay in ignored compiler output; the distributable Ethernet artifacts remain in pax-dev/firmware.

After changing LANs, set the development-server address in SD `roms/pax/.launcher_ui_network_url.txt`, with a trailing slash, and restart launcher_ui. Start the server with pax-tools/serve-dev.sh to supply file details.

The IDF 6.1 build compatibility hook sets `SPI_TRANS_DMA_USE_PSRAM` on hosted SPI transactions when PSRAM pools are enabled. Without it, the driver rejects the external buffers despite their DMA alignment. The patch is conditional and idempotent.

The compatibility hook also lets ESPHome reuse an already installed GPIO ISR service. Hosted SPI initializes that service before touchscreen setup; treating `ESP_ERR_INVALID_STATE` as an existing service preserves touchscreen interrupts.
