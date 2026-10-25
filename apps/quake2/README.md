# Quake II PAX

Game data lives in `/sd/roms/quake2/baseq2/`. Optional CD music uses
`baseq2/music/track02.mp3` through `track11.mp3` (track 1 was the CD's data
track). Quake II selects the appropriate track for each level and loops it.
Unpadded names such as `track2.mp3` and the legacy `quake2/music/` directory
are also supported. The Ogg-oriented `tracklist.cfg` is not used by this port.

The right analog stick looks around; L1 (the upper-left shoulder) sends F1 to
open/acknowledge the help computer. Stick look uses a 20% deadzone and elapsed
time, so its speed does not depend on the renderer's frame rate. It needs the
Quake II stick-to-mouse routing in the quick-launcher firmware; older loaders
merge the right stick into movement/shoulder buttons. Other apps keep their
existing mappings. The firmware configuration currently running on the P4 is
`/home/nonasuomy/code/esphome006/config/esp32-p4-elecrow-pax-3button.yaml`, using
the loader component from `pax-conversions`, not the full-library YAML here.
The native three-button screen accepts D-pad/left-stick navigation and A to
activate the focused choice. Its network status icon is drawn directly rather
than relying on the missing Ethernet glyph in the default LVGL font.

In Quake menus, A or Start confirms and B goes back. Select/Minus sends Escape
in any context (put away the help computer, open the main menu, or back out).
B retains jump/use in gameplay. The key chosen on a button press is retained
until release, including when that press closes or opens a menu.

Single-player death opens Load Game after an action-button press; this is the
original game's behavior, not a crash. Select the first `ENTERING ...` autosave
with A/Start to reload the level, or use B/Minus to leave the menu. Save slots
live in `baseq2/save/`. The PAX implements folder creation, file listing,
stat and removal so Quake can create/copy its autosave and restore it. Start
a New Game once after updating if the previous build left the slots empty.

The fixed-point PacketVideo MP3 decoder is pinned to the same source as the
Quake PAX. Decoding and SD reads run on a separate, lower-priority task, with
a 743 ms stereo ring feeding the steady-rate audio output task. Supplied
44.1 kHz tracks are averaged down to the game's 22.05 kHz output; 22.05 kHz
MP3 files are also accepted. Music and sound effects have independent volume:

```text
cd_volume 0.25       // music volume, range 0..1 (default 0.25)
cd_nocd 1           // disable music; 0 enables it
cd loop 9           // manually select a looping track
cd pause
cd resume
cd stop
```

Missing tracks leave sound effects working. Exiting the PAX joins both audio
tasks and closes the music file before app code and buffers are unmapped.

## Device regression

Use the shared queue for builds, staging, launch, and log capture:

```sh
python3 tools/device_test_queue.py run --device 10.20.30.180 --owner quake2-music -- \
  bash -lc '. /home/nonasuomy/esp/esp-idf/export.sh >/dev/null && \
    python3 ports/quake2/tests/build_smoke.py && \
    env -u PYTHONHOME -u PYTHONPATH /usr/bin/python3 \
      ports/quake2/tests/device_smoke.py --audio-check --music-check --input-check --skip-capture'
```

The smoke build sends three Enter events to start `base1`; it is separate from
the normal `dev-paxs/quake2.pax`. Verification requires gameplay frames,
three healthy 5-second audio windows, two music windows with no underruns,
two L1/F1 press-release pairs, and clean PAX exit/heap release. The runner
closes any prior game/parent-launcher chain before launching the test.
`QUAKE2 MUSIC STREAM` logs report decoder
wall-time as a fraction of audio duration, including any task preemption.

`device_quick_ui.py` verifies all three focus choices, takes a native UI
screenshot, activates NETWORK with A, and closes the PAX launcher afterward.
Run it under the same queue. Compile/upload the quick firmware with
`PYTHONPATH=/home/nonasuomy/code/esphome006 /usr/bin/python3 -m esphome`;
the installed `esphome` executable is an older, incompatible checkout.

`test_input.c` verifies F1 edges, accumulated mouse movement, menu/load-stall
discard, menu A/Back and Select/Escape with matching releases, and analog look
at 9, 30, 120 and 1000 input polls per second. `test_files.c` verifies the
directory/glob/stat/remove bridge, including missing-service errors.

Build with `build_smoke.py --death-check` and run `device_smoke.py --death-check
--audio-check --music-check --input-check --skip-capture` under one lease to
verify death, B/back, reopening Load Game, A/autosave reload, recovered living
gameplay, steady sound/music, and clean exit. This test redirects save/config
writes to `baseq2/quake2-death-test/`; it does not overwrite real save slots.
Production builds do not contain this redirect or automated test input.

`ports/quake2/tests/test_music.c` tests the real decoder on the host using a
short 44.1 kHz stereo MP3 fixture: looping, ring wrap, pause/resume, disable,
volume, saturating mixing, non-looping EOF, missing tracks, and shutdown.
`test_sound.c` separately checks the SFX sample clock and stall recovery.
