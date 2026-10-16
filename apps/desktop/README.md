# Desktop (VNC) for PAX

A standalone ESP32-P4 desktop viewer, based on the desktop-viewing use case of
`esphome006/config/lvgl_media.yaml`. This is a new implementation against the
current PAX service table; it does not require that ESPHome UI or its media player.

## Use

1. Enable a VNC server on the computer and allow its port through the LAN firewall.
   Choose **classic VNC password authentication** (or no authentication on an
   appropriately isolated network). TLS/VeNCrypt-only servers are not supported.
2. Open `desktop.pax` in the PAX launcher. Enter the computer's IPv4 address or
   hostname, its actual TCP port (normally `5900`), and the VNC password.
   This is the VNC server's password, which may differ from the computer login.
3. Choose **Connect**. A desktop near the panel's resolution reduces memory use
   and network traffic. Large desktop updates will be slower than small changes;
   frame rate depends on the server, desktop activity and network.

Touch the fields and use the on-screen keyboard, or use a USB keyboard. The
keyboard mode button cycles through lower case, upper case and symbols. Classic
VNC authentication uses up to eight password bytes. **Save Host** stores only the
host and port in `/sd/roms/desktop/connection.txt`. The password is never saved and
is cleared after a connection attempt reaches authentication or the app exits.
DNS/connection failures before authentication retain the typed password for retry.

During a session, touch sends a left click/drag; a USB mouse sends movement,
left/right/middle buttons and the wheel. A USB keyboard sends keys through the
loader's keyboard service. Keyboard repeat and modifier/chord behavior depend on
what events the firmware's HID/text service supplies.

Use **shared menu > Desktop > Disconnect desktop**, or hold two fingers for
1.5 seconds, to return to the connection page. Hold Menu for 3 seconds to exit.
The loader's close control is supported through its synthetic Menu+X input.
Before connecting, Esc or gamepad B exits. Tab moves between fields/buttons;
Enter advances through the fields and connects after the password field.

Another PAX app can prefill the address with
`app_open("desktop", "computer.local:5900", 1)`. This prefills the connection
screen; it neither auto-connects nor accepts a password argument.

VNC traffic in this version is **unencrypted**. Use a trusted LAN or a VPN.

## Supported features and limits

- RFB 3.3, 3.7 and 3.8; shared sessions; no-auth and classic VNC DES authentication.
- 16-bit little-endian RGB565; Hextile, CopyRect and Raw updates; DesktopSize.
- Aspect-preserving scaling and a local pointer; incremental update requests.
- Remote desktop storage: two bytes per pixel in PSRAM, up to 8 megapixels,
  with neither dimension exceeding 4096. Allocation failures return an error.
- Partial reads/writes, connection/read/write timeouts, bounded metadata,
  rectangle validation, cancellation and session cleanup.

SPICE, audio, clipboard sharing, USB redirection, TLS/VeNCrypt, Tight and ZRLE are
not implemented. The server is asked to use supported encodings. The original
YAML contains SPICE/VNC UI calls, but the referenced upstream component source
available during this work did not contain that backend; no implementation was
copied from it. The wire protocol follows
[RFC 6143](https://www.rfc-editor.org/rfc/rfc6143).

## Build

From the PAX workspace:

```sh
./pax-recipe/build-modules.sh desktop
```

This produces `pax-dev/apps/desktop.pax` and its listing JSON. The app uses the
existing firmware's network, display, input, memory and file services. No firmware
changes are required. It stays a development build (`publish: false`) until
hardware validation is complete.

## Tests

From `pax-modules`:

```sh
python3 -m unittest discover -s apps/desktop/tests -v

cc -std=c11 -g -O1 -fno-builtin -fno-tree-loop-distribute-patterns \
  -fsanitize=address,undefined -Wall -Wextra -Werror \
  -I ../pax-esphome/components/pax_loader \
  apps/desktop/tests/native_smoke.c apps/desktop/rfb.c \
  apps/desktop/vnc_auth.c -o /tmp/pax-desktop-native
mkdir -p /tmp/pax-desktop-captures
/tmp/pax-desktop-native /tmp/pax-desktop-captures 1024 600
/tmp/pax-desktop-native /tmp/pax-desktop-captures 800 480

python3 apps/desktop/tests/test_emulated.py \
  --workspace .. --pax ../pax-dev/apps/desktop.pax
```

The emulator test needs the existing `pax-tools/python-desktop` runtime and
permission to open a loopback socket. The native test writes PPM captures and
checks the full app with partial TCP transfers and temporary would-block results.
Some sandbox/ptrace environments require `ASAN_OPTIONS=detect_leaks=0`; explicit
allocation/free checks still run in the protocol suite and native cleanup check.

Validated on 2026-10-16: 12 protocol tests, full native-app smoke tests at 800x480
and 1024x600 under AddressSanitizer/UndefinedBehaviorSanitizer, and the compiled
RISC-V `.pax` in the existing PAX emulator over loopback TCP (6 frames, 971 receive
calls, keyboard and pointer packets verified). No physical ESP32-P4 or independent
third-party VNC server has been tested yet.
