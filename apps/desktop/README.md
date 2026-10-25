# Workspace: direct VNC and SVP Relay

Workspace (`desktop.pax`) 0.6.1 is an ESP32-P4 PAX app. **CONNECT** opens a VNC session
itself. The package remains `desktop.pax` and keeps `/sd/roms/desktop/` for existing saved connections and relay settings. **LISTEN** receives SVP Relay desktop/dashboard JPEG pictures on TCP
port **5000**. **VIA RELAY** asks Home Assistant to start a VNC desktop and feed
it to that receiver. These modes use the current PAX firmware's service table;
no new `simple_video_player` component is needed. The optional on-panel media browser requires the firmware bridge described below.

## Browse and play media on the panel

Choose **MEDIA** on the connection screen or **shared menu > Workspace > Media browser**.
**HA** lists Home Assistant media sources; tap a folder, then an audio or video file.
**SD** lists supported files beneath `/sd/media`. **UP**, **LOAD**, **PREV** and
**NEXT** navigate folders, refresh and change pages. Arrow keys move the selection;
Enter opens it, Backspace goes up, Escape closes the browser, and the mouse wheel
scrolls. Gamepad: arrows select/page, A opens, B goes up, X switches HA/SD, Y reloads, Start pauses/resumes, Select stops and L toggles mute. The bitmap font substitutes `?` for non-ASCII
title characters; file IDs retain their original UTF-8 bytes.

Music keeps the list visible. Video switches to the 512x300 receiver canvas.
**VIEW/CLOSE** returns to the picture; reopen Media browser through the shared menu.
The footer offers **PAUSE/RESUME**, **STOP** and **MUTE/UNMUTE**. Its pause/mute
labels track commands sent from this app; external HA changes are not synchronized.

The bridge uses the existing ESPHome native API connection, with no HA token on SD.
Add `packages/pax-media-browser.yaml` to the firmware and set
`pax_media_relay_entity` to the SVP Relay media player and `pax_media_ha_address`
to Home Assistant's IPv4 address. The private three-button profile includes it.
The ESPHome integration must allow the device to perform Home Assistant actions.
Audio-only files need the relay's **Audio player** set to **PAX Audio**, from the
optional `packages/pax-media-player.yaml` package. Keep the app open while playing
video or an SD file. The relay performs decoding/transcoding; video performance
is unchanged from 0.5.0 and still needs optimization.

For SD media, use MP3, FLAC, WAV, MP4/M4V, AVI, MJPEG or MJPG files. The firmware
exposes only the currently selected file to the configured HA address on TCP 5001,
using an opaque temporary URL and HTTP range requests. The next selection revokes
the previous URL. Commands on that port accept loopback connections only. Put
files in `/sd/media` or its subfolders; other SD directories are excluded.
Limit each SD folder to 256 entries and each file to 2 GiB.
HA pages contain at most 32 entries; navigation history is limited to 16 folders.
Picture viewing, playlists, artwork and seek controls are not implemented yet.

## Configure the panel in SVP Relay

1. Load the updated `desktop.pax` from the PAX launcher's network library.
2. Choose **LISTEN**, or shared menu > Workspace > Listen for SVP Relay.
3. In Home Assistant's SVP Relay panel configuration, select this ESPHome device.
   Set **Panel address** to its reachable address (currently `10.13.37.159`, or
   leave empty if the ESPHome integration already has that address).
4. Set **Stream port** to **5000**. No `simple_video_player` status sensor is
   published by the app, so enter this port manually.
5. **Audio player** is optional for desktop/dashboard pictures. Select a real
   media-player entity if you want the relay to route sound through that entity;
   leave the audio-output selectors empty unless the device actually provides
   the described `audio_router` select. The app does not invent those entities.
6. For the dashboard setup, follow the steps below.

## Show your Home Assistant dashboard

1. Open **DASHBOARD** on the app's connection screen (or shared menu > Workspace >
   Show dashboard). It opens the same port 5000 receiver as LISTEN and shows setup
   hints. The app receives a page rendered by the relay; it does not run Chromium.
2. In Home Assistant, open **Settings > Devices & services > SVP Relay**, configure
   **ESP32-P4 PAX Quick Launcher**, then choose **Settings**.
3. Set **Dashboard shown while nothing plays** to `/lovelace/default_view`.
   For this installation, the full URL is
   `http://10.42.42.52:8123/lovelace/default_view`.
4. Start with **Dashboard JPEG quality 60** and **Dashboard pictures per second 1**.
   Set the color scheme explicitly to Light or Dark if no Dashboard theme select
   is available. Leave the dashboard browser retention setting at your preference.
5. Save while the receiver is open. Complete the relay's **dashboard access**
   setup if prompted: choose an existing Home Assistant user and finish its login.
   This browser session belongs to the relay, independently of your normal browser.
   No HA token file is needed on the ESP32 just to receive dashboard pictures.
