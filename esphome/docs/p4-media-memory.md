# P4 media memory and audio scheduling

PAX keeps app allocations in PSRAM and reserves internal memory for peripheral
work. The common profile now prefers PSRAM for ordinary malloc requests larger
than 1 KiB, rather than the IDF default of 16 KiB. Explicitly internal/DMA
allocations retain their requested capabilities.

The P4 I2S speaker uses four 5 ms DMA buffers; its larger PSRAM ring buffer is
unchanged. Idle and paused waits always block for at least one scheduler tick,
even when the requested wait is shorter than the configured tick period. This
prevents the priority-19 speaker task from spinning while waiting for input.
Other ESP32 variants retain the 15 ms DMA period.

Repeated audio_init calls at the current sample rate reuse a running speaker.
Stopped speakers and rate changes still initialize the chain. Mute and volume
updates remain applied. tests/test_audio_init.py covers these transitions.

On the NANO/RPi 4.3-inch panel, native PPA conversion combines RGB565 rotation
and RGB888 conversion while retaining the double-buffer handoff needed for
stable scanout. The profile remains configured for the established DSI timings.

The Red Alert recovery build uses the current port and a 640x400 canvas.
Commercial game archives remain external to source control. Archive damage
must be corrected separately from firmware scheduling or memory problems.
