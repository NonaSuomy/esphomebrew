# Controller remapping

Launcher UI 0.6.7 adds Settings → Controllers → Set up controller. Release all
buttons, then press each prompted direction/button. Skip leaves that action
unbound. Defaults restores the identity map; Save applies it after NVS persistence
succeeds. Cancel does not modify the active profile.

This is one shared profile of normalized controller inputs, not per-VID/PID or
per-Bluetooth-address mappings. It covers the D-pad/left-stick digital directions,
A/B/X/Y/L/R, Select and Start. Existing USB/Bluetooth report parsers determine which
normalized inputs a physical device exposes; this does not make unsupported HID
reports supported. Right-stick pointer handling, keyboard/mouse input, board
buttons, remote tool injection, and the dedicated menu/close controls bypass it.

The host provides `/dev/pax-controller-map` through the existing file service;
no ABI-1 function offsets change. Reads return a raw controller snapshot, active
map and persistence status. A complete validated write queues an update. Only the
ESPHome loop writes preferences; the app's UI callbacks do no flash or SD IO.
Bindings are unique, in range or explicitly unbound. MENU and VOLUME bits always
pass through. Missing protocol support on older firmware shows an update message.

Validation includes native profile/virtual-stream tests, persistence-failure
checks, real LVGL wizard and touch geometry tests, app compilation and matching
Elecrow host firmware compilation. Physical controller testing is still needed.
