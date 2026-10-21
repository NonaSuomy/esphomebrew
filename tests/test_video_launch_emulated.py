"""Exercise the real RV32 browser with PAX_VIDEO_BINARY set."""
from pathlib import Path
import json
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = os.environ.get('PAX_VIDEO_BINARY')


@unittest.skipUnless(BINARY, 'set PAX_VIDEO_BINARY to test the compiled player')
class VideoLaunchTests(unittest.TestCase):
    def test_launcher_executable_argument_opens_same_browser_as_direct_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / 'videos').mkdir()
            (folder / 'videos' / 'sample.mp4').write_bytes(b'')
            pictures = []
            for index, argument in enumerate(('', '/sd/roms/pax/video.pax')):
                report = folder / f'{index}.json'
                picture = folder / f'{index}.ppm'
                result = subprocess.run([
                    str(ROOT.parent / 'pax-tools/python-desktop/bin/python'),
                    str(ROOT / 'tests/video_playback_fixture.py'), 'emulated',
                    '--pax', str(Path(BINARY).resolve()), '--sd-root', str(folder),
                    '--app-arg', argument, '--headless', '--seconds', '3',
                    '--report', str(report), '--screenshot', str(picture),
                ], env={**os.environ, 'SDL_AUDIODRIVER': 'dummy'},
                   capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr[-2000:])
                self.assertIsNone(json.loads(report.read_text())['error'])
                self.assertNotIn('not a video or sound file', result.stderr)
                self.assertTrue(picture.exists())
                pictures.append(picture.read_bytes())
            self.assertEqual(pictures[0], pictures[1])


if __name__ == '__main__':
    unittest.main()
