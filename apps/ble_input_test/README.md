# BLE Input Test

A diagnostic PAX app that runs on a BLE-enabled receiver and looks for the
`PAX NANO BLE Test` peripheral. It displays connection status and observed
keyboard, relative mouse and gamepad events, and saves a JSON report to
`/sd/roms/pax/ble_input_test.json`.

This is a test tool for compatible PAX BLE receiver firmware. It does not
include firmware, radio images or a general BLE pairing interface. Close the
app with the device remote when finished.
