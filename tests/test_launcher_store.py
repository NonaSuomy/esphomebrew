"""Launcher store recipe and isolated SDK regression checks."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_pax as bp


class LauncherStoreTests(unittest.TestCase):
    def test_launcher_is_publishable_with_its_own_sources(self):
        manifest = json.loads((bp.APPS / 'launcher_ui/pax.json').read_text())
        self.assertTrue(manifest.get('publish', True))
        folder = bp.source_file(bp.ROOT, manifest['source']['path'])
        files = bp.group_files(bp.ROOT, {'dir': manifest['source']['path'], 'files': manifest['files']})
        self.assertTrue(all((folder / f).is_file() for f in files))
        # Font glyphs are included by main.c, not separate translation units.
        self.assertNotIn('fontawesome_icons.c', files)
        self.assertIn('main.c', files)
        listing = bp.store_info('launcher_ui', manifest, folder)
        self.assertEqual(listing['category'], 'Tool')
        self.assertTrue(listing['controls'])

    def test_protocol_headers_are_isolated_and_paths_cannot_escape(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / 'repo'
            sdk = root / 'esphome/components/pax_loader'
            sdk.mkdir(parents=True)
            (sdk / 'psram_app.h').write_text('/* ABI */')
            (sdk / 'zlib.h').write_text('/* must not shadow app libraries */')
            target = Path(td) / 'staged'
            with patch.object(bp, 'ROOT', root):
                result = bp.build_sdk_headers(root, ['local:esphome/components/pax_loader/psram_app.h'], target)
                self.assertEqual([p.name for p in result.iterdir()], ['psram_app.h'])
                with self.assertRaises(ValueError):
                    bp.build_sdk_headers(root, ['local:../secret.h'], target)
                with self.assertRaisesRegex(ValueError, 'duplicate'):
                    bp.build_sdk_headers(root, ['local:esphome/components/pax_loader/psram_app.h'] * 2, target)


if __name__ == '__main__':
    unittest.main()
