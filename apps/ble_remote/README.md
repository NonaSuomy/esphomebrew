# BLE Remote

Build with `pax-recipe/build-ble-remote.sh`. Stage `ble_remote.pax` in `/sd/roms/pax/` and launch it from the PAX library.

The NANO must run `esp32-p4-nano-lcd43-ble-remote.yaml` (first run `pax-tools/prepare-ble-keyboard-test.py`). This reuses the tested ESP-Hosted C6 composite HID backend and retains its BLE identity/bonds. The ordinary NANO and AIO receiver firmware profiles are unchanged. Pair the advertised **PAX NANO BLE Test** from the receiving computer/AIO. The app's Advertise button enables advertising; closing the app releases all controls and returns to the launcher without deleting the bond.

Keyboard is a US-layout keyboard; modifiers latch for the next tap. Mouse Drag toggles a held left button so one finger can drag. Gamepad combines simultaneous fingers into 16 button bits and an eight-way D-pad (opposite directions cancel). It is standard BLE HID, with no analog axes or XInput emulation. The app uses an 800x480 RGB565 canvas (768000 bytes) plus a 256 KiB LVGL pool. All LVGL and BLE calls execute through the loader's main-loop UI bridge. Input releases on tab change, disconnection, normal exit and forced loader close.

The app uses the append-only optional `ble_remote` service. Older loaders or firmware without `ble_remote_id` reject the app with a log explaining the requirement. The ABI stays version 1; existing offsets do not move. The backend is generated from pinned upstream sources by the preparation tool and remains GPL-3.0; this app and bridge are Apache-2.0 and bundled LVGL is MIT. No new radio stack is included in the app.

## Validation

`python pax-modules/tools/test_ble_remote.py` builds the real LVGL UI for the host and exercises touch coordinates, one-shot modifiers, click/drag/motion/scroll, simultaneous D-pad plus action buttons, diagonal hats, disconnect cleanup and closing. Preview frames are written to `/tmp/pax-ble-remote/`.

Compile `pax-esphome/tests/ble_remote_bridge.cpp` with C++17, `-DPAX_APP_SIDE=1` and the loader include directory to test optional backend detection, report bounds, disconnection and release behavior. Firmware commands are `pax-tools/build-firmware.sh config nano-lcd43-ble-remote` and `pax-tools/build-firmware.sh compile nano-lcd43-ble-remote`. Building does not flash a device.

For an end-to-end check after flashing, run `python pax-tools/ble-remote-app-test.py --nano <address> --aio <address>`. It verifies the staged app and metadata on the NANO SD card, launches the existing AIO diagnostic receiver, drives the real app through the optional firmware's timed touch harness, verifies keyboard/mouse/gamepad reports and releases, then returns the AIO to its launcher and leaves BLE Remote running on the NANO. Timed swipe totals allow a few pixels for touch sampling; deterministic host motion tests remain exact. Left/right clicks remain pressed for at least 60 ms so polling receivers can observe them.
