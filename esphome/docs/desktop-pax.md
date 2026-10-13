# Desktop PAX apps and ABI services

There are now two app test paths, alongside the [ESPHome three-button SDL launcher](desktop-sdl.md). Both render through SDL2 and use the current loader service header without changing the device's ABI-1 wire format.

| Mode | App code | Purpose |
| --- | --- | --- |
| `native` | Existing `pax-modules/apps/launcher_ui` sources compiled as a host shared library | Fast launcher development with normal desktop tooling |
| `emulated` | Existing ESP32-P4 `.pax` binary, unchanged | Exercise actual RV32 machine code and its four-byte ABI service pointers |

The native library uses the computer's pointer sizes and is not an installable `.pax`. The emulator uses Unicorn's RISC-V backend, maps the original flat image at `0x4A000000`, zeroes BSS, creates the ABI-1 service table and bridges callback calls into the same desktop services. It enables the floating-point state required by the P4's ILP32F code. It does not emulate an entire ESP32-P4 board or its peripherals.

## Start

From `~/code/pax`:

```sh
./pax-tools/setup-desktop.sh
./pax-tools/run-desktop.sh build
./pax-tools/run-desktop.sh native
./pax-tools/run-desktop.sh emulated
```

`native` builds the launcher automatically when its library is missing. Run `build` again after changing launcher sources. The builder uses the same pinned LVGL 9.5 sources as the device app, fetching that commit when the local cache is missing. If the loader service header changes, the host refuses a stale native library and asks for a rebuild. The generated library and metadata live in `pax-dev/desktop/launcher_ui/`. Replaced libraries are preserved under `backups/desktop/launcher_ui/`.

The emulator defaults to `pax-dev/launcher_ui.pax`. Select another existing binary:

```sh
./pax-tools/run-desktop.sh emulated --pax pax-dev/apps/touchtest.pax
```

No firmware flash or physical device is needed. Close the SDL window or press Escape to request app exit. A locally launched child app returns to its caller when requested by `app_open`.

## Desktop SD files

The default SD directory is `pax-dev/desktop/sd`, which is excluded from Git. App paths under `/sd/` map into that directory. The launcher searches `roms/pax/` there. Copy the apps and their JSON sidecars you want to test:

```sh
mkdir -p pax-dev/desktop/sd/roms/pax
cp pax-dev/apps/touchtest.pax pax-dev/apps/touchtest.json pax-dev/desktop/sd/roms/pax/
```

Use `--sd-root /path/to/test-sd` for a different directory. Preferences written by the launcher remain in that test directory. USB storage is unavailable. File management rejects paths outside the chosen SD root, traversal components and symlinks that resolve outside it. Native app execution itself is a normal desktop process, not a security sandbox.

Local `.pax` app hand-off works in either mode. The child executes in the emulator. HTTP/HTTPS app-to-app hand-off now downloads the selected `.pax`, validates its ABI header and any JSON size/SHA-256, then starts it in the emulator. Downloads are cached under `pax-dev/desktop/downloads/` using the actual app filename. A child execution failure returns to its waiting caller and logs the failure. Verified store installation and automatic game-data installation remain unavailable; place required WADs, ROMs and other data in the desktop SD folder. The SDL three-button screen uses the same downloader for the launcher. App return is limited to two waiting callers, matching the loader's return-stack limit.

## Controls

The app window starts at 1024x600. Each selected canvas sets SDL's logical rendering size explicitly, preserving aspect ratio if the compositor or window size differs. SDL transforms mouse clicks into canvas coordinates, including high-DPI output and resized windows. Mouse clicks act as touchscreen input. Arrow keys and Enter are delivered as the launcher's USB keyboard events. WASD supplies gamepad directions, J/K supplies A/B, U/I supplies X/Y, Q/E supplies L/R, Tab supplies Select and F1 supplies Start. Escape or window close requests exit through L3 and X. Wheel input is also available.

The host supports RGB565 framebuffer access, dirty rectangles, software scaling, touch, keyboard/gamepad input, memory allocation, time/delay, local file operations and directory/stat services, launcher status, local app launch/return, DNS, TCP and HTTPS. TLS uses the computer's trusted certificates and verifies the requested hostname. Network listings and JSON sidecars use the app's existing HTTP parser through these services. Connect/handshake currently blocks the single desktop service thread for up to five seconds.

Desktop brightness, volume and auto-brightness values are session simulations. The launcher can save its own theme, list and other file-backed preferences. The network icon and IPv4/DHCP fields now read the computer's current default interface through Linux `ip` (iproute2). Network configuration is read-only: DHCP and IP changes in the launcher do not configure Linux. Device Wi-Fi provisioning and SD/USB mounting controls also do not configure the computer.

## Limits and checks

This is experimental app development support. Tested: the native launcher, the unchanged RISC-V launcher, its SD and local network catalog/JSON display, Settings touch and keyboard input, unchanged `touchtest.pax` including launch/return, and unchanged Doom PAX boot, menu input, movement, audio generation and network launch/return. It does not establish compatibility with every module, vendor-specific CPU instruction or engine.

Stereo S16 audio is queued to SDL. The RV32 emulator now supports guest task creation, independent CPU/FP registers and stacks, delay scheduling and task deletion. Tasks share the app image and receive instruction-budget time slices on one emulated CPU; hardware core affinity and FreeRTOS priority scheduling are not reproduced. Native task creation remains unavailable. MIDI, PNG decoding, UDP multiplayer, PPA acceleration, ZIP extraction progress and verified store/download installation are not implemented here. Optional unsupported services remain NULL; PNG decode reports no image and procedural UI sound is silent. The run report lists unavailable services. Apps that require them need additional desktop implementations before they can run. The device loader remains unchanged.

