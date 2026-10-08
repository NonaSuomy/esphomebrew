# Red Alert regression checks

Run after `tools/build_pax.py redalert` has populated the pinned source cache:

```sh
g++ -std=c++11 -O2 -I. -I.cache/src-redalert-bcce972e66b9 \
  ports/redalert/tests/audio_queue_test.cpp -o /tmp/redalert-audio-test
/tmp/redalert-audio-test
g++ -std=c++11 -O2 -ffunction-sections -fdata-sections \
  -I. -I.cache/src-redalert-bcce972e66b9 \
  ports/redalert/tests/video_blit_test.cpp -Wl,--gc-sections \
  -o /tmp/redalert-video-test
/tmp/redalert-video-test
g++ -std=c++11 -O2 -ffunction-sections -fdata-sections \
  -I. -I.cache/src-redalert-bcce972e66b9 \
  ports/redalert/tests/vqa_audio_test.cpp -Wl,--gc-sections \
  -o /tmp/redalert-vqa-audio-test
/tmp/redalert-vqa-audio-test
g++ -std=c++11 -O2 -DPAX_APP_SIDE=1 -ffunction-sections -fdata-sections \
  -I.cache/src-redalert-bcce972e66b9 -I.cache/src-redalert-bcce972e66b9/common \
  ports/redalert/tests/vqa_eof_test.cpp -Wl,--gc-sections \
  -o /tmp/redalert-vqa-eof-test
/tmp/redalert-vqa-eof-test
```

The tests include the actual port backends. Audio covers initial buffering,
underrun/refill, stopping and full-ring protection. Video compares opaque,
masked and clipped copies against the prior pixel-loop semantics, plus RGB565
palette conversion, unaligned tails and cursor clipping.
Video also checks the two-slot producer's full-queue protection, ordered slot
reuse, and unsigned sequence-counter rollover against the actual backend.
The VQA test checks that incomplete movie audio blocks are not submitted,
waiting for a block does not repeat the previous PCM, and completed blocks
advance and release their ring slots correctly.

Keep device builds, launches and log capture inside the lease described in
`docs/device-test-queue.md`. To stage a tested build under one network name:

```sh
python3 ports/redalert/tests/stage_debug.py /tmp/redalert-current \
  .cache/build/redalert/redalert.elf dev-paxs
```

The staging command verifies the PAX hash against generated metadata and
keeps the matching debug ELF in the build output. Do not symbolize an old
fault using an ELF whose binary payload does not match the faulting PAX.

The new audio patch prevents music voice eviction and treats unread source
audio as active during backend underruns. These need device gameplay testing
in addition to the host queue tests; a successful menu screenshot is not a
long-running gameplay/audio test.

## Movie timing checks

The movie profile counts newly decoded frames separately from repeated display
submissions. Do not compare redraw FPS to decoded FPS as though they were the
same metric. For the device's 3,402,506-byte `ENGLISH.VQA` pre-menu intro:

- Streaming profile: 12–15 fresh frames per roughly five seconds, with movie
  reads/seeks consuming 4.2–4.4 seconds of that interval.
- Small-movie preload test: 76 fresh frames per roughly five seconds, zero
  decoder-skipped frames, and movie reads/seeks consuming about 110 ms.
  Displayed frames measured about 13–14 fps during playback.

Only `ENGLISH.VQA` preloads before the playback clock starts, capped at 4 MiB.
The file destructor releases that cache at movie close. All mission movies use
the original streaming path without an additional BufferIO cache: general movie
caching caused a reported mission-video regression and out-of-memory error.
Preloading can add an initial loading pause. The recovery build streamed
`ALLY1.VQA` with zero failed allocations during the observed playback interval;
prolonged gameplay and the transition after the video still need verification.
Intro timing is not proof of either.

## Gameplay scheduling checks

Compare actual presented frames after the briefing, not only menu FPS.
Keeping the presenter below the audio mixer produced approximately 0–1 fps
during gameplay, despite 20–28 fps being submitted. Equal priorities alone,
and sleeping on contended audio locks alone, did not remove that bottleneck.
A priority-6 presenter on core 1, explicitly sleeping 10 ms after each complete
frame, measured approximately 8–9 fps in the same first-mission test. This is
an improvement, not a claim of full-speed gameplay or a final root-cause proof.
Firmware I2S tasks run above both port tasks. Retain original movie conversion;
internal conversion tiles and moving the presenter to core 0 regressed playback.
Audio tests additionally verify that a producer waits when a simulated mixer
owns its queue lock. Do not run the diagnostic screen stream during frame-rate
comparisons; capture screenshots separately.

Combined build `0.2.52-dev-bounded-av-presentation` (`17ea82c0d7e52e1f` hash
prefix) retained 15 fps during the main intro, measured about 11 fps during
`ALLY1.VQA`, and reached approximately 9 fps after entering gameplay. No new
allocation failure was logged during those intervals. Visual quality and
audio smoothness still need user confirmation; throughput is not proof of
either. The 8–9 fps gameplay result remains below the desired performance.

The running firmware's resampler is priority 4 (unpinned), below the old
priority-5 game task. The new game priority is 3 on core 0, leaving the
resampler able to preempt it. This is a PAX-only change; I2S/mixer firmware
priorities and the intro preload remain unchanged. Audio write latency was
variable even after this change in the menu test; do not claim click-free
gameplay without testing a mission.

Build 0.2.55 tested the port mixer at priority 7, above the priority-6
presenter. Audio writes improved in the menu, but part of the intro dropped
to 3 displayed fps. Do not retain that scheduling trade-off. Build 0.2.56
uses equal priority 6 for the mixer and presenter, with game priority 3 and
the firmware resampler unchanged at 4. Real-time pacing/blocking audio writes
are retained; device audio and mission-video confirmation is required.
The staged 0.2.56 binary has SHA-256 prefix `04f00e926e889d4a`. Its device
launch was rejected while another app was running; the retry observed Duke3D,
not Red Alert. Do not treat those capture intervals as validation of 0.2.56.

Patch 0022 distinguishes loader EOF from exhausted playback buffers. The
actual upstream playback loop is tested with three pending frames at EOF
and with manual skipping: EOF drains those frames, while a skip exits after
the first one. This fixes premature buffered-frame abandonment, not a proof
that all mission audio tails or timing issues have been resolved.
