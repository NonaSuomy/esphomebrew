# Desktop: direct VNC and SVP Relay

`desktop.pax` 0.2.0 is an ESP32-P4 PAX app. **CONNECT** opens a VNC session
itself. **LISTEN** receives SVP Relay desktop/dashboard JPEG pictures on TCP
port **5000**. **VIA RELAY** asks Home Assistant to start a VNC desktop and feed
it to that receiver. These modes use the current PAX firmware's service table;
no firmware change or new `simple_video_player` component is needed.

## Configure the panel in SVP Relay

1. Load the updated `desktop.pax` from the PAX launcher's network library.
2. Choose **LISTEN**, or shared menu > Desktop > Listen for SVP Relay.
3. In Home Assistant's SVP Relay panel configuration, select this ESPHome device.
   Set **Panel address** to its reachable address (currently `10.13.37.159`, or
   leave empty if the ESPHome integration already has that address).
4. Set **Stream port** to **5000**. No `simple_video_player` status sensor is
   published by the app, so enter this port manually.
5. **Audio player** is optional for desktop/dashboard pictures. Select a real
   media-player entity if you want the relay to route sound through that entity;
   leave the audio-output selectors empty unless the device actually provides
   the described `audio_router` select. The app does not invent those entities.
6. A dashboard URL is optional. Set its color scheme explicitly to Light or Dark
   if the device has no Dashboard theme select. The integration renders a
   configured dashboard while idle and sends its JPEG pictures to LISTEN mode.
   A modest pictures-per-second value is useful initially: JPEG decoding here is
   software, and performance on physical hardware has not been measured.

Keep the app open in relay mode. The TCP listener is not active on the firmware's
three-button screen or while the app is in direct VNC mode. The receiver accepts
one sender; if it disconnects, LISTEN waits for another. A sender silent for
30 seconds is dropped; normal relay heartbeats keep it alive.

The relay's video/movie/camera pipeline sends **AVI streams**, which this app
cannot play. Supported receiver traffic is baseline JPEG desktop/dashboard
pictures, cursor packets and dashboard PCM audio, not every SVP Relay media mode.
The relay's full SPICE main/input/audio/USB channels are also not implemented.

## Start a VNC desktop through the relay

The relay's configuration screen registers the panel; it does not itself choose
a computer to connect to. The PAX app requests the desktop through the relay's
existing authenticated Home Assistant API.

Create `/sd/roms/desktop/relay.conf` with **two lines**:

```text
http://homeassistant.local:8123
YOUR_HOME_ASSISTANT_LONG_LIVED_ACCESS_TOKEN
```

Use your actual Home Assistant origin (hostname/IP and port, without a path), and
an access token for a Home Assistant user permitted to use the integration. This
is **not** the relay worker's token. `relay.conf.example` shows the format. HTTPS
is supported through the loader's certificate-verified TLS services; self-signed
certificates are rejected. Plain HTTP carries the token without encryption, so
use a trusted network. The token file itself is secret: keep it out of source
control and backups you share. The app never logs it.

On the app's connection screen, enter the **VNC computer's** hostname/IP, VNC TCP
port (usually 5900) and VNC password, then choose **VIA RELAY**. The app starts
listening on 5000 before making the request, and continues accepting/rendering
while Home Assistant connects. The panel must already be registered in SVP Relay
with a reachable address and port 5000. A 401 means the HA token was rejected;
a 404 means the panel/API registration was not found; a 502 means the relay could
not reach the computer or panel.

The VNC password is cleared after the relay request. **SAVE HOST** saves only
the computer host/port; it never saves the VNC password or writes `relay.conf`.

## Controls

- Connection screen: touch/on-screen keyboard or USB keyboard; Tab moves among
  fields/buttons, Enter advances through the fields then connects directly.
- **LISTEN**: receive relay pictures; no HA token is required just to receive.
- **VIA RELAY**: request a relay VNC desktop using the fields and `relay.conf`.
- Touch in relay mode: contacts and release go to the relay; VNC turns these into
  left click/drag. USB mouse: movement, left/right/middle buttons and wheel.
- USB keyboard: press/release events become the relay's HID/codepoint messages.
  Repeat and modifier/chord behavior depend on the firmware HID/text events.