6. The relay shows this dashboard when no video, desktop or test is using the
   panel. If it previously tried while port 5000 was closed, save/reload its entry
   once the app is listening. A test finishing also resumes the idle dashboard.
7. Touch to operate cards; use a USB mouse/keyboard as needed. Shared menu >
   Workspace provides **Refresh page (F5)** and **Browser Back (Alt+Left)** after
   the relay subscribes to input. Browser typing sends separate text messages.

The relay needs Chromium on its Home Assistant host or configured worker.
Its README describes automatic Chromium setup for HAOS/container installs and
system Chromium for other hosts. If the panel stays on its waiting screen after
saving, check SVP Relay's Home Assistant logs for dashboard login/Chromium errors.
A displayed HA login page means the relay's browser authentication needs finishing.

Keep the app open in relay mode. The TCP listener is not active on the firmware's
three-button screen or while the app is in direct VNC mode. The receiver accepts
one sender; if it disconnects, LISTEN waits for another. Before its first picture, the sender has up to 30 minutes of silence for the
relay's initial Chromium installation/startup. After pictures begin, a sender
silent for 30 seconds is dropped; normal relay heartbeats keep it alive.
You can leave the receiver or exit the app during either wait.

## Video with sound through SVP Relay

1. Load the updated app, then choose **VIDEO/AUDIO** (shared menu > Workspace >
   Video / Audio via relay on smaller screens). Video/Audio mode uses a half-size canvas (512×300 on this 1024×600 board),
   upscaled by the firmware, and advertises mono 16 kHz sound. Dashboard/Listen
   keep the native canvas and stereo 48 kHz handshake. Both accept AVI/JPEG on
   port 5000, but Video/Audio is the intended low-bandwidth playback mode.
2. In Home Assistant's SVP Relay settings for this panel, start with **Frames per
   second 5**, **JPEG quality (ffmpeg -q:v) 15**, and **Sound codec Auto or PCM**.
   Auto selects PCM from this receiver's advertised codec mask. Keep dashboard
   quality/fps separate: those settings do not control video playback.
3. Open the panel's **SVP Relay media player** in Home Assistant and choose video
   from its media browser, or call `media_player.play_media` for that entity with
   a reachable video URL and `media_content_type: video/mp4` (use the source's real
   type). The relay runs ffmpeg, scales/letterboxes the source for the canvas and
   sends MJPEG video plus PCM sound together. The source need not already be AVI.
4. Sound embedded in this video plays through the ESP32's PAX speaker services.
   No separate HA Audio player entity is needed for embedded AVI sound. The HA
   **Audio player** selector still controls audio-only/announcement routing;
   this app does not create an ESPHome media-player API entity for those actions.
5. During AVI playback, shared menu > Workspace offers **Pause/Resume**, **Stop
   media (return to dashboard)** and **Mute/Unmute**. USB keyboard: **Space**,
   **Esc**, **M**. Pause stops reading the stream and uses TCP backpressure; sound
   already queued in the speaker may finish before the pause becomes silent.
   Use the shared menu's volume control for speaker volume.
6. Normal completion reports `E 1`; Stop reports `E 0`, then closes the sender
   connection and keeps the receiver open. The relay can restore the idle
   dashboard. Disconnect desktop exits the receiver instead.

A dedicated audio task consumes a bounded stereo PCM ring independently of
JPEG decoding. The ring starts after a short prefill, feeds silence on underrun
instead of repeatedly stopping the speaker, and keeps queued samples while
paused. It drains at normal EOF and is flushed on Stop; the task is deleted
before its ring is freed. Mono conversion occurs before enqueueing.

Supported AVI payloads are baseline MJPEG with 16-bit little-endian PCM, mono or
stereo at 8–48 kHz, with finite or ffmpeg's endless RIFF/movi headers. PCM is the
only advertised AVI audio codec: manually forcing MP3/FLAC produces a clear
unsupported-format error. Parser buffers are bounded; partial reads, odd-byte
padding, nested LIST bounds, invalid dimensions, truncated EOF and playback
clocks are checked. Late video pictures may be skipped to catch up with sound.
Decoding is software: the earlier 1024x600 benchmark measured about 2 fps at the full resolution. Video/Audio mode reduces decoding and
network load, with an initial target of 5 fps. This remains a low-frame-rate
playback option. Full SPICE main/audio/USB channels remain
unimplemented.

## Standalone music and announcements

The Elecrow three-button firmware can expose **PAX Audio** by including
`packages/pax-media-player.yaml`. This is a firmware feature alongside the app;
updating `desktop.pax` alone does not add the ESPHome media-player entity.
In SVP Relay's panel Settings, choose that device's **PAX Audio** as **Audio
player**, then send MP3/music media sources to the relay's own Player entity.
Home Assistant transcodes them to the native player's advertised mono 16 kHz
FLAC format. Music and announcements use separate mixer paths from PAX video
sound, so the dashboard can remain visible. Playback, pause, stop, volume and
mute are controlled from Home Assistant. An on-panel music library/queue UI
is not included by this firmware bridge.

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
- **VIDEO/AUDIO**: receive relay video and embedded PCM sound.
- **DASHBOARD**: receive the configured idle dashboard, with setup hints.
- **LISTEN**: receive relay pictures; no HA token is required just to receive.
- **VIA RELAY**: request a relay VNC desktop using the fields and `relay.conf`.
- Touch in relay mode: contacts and release go to the relay; VNC turns these into
  left click/drag. USB mouse: movement, left/right/middle buttons and wheel.
