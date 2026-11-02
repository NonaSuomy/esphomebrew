# BLE Remote

BLE Remote provides an LVGL touch keyboard, mousepad, gamepad, and ten named BLE host slots. It requires PAX firmware with the optional BLE Remote service and a BLE-peripheral capable receiver profile; standard launcher firmware does not provide the required HID bridge.

The app does not include a Bluetooth stack. The firmware owns advertising, pairing, persistent host slots, and BLE HID reports. Closing BLE Remote releases held controls and returns to the launcher.

The keyboard uses LVGL's standard keyboard widget and US layout. Ctrl, Alt, and Win latch until the next key; uppercase and shifted symbols are one-shot. The mousepad supports pointer motion, clicks, drag, and scrolling. Multitouch combines gamepad buttons with an eight-way D-pad. Hosts can be selected, connected, disconnected, paired, renamed, or forgotten with confirmation.

The application and bridge are Apache-2.0, LVGL is MIT, and the firmware BLE backend is GPL-3.0. The package contains no radio stack or game data.
