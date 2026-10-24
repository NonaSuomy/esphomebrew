# Shared PAX firmware configuration

Every active entry point uses `packages/pax-base.yaml`. Change common build settings, USB keyboard/gamepad/mouse input, the three-button launcher, logging, OTA, Improv serial SD uploads and PAX runtime services there. `device_control.yaml` remains the shared native API and port 8080 device page.

The layers are:

| Layer | Location | Purpose |
| --- | --- | --- |
| Shared firmware | `packages/pax-base.yaml` | ESP-IDF options, PSRAM, components, controls, runtime and three-button UI |
| Optional audio | `packages/pax-audio.yaml` | Mixer and resamplers; the board supplies `speaker_output` |
| Home Assistant audio player | `packages/pax-media-player.yaml` | Music and announcements on separate mixer inputs alongside PAX |
| Optional brightness | `packages/pax-backlight.yaml` | Brightness setting; the board supplies `display_backlight` |
| Dual networking | `packages/pax-network-dual.yaml` | Confirmed IP101 wiring, Ethernet-first routing and provisionable Wi-Fi fallback |
| Shared C6 SDIO | `packages/pax-hosted-c6-sdio.yaml` | NANO, M3 and JC1060 I_W_Y hosted wiring on slot 1 |
| Hardware | `boards/<manufacturer>/<board>.yaml` | Flash/revision, GPIOs, SD bus, codec, power, display and touch |
| Display combinations | `displays/hdmi720.yaml`, `displays/hdmi1080.yaml` | Shared ECA20-MHA1-I bridge timings, independent of board GPIOs |
| Device entry point | `esp32-p4-*-pax-*.yaml` | Device name, package selection, canvas/button geometry and Wi-Fi provisioning |
| Build aliases | `profiles.json` | One registry used by `build-firmware.sh` |
| Provenance | `board-sources.json` | Source revisions, schematics and unresolved hardware differences |

The existing Elecrow and Waveshare filenames and build aliases continue to work. Public `*-web` entry points contain a setup hotspot and use Improv to provision Wi-Fi after flashing. Private `*-3button` Wi-Fi profiles use `secrets.yaml`. Ethernet-only profiles do not require Wi-Fi credentials. Dual-network profiles operate over Ethernet without Wi-Fi provisioning; provision Wi-Fi to enable wireless fallback. Staging checks the complete local include closure so credentials in a shared package cannot slip into a public image.

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

Select only verified GPIOs and a known panel model or exact timings. Set flash capacity and `engineering_sample` for the actual chip revision. Keep the SD card on slot 0 when hosted Wi-Fi uses SDIO slot 1. Include `pax-network-dual.yaml` only when both interfaces and the IP101 pinout are confirmed. Its `network.priority` is Ethernet first, followed by Wi-Fi. Both interfaces stay enabled, and Wi-Fi connection loss does not reboot the device while Ethernet is available. HDMI profiles explicitly disable LVGL touchscreen auto-discovery. Smaller native panels use an even canvas that fits the panel; unchanged apps that assume 800x480 or larger may need their own screen adaptation.

Create a thin entry point that includes the base and hardware packages. Add audio and brightness only when supported. Set `panel_width`, `panel_height`, `canvas_width`, `canvas_height` and `pointer_dimensions`. The button size and positions are substitutions, so portrait panels can stack the three buttons without duplicating their callbacks. Add the alias to `profiles.json` and record sources and scope in `board-sources.json` and `docs/boards.md`.

Run configuration validation, full compilation and `tests/check_board_profiles.py`. Hardware testing must cover display, touch, SD read/write, network, USB input and audio before changing its test status. The current new board profiles are experimental; compilation does not demonstrate correct electrical wiring or panel behavior.

## Upstream updates

Pinout examples are adapted from the supplied device pages and the pinned jtenniswood repository revision recorded in `board-sources.json`. Upstream UI automations, voice assistant features, credentials and unrelated peripherals are omitted. To update a base, compare its pinned revision against upstream, apply hardware differences only to `boards/`, then validate and compile affected profiles. Fixes remain on the owning repository's `pax/workspace-20261008` branch.

The JC8012 touch component is vendored from `kvj/esphome` at the revision in `components/gsl3680/UPSTREAM.json`, with its license. Review upstream diffs before refreshing those files and keep local compatibility fixes in the PAX branch. This makes builds reproducible and keeps the source installer independent of a moving remote branch.

