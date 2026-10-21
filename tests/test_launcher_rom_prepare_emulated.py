"""A bad ZIP keeps the real RV32 picker alive without saving or launching it."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = os.environ.get('PAX_LAUNCHER_BINARY')

@unittest.skipUnless(BINARY, 'set PAX_LAUNCHER_BINARY to exercise the compiled launcher')
class RomPreparationEmulated(unittest.TestCase):
    def test_bad_zip_keeps_picker_open_and_selection_unchanged(self):
        with tempfile.TemporaryDirectory() as td:
            folder = Path(td)
            sd = folder / 'sd'
            apps = sd / 'roms/pax'
            apps.mkdir(parents=True)
            roms = sd / 'roms/nes'
            roms.mkdir()
            (apps / 'nes.pax').write_bytes(b'PAX')
            (apps / 'nes.json').write_text(json.dumps({'title': 'NES'}))
            (apps / '.launcher_ui_app_details').write_text('0\n')
            (apps / 'nes.rom').write_text('/sd/roms/nes/original.nes\n')
            (roms / 'broken.zip').write_bytes(b'not a ZIP')
            script = folder / 'input.json'
            script.write_text(json.dumps([
                {'at': at + delta, 'type': 'key', 'key': 13, 'down': down}
                for at in (1.5, 2.5) for delta, down in ((0, True), (.15, False))
            ]))
            report = folder / 'report.json'
            result = subprocess.run([
                str(ROOT.parent / 'pax-tools/python-desktop/bin/python'),
                str(ROOT.parent / 'pax-tools/desktop/host.py'), 'emulated',
                '--pax', str(Path(BINARY).resolve()), '--headless', '--seconds', '5',
                '--sd-root', str(sd), '--input-script', str(script), '--report', str(report)
            ], env={**os.environ, 'SDL_AUDIODRIVER': 'dummy'}, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])
            data = json.loads(report.read_text())
            self.assertIsNone(data['error'])
            self.assertEqual(data['services'].get('file_zip_extract_first'), 1, result.stderr)
            self.assertEqual(data['services'].get('settings_rom_path_set', 0), 0)
            self.assertEqual(data['services'].get('task_delete'), 1)
            self.assertGreater(data['frames'], 10)
            self.assertEqual((apps / 'nes.rom').read_text(), '/sd/roms/nes/original.nes\n')
            self.assertNotIn('opening /sd/roms/pax/nes.pax', result.stderr)

if __name__ == '__main__':
    unittest.main()
