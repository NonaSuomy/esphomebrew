# Single-device development test queue

The ESP32-P4 can only have one destructive test session at a time. Build,
flash, PAX launch, and serial capture should be wrapped in one queue lease so
parallel chat sessions do not steal `/dev/ttyUSB0` or reset each other's app.

Check the device:

```sh
python3 tools/device_test_queue.py status --device 10.20.30.180
```

While a lease is active, `status` reports the owner, command, start time,
heartbeat, and most recent output line. Other chats can use this to see whether
the owner is compiling, linking, launching, or collecting logs:

```sh
watch -n 2 'python3 tools/device_test_queue.py status --device 10.20.30.180'
```

Run a complete test. If another session owns the device, this waits until its
command exits:

```sh
python3 tools/device_test_queue.py run \
  --device 10.20.30.180 --owner quake2 -- \
  ./tools/my_build_flash_launch_and_capture.sh
```

Use a distinct owner such as `quake2`, `stargus`, or `redalert`. The lease is
held by the wrapper process and is automatically released when the command
finishes, fails, or is interrupted. Set `ESP32_TEST_QUEUE_DIR` to move the
queue metadata when `/tmp` is not shared between the workers.

Keep LAN logging first and serial capture as backup inside the same lease. A
serial reset is also safe as a last-resort recovery action, but only from the
leased command:

```sh
python3 tools/device_test_queue.py run \
  --device 10.20.30.180 --owner quake2 -- \
  bash -lc 'python3 tools/lan_launch_and_logs.py 10.20.30.180 || \
            python3 tools/serial_launch_and_logs.py /dev/ttyUSB0 || \
            esptool.py --chip esp32p4 --port /dev/ttyUSB0 run'
```
