import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from install_recipe import load, validate
from prepare_storage import destination, copy_new, install_group

class InstallationRecipesTests(unittest.TestCase):
    def test_registry_covers_every_manifest_and_native_package(self):
        entries=json.loads((ROOT/'install-recipes.json').read_text())
        self.assertEqual(len(entries),148)
        names={e['app'] for e in entries}
        for e in entries:self.assertEqual(load(ROOT/e['path'])['app'],e['app'])
        for p in (ROOT/'apps').glob('*/pax.json'):
            manifest=json.loads(p.read_text())
            if manifest['name']=='pax_emulator_common':continue
            self.assertIn(manifest['name'].removeprefix('psram_'),names)
            self.assertEqual(load(p.parent/manifest['install_recipe'])['app'],manifest['name'].removeprefix('psram_'))
        self.assertEqual(sum(n.startswith('scummvm-') for n in names),105)
        dev=ROOT.parent/'pax-dev/apps'
        if dev.exists():
            for p in dev.rglob('*.pax'):self.assertIn(p.stem.removesuffix('-debug'),names)

    def test_verified_zip_install_and_preserve_existing(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);incoming=root/'roms/test/incoming';incoming.mkdir(parents=True)
            data=b'public fixture data'
            archive=incoming/'game.zip'
            with zipfile.ZipFile(archive,'w') as z:z.writestr('data/file.dat',data)
            item={'source':'data/file.dat','target':'/sd/roms/test/file.dat','size':len(data),'sha256':hashlib.sha256(data).hexdigest()}
            group={'format':'zip-files','zip_name':'game.zip','archive_size':archive.stat().st_size,'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'files':[item]}
            recipe={'archive_dir':'/sd/roms/test/incoming'}
            install_group(recipe,group,root,root/'cache',True)
            target=root/'roms/test/file.dat';self.assertEqual(target.read_bytes(),data)
            target.write_bytes(b'owned existing file');install_group(recipe,group,root,root/'cache',True)
            self.assertEqual(target.read_bytes(),b'owned existing file')
            target.unlink();item['sha256']='0'*64
            with self.assertRaises(ValueError):install_group(recipe,group,root,root/'cache',True)
            self.assertFalse(target.exists());self.assertEqual(list(target.parent.glob('.pax-install-*')),[])

    def test_paths_and_failed_fat_copy(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            with self.assertRaises(ValueError):destination(root,'/sd/../escape')
            (root/'escape').symlink_to('/tmp')
            with self.assertRaises(ValueError):destination(root,'/sd/escape/file.dat')
            target=root/'good.dat'
            with patch('prepare_storage.os.link',side_effect=OSError('FAT')),patch('prepare_storage.shutil.copyfileobj',side_effect=OSError('disk full')):
                with self.assertRaises(OSError):copy_new(io.BytesIO(b'abc'),target,3)
            self.assertFalse(target.exists())
            with patch('prepare_storage.os.link',side_effect=OSError('FAT')):
                self.assertEqual(copy_new(io.BytesIO(b'abc'),target,3),'installed')
            self.assertEqual(target.read_bytes(),b'abc')
            self.assertEqual(copy_new(io.BytesIO(b'new'),target,3),'kept')
