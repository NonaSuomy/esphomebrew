# PAX desktop SDL2 launcher

`host-pax-sdl.yaml` runs the current three-button ESPHome screen on a computer. It uses the same `packages/pax-three-button-ui.yaml`, LVGL9.5 fork, colors, button positions and focus behavior as the device profiles. SDL mouse input acts as touch and SDL keys replace the device's keyboard/gamepad sensors. The old `esphome006/config/host_elecrow_aio.yaml` was used as a reference.

## Start from the PAX workspace

```sh
cd ~/code/pax
./pax-tools/setup-desktop.sh  # first-time desktop ABI dependencies
./pax-tools/build-firmware.sh run desktop
```

The first run compiles a native program and opens a 1024x600 SDL window. Close the window or press Ctrl-C in the terminal to stop. No serial port, ESP32 device or device Wi-Fi credentials are required.

To validate or build without starting the window:

```sh
./pax-tools/build-firmware.sh config desktop
./pax-tools/build-firmware.sh compile desktop
```

After `compile`, the program is staged at `pax-dev/desktop/pax-desktop/program`. It can be started directly from the terminal. Native binaries are local build outputs and are excluded from Git; share the YAML/source instead. Superseded native builds are preserved under `backups/desktop/` when staging replaces them.

## Controls and test scope

Click STORAGE, NETWORK or GITHUB STORE with the mouse. Tab and arrow keys change focus; Enter or Space selects. WASD and F/Escape provide the shared gamepad-style navigation bindings. The footer and console show the selected path/URL.

The buttons now open the actual `launcher_ui.pax` through the [desktop PAX service host](desktop-pax.md). The three-button window hides while the app runs and returns when it closes. Storage uses `pax-dev/desktop/sd/roms/pax/launcher_ui.pax`, copying the local development launcher there on first use. It preserves an existing desktop SD copy. Network downloads the launcher from `http://10.13.37.84:8000/launcher_ui.pax` and opens its LAN catalog using the same server. GitHub downloads the launcher from the shared GitHub URL and opens its store view.

Downloads validate the ABI-1 header and any size/SHA-256 declared by the JSON sidecar. Cached binaries are under `pax-dev/desktop/downloads/`; changed cached binaries and metadata are preserved under `backups/desktop/downloads/`. Failures appear in the footer and terminal. This does not emulate SD hardware, USB host, OTA or the MIPI/HDMI bridge. Desktop app service limits are documented separately.

For a different server without editing or rebuilding the YAML:

```sh
PAX_DESKTOP_NETWORK_URL=http://YOUR_IP:8000/launcher_ui.pax ./pax-tools/build-firmware.sh run desktop
```

`PAX_DESKTOP_SD_ROOT=/path/to/test-sd` chooses another desktop SD directory.

The desktop profile is registered as `desktop` for build commands. It is not a board flash target and is excluded from the public serial installer registry. Existing board configurations retain the same resolved settings.

## Dependencies

On this computer SDL2 is already installed. The YAML uses `sdl2-config` to discover its compiler/linker flags. A new Linux computer needs a C++ compiler, SDL2 development headers, the normal PAX Python/PlatformIO setup and a desktop display session. For Debian/Ubuntu, the SDL dependency is `libsdl2-dev`; Fedora uses `SDL2-devel`. An ESP32 firmware toolchain is not required to execute the compiled host program.

## Headless input check

After compilation:

```sh
python3 pax-esphome/tests/check_desktop_sdl.py
```

The test uses SDL's dummy video driver and injected SDL events, with a temporary preferences directory. It checks mouse selection of all three choices and keyboard Right+Enter selection of Network. The program's `PAX_SDL_SMOKE_TEST` environment flag is used only by this check; normal desktop runs do not inject input or stop automatically. This selection-only check suppresses app launches. `pax-tools/python-desktop/bin/python pax-tools/desktop/test_button_launch.py` additionally checks an actual SDL Network click through download, emulator startup and catalog loading, Storage startup and SHA-256 rejection against a temporary loopback server.

LVGL's ESP32 memory allocation stays unchanged. The local fork now guards its ESP-IDF heap header, uses its host allocator for host rotation buffers, and selects the no-OS LVGL backend for host builds instead of FreeRTOS. The desktop adapter is host-only, so it is never included in device firmware.

## Actual launcher and PAX app tests

The separate [desktop PAX host](desktop-pax.md) now provides native launcher builds and execution of existing RISC-V `.pax` binaries through a service bridge. Use `./pax-tools/run-desktop.sh native` or `emulated` after desktop setup. The ESPHome SDL screen above now uses that host automatically for button selections.