## Desktop preview

The [SDL2 desktop profile](desktop-sdl.md) is built with `./pax-tools/build-firmware.sh run desktop`. Its screen comes from `packages/pax-three-button-ui.yaml`, also included by `pax-base.yaml`. It previews shared LVGL controls on the computer; ESP32-P4 PAX app binaries still need the device runtime.

## Music and announcements with SVP Relay

The Elecrow three-button profile includes `packages/pax-media-player.yaml` and
exposes **PAX Audio**, a normal ESPHome speaker media player. Other audio-capable
profiles can opt in by including this package after `pax-audio.yaml`.

In Home Assistant, configure the SVP Relay panel, open Settings, and select the
ESPHome device's **PAX Audio** entity under **Audio player**. Keep the relay's
own Player entity as the target for media browsing/playback; do not select it
as its own Audio player. Audio-only media sources (including MP3s) then use the
native speaker media player, while video keeps its embedded PCM sound through
`desktop.pax`. No SVP Relay source changes are needed.

Home Assistant converts audio to the advertised mono 16 kHz FLAC format. The
player has a 256 KiB encoded stream buffer and PSRAM decoder/resampler task stacks.
The optional package lowers the normal malloc internal-memory preference threshold
to 1 KiB, leaving internal DMA memory for I2S while the PAX display is active. Regular music
uses a third mixer input so it does not change the PAX application's input
format or volume. Announcements use the separate announcement input. Playback,
pause, stop, volume, mute and announcements are available through Home Assistant;
this does not add an on-panel music browser or queue/cover-art screen.

Device check, 2026-10-17: the user confirmed audible MP3 playback through
SVP Relay with the PAX receiver open. Pause/resume and stop state transitions,
volume/mute round trips, and a finite FLAC announcement returning to idle were
verified on the ESP32-P4. The initial speaker startup failure was traced to
internal DMA memory pressure and corrected with the allocation settings above.
SVP Relay controls whether its idle dashboard is shown during music/cover art;
this player does not add an on-panel cover-art interface.

The earlier quiet tone was not heard by the user. A subsequent spoken FLAC
announcement through SVP Relay was replayed on 2026-10-17 and the user confirmed
it was clearly audible. Player state alone does not verify acoustic output.

## Optional on-panel media browser

The Desktop 0.6.0 PAX app can browse Home Assistant media and `/sd/media` through
`packages/pax-media-browser.yaml`. Set `pax_media_relay_entity` to the SVP Relay
media player entity and `pax_media_ha_address` to HA's IPv4 address. Enable HA
actions in the device's existing ESPHome integration. This uses the native API,
without storing an HA token on SD. The private three-button profile includes it.

The bridge accepts app commands on loopback TCP 5001. The configured HA address
can fetch only the selected SD file via an opaque expiring URL, with HTTP ranges;
other remote addresses and remote commands are rejected. Keep media beneath
`/sd/media`; use subfolders with at most 256 entries and files no larger than
2 GiB. Music playback needs an Audio player in SVP Relay; the optional PAX Audio
package provides one. See `apps/desktop/README.md` for browser controls.

### Choose a nearby Wi-Fi network

In Launcher Settings, the WI-FI NAME row has a manual name field and a SCAN NEARBY picker. Open the picker to scan, then choose a network from the strongest-first list (signal in dBm, Open or Secured). Repeated radios with the same name/security are combined. Scan again refreshes the list. Enter manually is always available, including for hidden networks. Selecting a different secured network opens password entry; open networks clear any staged password. Nothing connects until APPLY WI-FI is selected. The scan runs in the background and does not change the current credentials or disconnect Wi-Fi. It requires firmware with the `/dev/pax-wifi-scan` endpoint; older firmware retains manual entry.

### Three-button network bootstrap

The three-button **NETWORK** action discovers PAX Supply before downloading
`launcher_ui.pax`. Discovery runs in the background download task, so it works
before Launcher UI itself is available. It respects the SD settings for automatic
versus manual Supply URLs and falls back to the cached/configured URL if no
server responds. Start the computer server with `./pax-tools/serve-dev.sh 8000`.

The **GITHUB** action uses the shared `launcher_github_url` from `pax-base.yaml`.
The old Elecrow/Waveshare override pointed at a missing `dev-builds` release
asset; these profiles now inherit the published GitHub Pages launcher URL.
