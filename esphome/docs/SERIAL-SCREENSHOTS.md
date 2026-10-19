# Screenshots without Wi-Fi

With the updated P4 firmware installed, connect its USB-UART port and run:

```sh
python3 /home/nonasuomy/code/pax/pax-recipe/tools/record_screen.py \
  --serial /dev/ttyUSB0 --screenshot /tmp/pax-media/desktop-003.png
```

No IP address or network connection is needed. Close serial monitors first; the tool opens the port exclusively and leaves the auto-program lines released. On Linux, both lines are released together to avoid a reset pulse while opening the USB-UART. The client allows up to 60 seconds for the serial service to become ready during startup. The baud rate defaults to 115200 and can be selected with `--baud` when the firmware logger uses another rate. Pyserial and Pillow are required in the Python environment running the command.

The image uses the same logical canvas and orientation as network screenshots, including the shared game menu. With no app running, builds with LVGL snapshot support can capture the firmware menu. Transfer progress is printed about every five seconds. A full 1024×600 RGB565 image takes at least two minutes at 115200 baud; allow additional time for request/response overhead. Serial capture supports still screenshots. Video/audio recording continues to use the network interface.

An immutable snapshot is captured by a worker, then fetched in chunks through the existing Improv vendor channel. Normal serial logs are ignored, packets are checksummed, and the entire RGB565 image is verified against SHA-256 before writing a PNG. Offset-based reads can be retried without recapturing a different frame. Requests carry a session ID so delayed replies from an older capture cannot be used. The snapshot is freed on release or after two minutes without requests. An old firmware responds with a clear update-required error.

Protocol: vendor command `0x74` with empty payload retains the `PAXSD1` ping. Nonempty payload is `op:u8, session:u32le`, plus `offset:u32le` for operation 3. Operations are 1 capture, 2 metadata/status, 3 chunk read, 4 release. Responses are `0x74, status, op, session:u32le`; status 0 means success, 2 means capturing, and 1 is followed by an error string instead of op/session. Metadata adds width/height as u16le, size as u32le, then SHA-256 as 32 raw bytes. Chunks add offset:u32le and up to 240 pixel bytes. Transfer uses no SD file and does not change the running app.

Validation: Elecrow P4 AIO hardware captured a 1024×600 menu image entirely over `/dev/ttyUSB0` at 115200 baud and passed SHA-256 verification. The Linux simultaneous-line release probe obtained a fresh snapshot in 0.40 seconds with no ROM boot marker observed. Host tests cover framing, interleaved logs, corrupt packets, full-image corruption, legacy firmware, timeouts, CLI network bypass, and modem-line setup. The production firmware handler is also tested for idempotent starts, busy/stale sessions, offsets, maximum response size, allocation-task failure, and release.
