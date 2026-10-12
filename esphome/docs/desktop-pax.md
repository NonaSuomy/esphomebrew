# Desktop PAX apps and ABI services

There are now two app test paths, in addition to the [ESPHome three-button SDL preview](desktop-sdl.md). Both render through SDL2 and use the current loader service header without changing the device's ABI-1 wire format.

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

Local `.pax` app hand-off works in either mode. The child executes in the emulator. HTTP app hand-off and store installation are not implemented; first place the desired binary in the desktop SD folder. App return is limited to two waiting callers, matching the loader's return-stack limit.

## Controls

Mouse clicks act as touchscreen input. Arrow keys and Enter are delivered as the launcher's USB keyboard events. WASD supplies gamepad directions, J/K supplies A/B, U/I supplies X/Y, Q/E supplies L/R, Tab supplies Select and Enter supplies Start. Escape or window close requests exit through L3 and X. Wheel input is also available.

The host supports RGB565 framebuffer access, dirty rectangles, software scaling, touch, keyboard/gamepad input, memory allocation, time/delay, local file operations and directory/stat services, launcher status, local app launch/return, DNS, TCP and HTTPS. TLS uses the computer's trusted certificates and verifies the requested hostname. Network listings and JSON sidecars use the app's existing HTTP parser through these services. Connect/handshake currently blocks the single desktop service thread for up to five seconds.

Desktop brightness, volume and auto-brightness values are session simulations. The launcher can save its own theme, list and other file-backed preferences. Device Wi-Fi provisioning, IP configuration changes and SD/USB mounting controls do not configure the computer.

## Limits and checks

This is experimental app development support. Tested: the native launcher, the unchanged RISC-V launcher, its SD and local network catalog/JSON display, Settings touch and keyboard input, and unchanged `touchtest.pax` including launch/return. It does not establish compatibility with every module, vendor-specific CPU instruction or engine.

Audio, FreeRTOS task creation, MIDI, PNG decoding, UDP multiplayer, PPA acceleration, ZIP extraction and verified store/download installation are not implemented here. Optional unsupported services remain NULL; PNG decode reports no image and procedural UI sound is silent. The run report lists unavailable services. Apps that require them need additional desktop implementations before they can run. The device loader remains unchanged.

```sh
pax-tools/python-desktop/bin/python pax-tools/desktop/test_services.py
pax-tools/python-desktop/bin/python pax-tools/desktop/test_launchers.py
pax-tools/python-desktop/bin/python pax-tools/desktop/test_handoff.py
./pax-tools/run-desktop.sh emulated --headless --seconds 5 --report /tmp/pax-run.json --screenshot /tmp/pax-screen.ppm
```

The contract tests use temporary storage and a loopback server. Launcher tests check actual frame colors, local files, TCP catalog reads, JSON friendly names and injected touch/key events. The hand-off test starts the unchanged touch test and verifies the launcher returns. `--input-script` accepts a JSON array of timed touch/key events for repeatable tests. Screenshots are RGB PPM images; the final exit-clear frame is excluded from the captured app view.

Dependencies: Linux, Python 3, the pinned `unicorn==2.1.4` package in an isolated environment, SDL2 runtime, Git, a C compiler and system development headers for native builds. No RISC-V compiler is required to run an existing `.pax`. See the [Unicorn project](https://www.unicorn-engine.org/) for its CPU-emulation backend. Desktop performance and timing do not predict device performance.