- USB keyboard: press/release events become the relay's HID/codepoint messages.
  Repeat and modifier/chord behavior depend on the firmware HID/text events.
- Hold two fingers for 1.5 s while connected, or shared menu > Workspace > Disconnect,
  to return to the connection screen. Esc leaves the receiver while it is waiting.
- Hold Menu 3 s to exit to the launcher. The loader's synthetic Menu+X close
  control is supported too.

An app can open the receiver with `app_open("desktop", "relay", 1)` or
`app_open("desktop", "dashboard", 1)` for dashboard setup mode.
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
- Touch `T`, keyboard `K` and browser text `X`, pointer `P`, echo `R` and statistics `M` responses.
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
The development build stays `publish: false` while dashboard deployment and speaker validation remain in progress.

From `pax-modules`, run the protocol tests:

```sh
python3 -m unittest discover -s apps/desktop/tests -v
```

The C smoke tests in `tests/` exercise the full direct app, JPEG decode and HTTP
command path under AddressSanitizer/UndefinedBehaviorSanitizer. Compile with
`-DPAX_APP_SIDE=1` and `-I ../pax-esphome/components/pax_loader`; the full app uses
`main.c` via `native_smoke.c` plus `rfb.c`, `vnc_auth.c`, `relay.c`,
`relay_jpeg.c`, `relay_http.c`, `audio_buffer.c`, `media_client.c` and `jpeg/tjpgd.c`. `tests/dashboard_smoke.c` checks dashboard entry, disconnected controls, F5 and
Alt+Left (including an already held Alt modifier). The JPEG smoke accepts a saved
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

Validated 2026-10-16: protocol tests; sanitized native app, JPEG and HTTP smoke
tests; desktop TCP service tests; compiled RV32 receiver with the installed relay
encoder/parser (JPEG colors, cursor, keyboard/mouse/touch, echo, statistics and
API-start ordering). Direct VNC regression tests also pass. Physical ESP32-P4 receives and displays the relay test pictures: 1024x600 at
about 2.0 fps at quality 60, 1.7 fps at 75 and 1.5 fps at 90; the network probe
reported 2.1 Mbit/s and 53 ms echo. Red test indicators reflect the relay
performance thresholds. The Home Assistant dashboard was subsequently authenticated and displayed on
the physical ESP32-P4. AVI video and speaker validation are recorded separately
when tested; emulator results do not prove physical speaker output.

The `test_avi.py` tests include real ffmpeg AVI pipe output, PCM mono/stereo,
finite/endless RIFF, pause clocks, explicit stop/completion and malformed streams.
`test_avi_emulated.py` runs the compiled RV32 app with the installed relay's actual
`AviSender` and ffmpeg, verifying picture pixels and nonzero PCM submissions.

Hardware check, 2026-10-16: Home Assistant's SVP Relay fetched a local MP4 test,
transcoded it to AVI and the ESP32-P4 displayed its MJPEG test picture. Speaker
logs report 48 kHz stereo, running/unmuted and complete PCM writes. The diagnostic
audio capture contains the expected 440 Hz tone. Normal completion returned the
panel to its Home Assistant dashboard. Local speaker audibility remains a user
check; diagnostic capture proves the PCM path, not the acoustic output. The
0.4.1 timing fix rebases a late frame's clock so later pictures can recover.

The initial 1024×600/stereo 48 kHz hardware test was too slow and the user heard
clicking despite valid PCM writes. Version 0.5.0 therefore uses a 512×300 video
canvas, mono 16 kHz negotiation, and a separately paced audio task with prefill.
The native threaded audio test covers conversion, pacing, pause, mute and task
cleanup under ASan/UBSan. The RV32 integration test checks the real sender's
512×300/mono/16 kHz negotiation and nonzero audio with a running guest audio task.

Physical 0.5.0 check, 2026-10-16: the user confirmed faster picture motion with
the 512×300 mode and a steady speaker tone, with brief interruptions when
playback stalls. This is improved low-frame-rate playback; occasional stalls
and corresponding audio gaps remain. The test used 5 fps, ffmpeg JPEG quality
15 and mono 16 kHz PCM. Dashboard regression, the compiled real-sender video
test, 34 protocol tests and firmware configuration validation passed.

`tests/media_client_smoke.c` checks Unicode/truncated JSON, paging bounds, SD sorting/filtering, fragmented HTTP, request escaping and cancellation. The firmware `tests/media_bridge_protocol.cpp` checks HTTP framing, range requests and SD path boundaries.