- Hold two fingers for 1.5 s while connected, or shared menu > Desktop > Disconnect,
  to return to the connection screen. Esc leaves the receiver while it is waiting.
- Hold Menu 3 s to exit to the launcher. The loader's synthetic Menu+X close
  control is supported too.

An app can open the receiver with `app_open("desktop", "relay", 1)`.
A `computer.local:5900` launch argument still prefills the direct-VNC fields.

## Direct VNC

**CONNECT** preserves the original direct client: RFB 3.3/3.7/3.8, classic VNC
password/no-auth, RGB565, Hextile/CopyRect/Raw, resizing and aspect-preserving
scaling. The computer must offer a VNC server; the ESP32-P4 address is not the
computer's address. Classic authentication uses eight password bytes. Saved host
and port live in `/sd/roms/desktop/connection.txt`.

## Receiver implementation

The installed SVP Relay 0.9.42 sender was the compatibility reference:
`svp_core/link.py`, `udisp.py`, `page.py`, `probe.py`, and `desktop/caster.py`.
Wire support includes:

- `C`, `V`, `A`, `N`, `L`, `F`, `S` greeting, with an upright canvas (`turns=0`)
  and a 1 MiB maximum JPEG packet; no firmware ABI changes.
- JPEG type 3, input subscription 0x21, cursor shape/position 0x23/0x24,
  PCM 0x10, statistics 0x20, echo 0x22, heartbeat and throughput probe fill 0x7E.
- Touch `T`, keyboard `K`, pointer `P`, echo `R` and statistics `M` responses.
- Bounded parser work, payload/cursor limits, rectangle/JPEG dimension validation,
  partial reads/writes, outgoing backpressure and cancellation during decoding.

Baseline JPEG decoding uses ChaN's TJpgDec from the pinned LVGL source. Its
copyright/license and local changes are recorded in `jpeg/`. Cursor pixels are
alpha-composited over a separate picture buffer. Incoming dashboard PCM is sent
to the loader's audio services; this does not add ESPHome audio actions/entities
or a direct VNC audio channel. Receiver TCP traffic is unauthenticated and
unencrypted, so expose it only to a trusted network.

## Build and validation

From the PAX workspace, run `./pax-recipe/build-modules.sh desktop`.
The development build stays `publish: false` pending hardware validation.

From `pax-modules`, run the protocol tests:

```sh
python3 -m unittest discover -s apps/desktop/tests -v
```

The C smoke tests in `tests/` exercise the full direct app, JPEG decode and HTTP
command path under AddressSanitizer/UndefinedBehaviorSanitizer. Compile with
`-DPAX_APP_SIDE=1` and `-I ../pax-esphome/components/pax_loader`; the full app uses
`main.c` via `native_smoke.c` plus `rfb.c`, `vnc_auth.c`, `relay.c`,
`relay_jpeg.c`, `relay_http.c` and `jpeg/tjpgd.c`. The JPEG smoke accepts a saved
udisp JPEG packet; the HTTP smoke uses mock services and no real credentials.
In sandbox/ptrace environments, use `ASAN_OPTIONS=detect_leaks=0`; allocation
cleanup is also checked explicitly.

Test the compiled RISC-V binary with the installed relay source:

```sh
python3 apps/desktop/tests/test_relay_emulated.py \
  --workspace .. --pax ../pax-dev/apps/desktop.pax \
  --relay /path/to/svp-relay
```

This uses the actual installed `PanelLink` parser and `desktop_jpeg` function,
plus a **mock** authenticated HA endpoint and temporary test-only token. It does
not contact your real Home Assistant. PAX's desktop TCP listener binds loopback
only. The emulator integration tests require permission to open loopback sockets.
The existing `tests/test_emulated.py` covers direct VNC.

Validated 2026-10-16: 23 protocol tests; sanitized native app, JPEG and HTTP smoke
tests; desktop TCP service tests; compiled RV32 receiver with the installed relay
encoder/parser (JPEG colors, cursor, keyboard/mouse/touch, echo, statistics and
API-start ordering). Direct VNC regression tests also pass. Physical ESP32-P4,
real Home Assistant deployment and speaker performance remain untested.
