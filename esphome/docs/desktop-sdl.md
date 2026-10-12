# PAX desktop SDL2 preview

`host-pax-sdl.yaml` runs the current three-button ESPHome screen on a computer. It uses the same `packages/pax-three-button-ui.yaml`, LVGL9.5 fork, colors, button positions and focus behavior as the device profiles. SDL mouse input acts as touch and SDL keys replace the device's keyboard/gamepad sensors. The old `esphome006/config/host_elecrow_aio.yaml` was used as a reference.

## Start from the PAX workspace

```sh
cd ~/code/pax
./pax-tools/build-firmware.sh run desktop
```

The first run compiles a native program and opens a1024x600 SDL window. Close the window or press Ctrl-C in the terminal to stop. No serial port, ESP32 device or device Wi-Fi credentials are required.

To validate or build without starting the window:

```sh
./pax-tools/build-firmware.sh config desktop
./pax-tools/build-firmware.sh compile desktop
```

After `compile`, the program is staged at `pax-dev/desktop/pax-desktop/program`. It can be started directly from the terminal. Native binaries are local build outputs and are excluded from Git; share the YAML/source instead. Superseded native builds are preserved under `backups/desktop/` when staging replaces them.

## Controls and test scope

Click STORAGE, NETWORK or GITHUB STORE with the mouse. Tab and arrow keys change focus; Enter or Space selects. WASD and F/Escape provide the shared gamepad-style navigation bindings. The footer and console show the selected path/URL.

This is a preview of the ESPHome three-button screen. `pax_desktop` reports each shared launch callback. It does not execute or download `launcher_ui.pax`, games or emulator binaries: those contain ESP32-P4 RISC-V machine code. It does not emulate SD hardware, USB host, OTA, the MIPI/HDMI bridge or the app's native Settings/Store screens. A full app test on a computer needs a separate native port or RISC-V emulator and the PAX service layer.

The desktop profile is registered as `desktop` for build commands. It is not a board flash target and is excluded from the public serial installer registry. Existing board configurations retain the same resolved settings.

## Dependencies

On this computer SDL2 is already installed. The YAML uses `sdl2-config` to discover its compiler/linker flags. A new Linux computer needs a C++ compiler, SDL2 development headers, the normal PAX Python/PlatformIO setup and a desktop display session. For Debian/Ubuntu, the SDL dependency is `libsdl2-dev`; Fedora uses `SDL2-devel`. An ESP32 firmware toolchain is not required to execute the compiled host program.

## Headless input check

After compilation:

```sh
python3 pax-esphome/tests/check_desktop_sdl.py
```

The test uses SDL's dummy video driver and injected SDL events, with a temporary preferences directory. It checks mouse selection of all three choices and keyboard Right+Enter selection of Network. The program's `PAX_SDL_SMOKE_TEST` environment flag is used only by this check; normal desktop runs do not inject input or stop automatically.

LVGL's ESP32 memory allocation stays unchanged. The local fork now guards its ESP-IDF heap header, uses its host allocator for host rotation buffers, and selects the no-OS LVGL backend for host builds instead of FreeRTOS. The desktop adapter is host-only, so it is never included in device firmware.

## Actual launcher and PAX app tests

The separate [desktop PAX host](desktop-pax.md) now provides native launcher builds and execution of existing RISC-V `.pax` binaries through a service bridge. Use `./pax-tools/run-desktop.sh native` or `emulated` after desktop setup. The ESPHome YAML preview above continues to test the shared three-button firmware screen.
