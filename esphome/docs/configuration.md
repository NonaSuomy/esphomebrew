# Shared PAX firmware configuration

Every active entry point uses `packages/pax-base.yaml`. Change common build settings, USB keyboard/gamepad/mouse input, the three-button launcher, logging, OTA, Improv serial SD uploads and PAX runtime services there. `device_control.yaml` remains the shared native API and port 8080 device page.

The layers are:

| Layer | Location | Purpose |
| --- | --- | --- |
| Shared firmware | `packages/pax-base.yaml` | ESP-IDF options, PSRAM, components, controls, runtime and three-button UI |
| Optional audio | `packages/pax-audio.yaml` | Mixer and resamplers; the board supplies `speaker_output` |
| Optional brightness | `packages/pax-backlight.yaml` | Brightness setting; the board supplies `display_backlight` |
| Hardware | `boards/<manufacturer>/<board>.yaml` | Flash/revision, GPIOs, SD bus, codec, power, display and touch |
| Display combinations | `displays/hdmi720.yaml`, `displays/hdmi1080.yaml` | Shared ECA20-MHA1-I bridge timings, independent of board GPIOs |
| Device entry point | `esp32-p4-*-pax-*.yaml` | Device name, package selection, canvas/button geometry and Wi-Fi provisioning |
| Build aliases | `profiles.json` | One registry used by `build-firmware.sh` |
| Provenance | `board-sources.json` | Source revisions, schematics and unresolved hardware differences |

The existing Elecrow and Waveshare filenames and build aliases continue to work. Public `*-web` entry points contain a setup hotspot and use Improv to provision Wi-Fi after flashing. Private `*-3button` Wi-Fi profiles use `secrets.yaml`. Ethernet profiles do not require Wi-Fi credentials. Staging checks the complete local include closure so credentials in a shared package cannot slip into a public image.

The original `esp32-p4-elecrow-pax.yaml` is a legacy full-launcher reference. It is not in the active build registry. Do not add new shared features there.

## Build and upload

From the root of the PAX workspace:

```sh
./pax-tools/build-firmware.sh config jc1060
./pax-tools/build-firmware.sh compile jc1060
./pax-tools/build-firmware.sh upload jc1060 --device /dev/ttyUSB0
```

After network setup, replace the serial port with the device IP for OTA. Use the board's programming port. Tab5 uses USB Serial/JTAG, commonly `/dev/ttyACM0`; other boards commonly use a USB-to-UART bridge. A different display or silicon revision needs its matching profile.

`python3 pax-tools/firmware-profile.py unknown` prints all available build aliases. The starter SD files, browser uploads and store downloads use the same services on every profile. The source package includes the shared layers and the pinned JC8012 GSL3680 driver. Additional keys include `espressif-ev`, `olimex` and `firebeetle2`; their [manufacturer pinouts](pinouts.md) record the exact board scope.

## Adding a board

Create a hardware package under its manufacturer's directory. Keep the common component IDs: `main_display`, `sd_card`, `usb_storage`, `usb_hidx_component`, `pax_runtime` and `lvgl_component`. Touch profiles use `touch_panel`; audio profiles provide `speaker_output`; brightness profiles provide `display_backlight`.

Select only verified GPIOs and a known panel model or exact timings. Set flash capacity and `engineering_sample` for the actual chip revision. Keep the SD card on slot 0 when hosted Wi-Fi uses SDIO slot 1. HDMI profiles explicitly disable LVGL touchscreen auto-discovery. Smaller native panels use an even canvas that fits the panel; unchanged apps that assume 800x480 or larger may need their own screen adaptation.

Create a thin entry point that includes the base and hardware packages. Add audio and brightness only when supported. Set `panel_width`, `panel_height`, `canvas_width`, `canvas_height` and `pointer_dimensions`. The button size and positions are substitutions, so portrait panels can stack the three buttons without duplicating their callbacks. Add the alias to `profiles.json` and record sources and scope in `board-sources.json` and `docs/boards.md`.

Run configuration validation, full compilation and `tests/check_board_profiles.py`. Hardware testing must cover display, touch, SD read/write, network, USB input and audio before changing its test status. The current new board profiles are experimental; compilation does not demonstrate correct electrical wiring or panel behavior.

## Upstream updates

Pinout examples are adapted from the supplied device pages and the pinned jtenniswood repository revision recorded in `board-sources.json`. Upstream UI automations, voice assistant features, credentials and unrelated peripherals are omitted. To update a base, compare its pinned revision against upstream, apply hardware differences only to `boards/`, then validate and compile affected profiles. Fixes remain on the owning repository's `pax/workspace-20261008` branch.

The JC8012 touch component is vendored from `kvj/esphome` at the revision in `components/gsl3680/UPSTREAM.json`, with its license. Review upstream diffs before refreshing those files and keep local compatibility fixes in the PAX branch. This makes builds reproducible and keeps the source installer independent of a moving remote branch.

## Desktop preview

The [SDL2 desktop profile](desktop-sdl.md) is built with `./pax-tools/build-firmware.sh run desktop`. Its screen comes from `packages/pax-three-button-ui.yaml`, also included by `pax-base.yaml`. It previews shared LVGL controls on the computer; ESP32-P4 PAX app binaries still need the device runtime.
