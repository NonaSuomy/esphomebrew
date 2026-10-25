"""Store completeness and private asset build boundaries."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_pax as bp
import dev_build_apps as dev

PUBLIC = ('azip', 'recipe', 'wolf4sdl', 'spear', 'desktop', 'quake2',
          'scratch', 'video', 'template', 'opentyrian', 'tiberiansun', 'anotherworld', 'stargus')
PRIVATE = ()


class StoreExpansionTests(unittest.TestCase):
    def manifest(self, name):
        return json.loads((bp.APPS / name / 'pax.json').read_text())

    def test_requested_public_apps_have_publishable_listings(self):
        for name in PUBLIC:
            with self.subTest(name=name):
                manifest = self.manifest(name)
                self.assertTrue(manifest.get('publish', True))
                self.assertTrue(bp.public_build_manifest(manifest))
                listing = bp.store_info(name, manifest, bp.APPS / name)
                self.assertIn('extract', listing)
        self.assertEqual(self.manifest('desktop')['title'], 'Workspace')

    def test_legacy_private_data_flags_cannot_enable_publication(self):
        manifest = {'publish': False, 'requires_private_data': True,
                    'publication_blocker': 'private assets'}
        self.assertFalse(bp.public_build_manifest(manifest))
        manifest['publish'] = True
        with self.assertRaises(ValueError):
            bp.public_build_manifest(manifest)


    def test_custom_recipe_stages_current_sdk_without_name_or_cache_globals(self):
        with tempfile.TemporaryDirectory() as td:
            units, flags = bp.custom_units(self.manifest('azip'), bp.ROOT, Path(td))
            self.assertEqual(len(units), 17)
            self.assertTrue(any('-I' in f and '/sdk/azip' in f for f in units[0].flags))
            self.assertIn('-Wl,--wrap=fopen', flags)

    def test_workspace_build_preserves_nonzero_entry_and_disables_sd_staging(self):
        with tempfile.TemporaryDirectory() as td:
            workspace = Path(td)
            root = workspace / 'pax-modules'
            root.mkdir()
            builder = workspace / 'pax-recipe/tools/build_stargus_pax.sh'
            builder.parent.mkdir(parents=True)
            builder.write_text('#!/bin/sh\n')
            manifest = {'name': 'stargus', 'version': '0.1.0', 'source': {},
                        'publish': False, 'requires_private_data': True,
                        'publication_blocker': 'converted game data',
                        'workspace_build': {'script': builder.name,
                            'output': 'pax-modules/.cache/native/stargus.pax'}}
            output = root / '.cache/native/stargus.pax'
            output.parent.mkdir(parents=True)
            output.write_bytes(bp.PAX_HEADER.pack(bp.PAX_MAGIC, 1, 4, 16, 0, 8, 0, 0) + b'x' * 16)
            with patch.object(bp, 'ROOT', root), patch.object(bp, 'run') as run:
                info = bp.build_workspace_app(manifest, root / 'pax.json', workspace / 'dist')
            self.assertEqual(info['entry_offset'], 4)
            self.assertFalse(info['publish'])
            self.assertEqual(run.call_args.kwargs['env']['PAX_STAGE_SD'], '0')
            self.assertEqual(run.call_args.kwargs['env']['PAX_STAGE_DIR'], '')

    def test_generated_codec_archive_is_linked_before_newlib(self):
        with tempfile.TemporaryDirectory() as td:
            source = Path(td)
            manifest = {'name': 'codec-test', 'groups': [], 'newlib': True,
                        'libraries': ['.pax-gen/aom-build/libaom.a']}
            _, flags = bp.custom_units(manifest, source, source / 'objects')
            library = str(source / '.pax-gen/aom-build/libaom.a')
            self.assertIn(library, flags)
            self.assertLess(flags.index(library), flags.index('-lc'))

    def test_per_group_engine_flags_do_not_leak_to_other_units(self):
        with tempfile.TemporaryDirectory() as td:
            manifest = {'name': 'group-flags', 'groups': [
                {'dir': 'local:apps/anotherworld', 'files': ['pax_data.cpp'],
                 'force_includes': ['local:apps/anotherworld/pax_data.h'], 'cxxflags': ['-fno-rtti', '-O2']},
                {'dir': 'local:apps/anotherworld', 'files': ['pax_main.cpp']} ]}
            units, _ = bp.custom_units(manifest, bp.ROOT, Path(td))
            self.assertIn('-fno-rtti', units[0].flags)
            self.assertIn('-include', units[0].flags)
            self.assertNotIn('-fno-rtti', units[1].flags)
            self.assertNotIn('-include', units[1].flags)

    def test_spear_has_separate_campaign_build(self):
        spear = self.manifest('spear')
        wolf = self.manifest('wolf4sdl')
        self.assertIn('-DSPEAR=1', spear['cflags'])
        self.assertNotIn('-DSPEAR=1', wolf['cflags'])
        self.assertTrue(all('/spear/' in item['path'] for item in spear['requires']))


if __name__ == '__main__':
    unittest.main()
