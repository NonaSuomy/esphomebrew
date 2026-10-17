# M3 LCD101 device setup fix — 2026-10-17

The M3 LCD101 browser installer image and flash kit now include the shared PAX
setup-page address fix. A request to `http://DEVICE_IP:8080/pax` validates the
actual local address of the accepted connection as well as the device hostname.
This allows setup through a secondary interface when the preferred interface's
address differs. Unknown Host names, wrong ports, missing session tokens and
foreign Origins remain rejected.

The build retains the M3's 1280×800 panel, landscape/touch mapping, pinout and
360 MHz revision 0/1 target. It uses the current dual-network configuration:
Ethernet first, provisioned C6 Wi-Fi as fallback. Revision 3 hardware still needs
a separate target; this release does not change chip revision support.

The firmware compiled successfully and panel mapping checks passed. The shared
setup fix was verified on an ESP32-P4 Elecrow device. M3 hardware testing remains
pending; the installer continues to label this board untested.

The exact source change is in
[pax-device-setup-secondary-interface.patch](pax-device-setup-secondary-interface.patch).
Apply it from the `pax-esphome` directory of the full PAX source installer.
The flash kit contains the updated M3 configuration and panel adapter for reference.
The installer build-info record contains the binary SHA-256 and source revisions.
Other boards' firmware images are unchanged by this publication.

## Hosted Wi-Fi wiring in this build

Both network interfaces are enabled. Ethernet is preferred, and Wi-Fi remains
available when the cable is disconnected. Provision Wi-Fi through serial Improv
or the `PAX Network Setup` fallback access point. The public image contains no
saved station credentials.

```yaml
esp32_hosted:
  active_high: true
  variant: ESP32C6
  reset_pin: GPIO54
  cmd_pin: GPIO19
  clk_pin: GPIO18
  d0_pin: GPIO14
  d1_pin: GPIO15
  d2_pin: GPIO16
  d3_pin: GPIO17
  use_psram: true
  type: sdio
  slot: 1

network:
  priority:
    - ethernet
    - wifi
```

The SD card uses slot 0, separate from the hosted Wi-Fi SDIO slot 1.
