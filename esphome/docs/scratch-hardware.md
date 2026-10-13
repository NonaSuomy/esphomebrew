# Scratch hardware blocks on the Elecrow ESP32-P4 AIO

Scratch 0.3 adds a **Board IO** category. The native editor and player need the updated PAX loader firmware for sensor/output functions. Editor, normal Scratch scripts, and SB3 saving still work on older loaders; hardware reports `unavailable` and write status `-1` there.

Use **sensor [channel]**, **[channel] available?**, and **set [channel] to [value]**. Tap a channel socket to choose from the firmware's configured channels; the write picker lists outputs. A keyboard option permits entering names. Reporters connect to ordinary Scratch arithmetic, comparisons, conditions and variables. Dedicated distance, temperature, humidity, light, IMU, button, relay, servo, RGBW LED, microphone and camera blocks provide shortcuts.

**last IO status** reports the last read or queued write: `0` success/accepted, `-1` unknown/read-only/no bridge, `-2` missing sample or invalid/out-of-range value, `-3` output queue full. Output acceptance is not confirmation of physical movement or a completed camera capture. Check `camera_capture_status` for the optional camera result.

## Configured onboard channels

The active Elecrow radio profiles include `boards/elecrow/scratch-io.yaml`. Sensor units and output ranges are explicit:

| Component | Channels | Units / values |
| --- | --- | --- |
| HC-SR04 | `distance_cm` | cm; GPIO13 trigger / GPIO12 echo |
| BH1750 | `light_lux` | lux; I2C 0x5C |
| DHT20 / AHT20 | `temperature_c`, `humidity_pct` | C, %; I2C 0x38 |
| LSM6DS3TR-C | `accel_x`, `accel_y`, `accel_z` | g; I2C 0x6B |
| LSM6DS3TR-C | `gyro_x`, `gyro_y`, `gyro_z` | degrees/s |
| PIR | `motion` | 0/1; GPIO24 |
| ADC buttons | `buttons_voltage` | V; GPIO16; dedicated button blocks use the existing calibrated D-pad mapping |
| MQ2 | `gas_voltage` | V; GPIO17; raw voltage, not a calibrated gas concentration |
| Microphone | `microphone_enabled`, `microphone_level` | enable 0/1; PCM16 RMS 0..100% of full scale, not calibrated dB SPL; GPIO3/4 PDM |
| Relay | `relay` | output 0/1; GPIO42 |
| Servo | `servo` | output -100..100%; GPIO25 |
| WS2814 RGBW LED | `led_red`, `led_green`, `led_blue`, `led_white`, `led_brightness` | outputs 0..100%; GPIO8 |
| Display / touch | `display_brightness`; normal Scratch mouse reporters | brightness output 5..100%; full 1024x600 touch coordinates |
| Speakers | `speaker_enabled`; normal Scratch Sound blocks | enable 0/1; existing stereo I2S sound playback |

The active legacy ESPHome component snapshot also needs the P4 PDM validation patch in `patches/0001-p4-pdm-validation.patch` (apply in `dependencies/esphome`); this workspace already has it. The driver uses its existing PDM RX implementation.

The microphone starts only when its enable block runs. A missing/disabled microphone reading remains unavailable. Servo values are normalized: on V1.0 they control continuous rotation around neutral; on V1.1 they control position around center. Device behavior has not been validated by these compiler/host checks.

## Hall and touch-pad pin conflicts

The radio uses GPIO2 for reset and GPIO7 for SPI MOSI. These are also the board's TTP223 touch-pad and Hall pins. They cannot be used as separate GPIO sensors while driving that radio bus.

`esp32-p4-elecrow-pax-scratch-local.yaml` removes the hosted radio/Wi-Fi configuration, uses IP101 Ethernet, and includes `scratch-local-inputs.yaml` to expose `touch_pad` and active-low `hall` (0/1). Use this variant with the conflicting wireless module disconnected. GPIO24/25 are used for PIR/servo with UART0 logging; USB-Serial-JTAG on those pins is incompatible.

## Experimental SC2336 camera

`esp32-p4-elecrow-pax-scratch-camera.yaml` adds the board-documentation camera driver, pinned to `youkorr/test2_esp_video_esphome` commit `8c354e3c86ec9e5bb13c5836c34721d04bc686fb`. Its separate I2C bus is GPIO45/46, address 0x30, and clock GPIO30. The package selects JPEG snapshots and exposes:

- `camera_ready`: actual pipeline-ready state, 0/1.
- `camera_capture`: write 1 to capture a distinct `/sd/roms/scratch/camera-<timestamp>.jpg`.
- `camera_capture_status`: 1 saved, -1 failed; unavailable before the first capture.

The camera configuration is validated separately. Its external video stack remains experimental; it is not included in the normal firmware artifact and no camera hardware success is claimed. There is no live camera-as-stage-background block in this version.

## Other interfaces and extensions

SD stores projects, assets and recovery drafts. USB keyboard, mouse and gamepads feed the normal Scratch input blocks. Ethernet/Wi-Fi serve the existing PAX loader/network interfaces. Optional radio/LoRa/nRF modules and arbitrary UART/GPIO transactions are not automatically assigned pins by Scratch.

To add an ESPHome-backed module, add a named `pax_loader.hardware` entry with exactly one `sensor_id`, `binary_sensor_id` or `number_id`, plus an optional `unit`. A template number's `set_action` can drive another configured actuator. Numeric limits come from that number's traits. Writes are bounded and queued; actions execute on the ESPHome main loop, never on the Scratch task. Inputs are sampled into protected cached values. The four services are appended to ABI 1 without moving existing table slots.

## Reference and validation

The board inventory/pins come from the local Elecrow documentation supplied in this chat. The UI reference is `scratchfoundation/scratch-editor` at `2e28f0a8e7b7bd4b85ff3699c7ef9515636a4a04`, particularly `packages/scratch-gui/src/components/gui/gui.css` and `src/css/colors.css`. PAX implements the native contours, fonts, layout and block graphs; the browser React GUI is not bundled into the embedded runtime. ScratchEverywhere continues executing standard SB3 scripts.

Host tests execute exported sensor/output opcodes through the real interpreter with a fake loader, round-trip loader-backed ZIP archives, exercise touch editing and native canvas negotiation, and verify nested C-block/tab alignment. Firmware compiler results and any limitations are recorded in the workspace validation report. Flashing and device testing are separate steps.
