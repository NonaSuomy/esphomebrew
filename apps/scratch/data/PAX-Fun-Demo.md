# PAX Fun Demo

Open **PAX-Fun-Demo.sb3** in the PAX Scratch player or builder. The original **PAX Demo.sb3** smoke test is retained unchanged. This demo adds live readings and editable native Scratch scripts using the Board IO extension; it requires the PAX player rather than the browser Scratch website for hardware opcodes.

Touch the purple buttons or use a USB keyboard. **Space / Next** cycles seven pages. Readings refresh twice per second; numeric sensor values are rounded to one or two decimal places. Missing channels or samples show **unavailable**, rather than becoming zero through arithmetic.

| Page | Examples |
| --- | --- |
| Environment | Temperature (C), humidity (%), light (lux), distance (cm), MQ2 raw voltage, PIR motion |
| Tilt and rotation | Three accelerometer axes (g), three gyroscope axes (degrees/second) |
| Buttons and touch | ADC voltage, D-pad, A, mouse/touch X and Y in Scratch stage coordinates |
| More controller inputs | B/X/Y, shoulders, Start/Select, mouse/touch press |
| Optional inputs | Hall, capacitive touch pad, microphone RMS/enable, camera ready/capture result |
| RGBW output state | Red, green, blue, white and brightness settings |
| Other output state | Relay, servo, LCD brightness, speaker enable and camera trigger |

| Button/key | Example |
| --- | --- |
| LED / 1 | Red, green, blue and white at 40%, 0.4 seconds each, then off |
| Relay / 2 | On for 0.5 seconds, then off |
| Servo / 3 | +60% forward for one second, neutral pause, -60% reverse for one second, then stop (0%) |
| Sound / 4 | Amplifier startup pause, an original short melody at Scratch volume 85%, drain pause, then restore speaker enable |
| Mic / 5 | Enable microphone and show its RMS for eight seconds, restoring prior enable |
| Display / 6 | 40% LCD brightness for one second, restoring prior brightness |
| Camera / 7 | Queue one JPEG capture when camera is ready; show its separate result |
| Sprite / 8 | Say `The temp is: 23.5 C` using nested join blocks and `round(temp * 10) / 10` |

The moving SVG face, PNG sprite and pen trail demonstrate the original graphical features. Only reads and animation run on green flag. Outputs are requested explicitly. A single worker runs the timed output examples so repeated clicks cannot restart a pulse halfway through its cleanup. Other output requests are ignored while an example runs; Next remains available.

The servo value is normalized percent, not degrees: V1.0 uses continuous rotation and V1.1 uses position. MQ2 voltage is not calibrated gas concentration. Microphone RMS is percent of full scale, not dB SPL. Output-state pages show requested settings, not physical feedback. An accepted write does not prove successful actuation. Camera result 1 means saved and -1 means failed.

Hall/touch-pad require the local Ethernet/non-radio profile because their pins conflict with the radio. Camera requires the separate experimental camera profile. Disabled microphone readings can be unavailable. Radio/UART transactions and live camera backgrounds are not exposed by this version; this SB3 cannot enable unsupported firmware features. SD project/assets loading, display, touch and speaker playback are exercised by the project itself.

Let timed output examples finish before stopping: Scratch Stop cancels scripts, including their cleanup. The current text renderer uses ASCII, so the speech example ends in `C`; degree-symbol support is still pending. The player supports the standard round opcode, although the builder palette does not yet offer it. Imported round blocks retain their inputs and execute normally.

Regenerate with `python3 tests/make_hardware_demo.py`. Its adjacent `.sb3.json` streams the archive from SD. The data pack includes both demos. Runtime checks use the real interpreter with simulated full/missing hardware; physical sensor/actuator behavior requires a board run.
