# BLE Remote

Build with `pax-recipe/build-ble-remote.sh`. Stage `ble_remote.pax` in `/sd/roms/pax/` and launch it from the PAX library.

The NANO must run `esp32-p4-nano-lcd43-ble-remote.yaml` (first run `pax-tools/prepare-ble-keyboard-test.py`). This reuses the tested ESP-Hosted C6 composite HID backend and retains its BLE identity/bonds. The ordinary NANO and AIO receiver firmware profiles are unchanged. Pair the advertised **PAX NANO BLE Test** from the receiving computer/AIO. The app's Advertise button enables advertising on the active host slot; closing the app releases all controls and returns to the launcher without deleting the bond.

Keyboard is a US-layout keyboard; modifiers latch for the next tap. Mouse Drag toggles a held left button so one finger can drag. Gamepad combines simultaneous fingers into 16 button bits and an eight-way D-pad (opposite directions cancel). It is standard BLE HID, with no analog axes or XInput emulation. The app uses an 800x480 RGB565 canvas (768000 bytes) plus a 256 KiB LVGL pool. All LVGL and BLE calls execute through the loader's main-loop UI bridge. Input releases on tab change, disconnection, normal exit and forced loader close.

The app uses the append-only optional `ble_remote` service. Older loaders or firmware without `ble_remote_id` reject the app with a log explaining the requirement. The ABI stays version 1; existing offsets do not move. The backend is generated from pinned upstream sources by the preparation tool and remains GPL-3.0; this app and bridge are Apache-2.0 and bundled LVGL is MIT. No new radio stack is included in the app.

## Validation

`python pax-modules/tools/test_ble_remote.py` builds the real LVGL UI for the host and exercises touch coordinates, one-shot modifiers, click/drag/motion/scroll, simultaneous D-pad plus action buttons, diagonal hats, disconnect cleanup and closing. Preview frames are written to `/tmp/pax-ble-remote/`.

Compile `pax-esphome/tests/ble_remote_bridge.cpp` with C++17, `-DPAX_APP_SIDE=1` and the loader include directory to test optional backend detection, report bounds, disconnection and release behavior. Firmware commands are `pax-tools/build-firmware.sh config nano-lcd43-ble-remote` and `pax-tools/build-firmware.sh compile nano-lcd43-ble-remote`. Building does not flash a device.

For an end-to-end check after flashing, run `python pax-tools/ble-remote-app-test.py --nano <address> --aio <address>`. It verifies the staged app and metadata on the NANO SD card, launches the existing AIO diagnostic receiver, drives the real app through the optional firmware's timed touch harness, verifies keyboard/mouse/gamepad reports and releases, then returns the AIO to its launcher and leaves BLE Remote running on the NANO. Timed swipe totals allow a few pixels for touch sampling; deterministic host motion tests remain exact. Left/right clicks remain pressed for at least 60 ms so polling receivers can observe them.

## Saved hosts

The optional NANO BLE Remote profile enables ten saved host slots with one active connection. Open **Hosts**, select a numbered slot, and tap **Connect selected**. **Pair new device** chooses the first empty slot and advertises **PAX NANO BLE Test**, keeping other pairings. Scan from the receiving device's Bluetooth settings. The old slot's address and pairing remain saved; selecting that slot reconnects it.

**Disconnect** pauses advertising on the active slot and retains its pairing. **Connect selected** re-enables it. **Forget selected** requires a second tap within six seconds and pauses that slot so its previous host cannot immediately reclaim it. Also forget the NANO in that host's Bluetooth settings before pairing again. If all ten slots are occupied, Pair new refuses to replace any pairing. Slot state and the active slot survive firmware restarts. The app needs the corresponding firmware update for these commands; older service numbers and ABI offsets are unchanged.

Keyboard/mouse/gamepad reports are verified against the AIO receiver. Android pairing and controls were confirmed by the user on 2026-10-26; iOS/Windows/macOS still require platform tests. The existing AIO forced-reconnect limitation is not resolved by host selection controls.

Bluetooth startup is delayed until hosted networking is initialized. Until then, host mutations return `-6` and the app shows a starting message, preserving the saved NVS slots.

The firmware build hook backports Espressif's [TinyCrypt ECC fix](https://github.com/espressif/esp-idf/commit/9fd7cb7e606) to ESP-IDF 6.1.0. Its original P4 accelerator path can truncate the regularized ECDH scalar and fail LE Secure Connections with a DHKey mismatch. The backport retains secure pairing and is checked against independent P-256/P-192 ECDH results with and without blinding; the original SDK fails the regression. Run `python -m unittest discover -s pax-esphome/tests -p test_tinycrypt_ecc.py`.

Hardware validation on 2026-10-26: Linux paired and bonded successfully, resolved all HID services, reconnected after switching slots, and received keyboard Q press/release, relative mouse motion, left-button press/release, and gamepad A/D-pad with neutral releases. The two-tap Forget control was verified on that temporary pairing, then it was removed from both devices. The AIO pairing in slot 1 was preserved and the user confirmed Android pairing and controls working on slot 2. This does not certify iOS/macOS/Windows or resolve the AIO receiver's separate reconnect limitation.

## Host names and addresses

Select a saved host in **Hosts**, then tap **Edit name**. Enter a label such as `iPhone SE`, `Android` or `AIO` and tap **Save name**. The local editor does not send Bluetooth keystrokes. Each label is stored with its host slot in the NANO's NVS, survives reboots and app updates, and is removed when the pairing is forgotten. Labels accept up to 31 UTF-8 bytes; an empty label restores the unnamed display. Save failures keep the editor open.

Slot cards show the name and connection state separately. The selected host also shows its Bluetooth address, using the peer's stable identity when available. These are user labels: the current peripheral backend does not read a central device's GAP Device Name. A Bluetooth address does not reliably identify its model or OS, particularly when privacy addresses are used.

The optional test firmware reserves Host 4 for an authenticated Secure Connections iOS compatibility test. Its PIN comes from the ignored `ble_ios_test_passkey` secret; other slots retain no-PIN pairing. iPhone SE on iOS 15.8.8 still requires a successful typing test before being described as supported.