```sh
pax-tools/python-desktop/bin/python pax-tools/desktop/test_services.py
pax-tools/python-desktop/bin/python pax-tools/desktop/test_window_geometry.py
pax-tools/python-desktop/bin/python pax-tools/desktop/test_launchers.py
pax-tools/python-desktop/bin/python pax-tools/desktop/test_handoff.py
pax-tools/python-desktop/bin/python pax-tools/desktop/test_handoff.py --keyboard
pax-tools/python-desktop/bin/python pax-tools/desktop/test_handoff.py --network --keyboard
pax-tools/python-desktop/bin/python pax-tools/desktop/test_handoff.py --network --keyboard --bad-child
./pax-tools/run-desktop.sh emulated --headless --seconds 5 --report /tmp/pax-run.json --screenshot /tmp/pax-screen.ppm
```

The contract tests use temporary storage and a loopback server. Launcher tests check actual frame colors, local files, TCP catalog reads, JSON friendly names and injected touch/key events. The hand-off test starts the unchanged touch test and verifies the launcher returns. `--input-script` accepts a JSON array of timed touch/key events for repeatable tests. The `display_geometry` run-report field records the canvas, logical, window and renderer output dimensions to diagnose desktop scaling. The SDL geometry test verifies real click events at opposite corners after resizing. Screenshots are RGB PPM images; the final exit-clear frame is excluded from the captured app view.

Dependencies: Linux, Python 3, the pinned `unicorn==2.1.4` package in an isolated environment, SDL2 runtime, Git, a C compiler and system development headers for native builds. No RISC-V compiler is required to run an existing `.pax`. See the [Unicorn project](https://www.unicorn-engine.org/) for its CPU-emulation backend. Desktop performance and timing do not predict device performance.

## Doom

Doom now runs as an unchanged RISC-V PAX, including its watchdog and sound mixer tasks. The game was tested through SDL video and audio backends, New Game/difficulty selection, movement, and HTTP launcher hand-off followed by clean return. A 25-second gameplay check rendered 318 frames, about 12-13 frames/s; this remains slower than native Doom. Audio generation and queueing were checked with SDL's dummy audio backend, not by listening to physical speakers.

```sh
cd ~/code/pax
pax-tools/python-desktop/bin/python pax-tools/desktop/setup_doom.py
./pax-tools/run-desktop.sh emulated --pax pax-dev/apps/doom.pax
```

The setup helper uses cached or pinned public free shareware files, verifies the manifest size/SHA-256, and preserves existing files. It prepares `doom1.wad` and `prboom.wad` in `pax-dev/desktop/sd/roms/doom/`. This is `/sd/roms/doom/` inside the app. These data files and runtime preferences are excluded from Git. Use `--sd-root` with both the setup helper and runner to choose another storage directory. Your own `doom.wad` or `doom2.wad` can also be placed there.

You can instead restart the three-button SDL screen and select Doom from Storage or Network. Both use the same desktop SD folder by default. The older Doom binary's directory stat implementation causes its save-directory check to fall back to `/sd/roms/doom/`, so configuration/save files may be written beside the WADs.

Controls: J accepts Doom menus and fires; WASD moves/turns, Q/E strafes, K runs/backs out, I opens the map, M opens the game menu, F1 uses/opens, and Tab changes weapons. Escape or window close exits the app. Keyboard Enter remains reserved for the launcher's selection behavior; use J in Doom.

```sh
pax-tools/python-desktop/bin/python pax-tools/desktop/test_guest_tasks.py
pax-tools/python-desktop/bin/python pax-tools/desktop/test_doom.py
pax-tools/python-desktop/bin/python pax-tools/desktop/test_doom.py --handoff
```

The Doom checks use temporary SD directories and verify actual guest tasks, colored frames, non-silent PCM blocks queued to an SDL audio device, deletion of both tasks, and optional URL launch/return. Software RGB565 scaling uses SDL's compiled blitter instead of per-pixel Python loops.

## NES and ZIP ROMs

Place NES `.nes` or `.zip` ROMs in `pax-dev/desktop/sd/roms/nes/`, then open NES and select a game. The desktop host implements first-match and named ZIP extraction for stored and deflated members. Nested paths and uppercase extensions work; matching ROMs are extracted into `/sd/roms/pax/.zipcache/`. Encrypted archives and members larger than 128 MiB are rejected. Progress extraction remains unavailable.

NES 0.6.1 accepts ZIP paths from ROM settings and `nes.rom` sidecars. The launcher keeps Enter/A from activating the app list underneath its ROM picker. ROM settings survive app hand-off and are saved under the selected SD root in `.pax-desktop-rom-path`.

```sh
pax-tools/python-desktop/bin/python pax-tools/desktop/test_zip_rom.py --launcher
```

The check uses an original generated mapper-0 ROM and exercises ZIP selection, extraction, NES rendering and return to the launcher. It does not establish compatibility with every mapper or game.

## Browser builds

The browser Web Lab offers both source-compiled launcher/NetSurf previews and original ABI-1 RISC-V `.pax` execution through Unicorn WebAssembly. See `pax-tools/web/README.md` for build instructions. Choose **Start PAX emulator** for original binaries and verified GitHub store installation; **Source launcher** retains the source preview. Browser SD storage uses IndexedDB, and browser HTTP access follows CORS and HTTPS restrictions. Audio requires **Enable audio**. Browser compatibility and performance vary between apps.
