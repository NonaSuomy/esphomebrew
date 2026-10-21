# Video Player

Launching `video.pax` from PAX opens the media browser at `/sd/videos/`, with
`/sd/` as a fallback. The launcher passes the executable's path as its argument;
the player treats `.pax` arguments as a browser launch. A media path or URL passed
by another app starts playback and returns to that app afterward.

Version 0.2.0 adds MSMPEG4v2, Theora, HEVC, VP9 and AV1 to H.264, MPEG-4 Part 2,
MJPEG and VP8. AV1 uses pinned libaom 3.9.1, a generic C, single-threaded, 8-bit
only decoder build. HEVC, VP9 and AV1 remain experimental on the ESP32-P4;
registration or emulator decoding does not establish smooth hardware playback.
Audio includes AAC, MP3, Opus, Vorbis and PCM.

## Matched samples

`tools/make_video_test_pack.py --help` describes generation of a local test pack.
It creates 37 short samples: nine video codecs at 320x180 and 640x360, twelve
audio variants, six container comparisons, and a flash/beep sync check. Each
output is fully decoded on the host and recorded with a SHA-256 manifest.
Copy `videos/pax-tests/` into `/sd/videos/pax-tests/` without changing its tree.
The generator does not upload files.

Initial bounded-heap RV32 emulator checks passed 33 of 37 samples. The 640x360
AV1 sample did not produce a video frame during its 12-second test window.
MPEG-TS/MTS/M2TS samples exhausted the 20 MiB test heap during stream probing;
those containers need further work. Other samples produced video/audio as
applicable. These are functional emulator results, not ESP32-P4 speed claims.
The player logs decoder names, frame counts/drops and elapsed playback time.

## Browser regression

Build the player, then run:

```sh
PAX_VIDEO_BINARY=/tmp/pax-video-release-build/video.pax \
  pax-tools/python-desktop/bin/python -m unittest discover \
  -s pax-modules/tests -p test_video_launch_emulated.py
```

This runs the actual RV32 executable and compares browser images for direct and
launcher-style launches. Use a real media argument separately to test playback.
