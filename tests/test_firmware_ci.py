"""Publication requires a complete, matching and intact firmware matrix."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import struct
from unittest.mock import patch
import sys
import tempfile
import unittest
import zipfile

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
if not (TOOLS / 'firmware_ci.py').is_file():
    TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import firmware_ci as ci

class FirmwareCI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.saved = ci.ROOT, ci.FW, ci.CI, ci.WORK
        ci.ROOT = self.root; ci.FW = self.root / 'esphome'
        ci.CI = self.root / 'tools/ci'; ci.WORK = self.root / '.firmware-ci'
        ci.FW.mkdir(); (self.root / 'web-installer').mkdir()
        for key in ['one', 'two']:
            (ci.FW / (key + '.yaml')).write_text('substitutions:\n  device_name: test-' + key + '\n')
        ci.write(ci.FW / 'installer-profiles.json', {'one': 'one.yaml', 'two': 'two.yaml'})
        ci.write(self.root / 'web-installer/boards.json', [{'profiles': [['one', 'One'], ['two', 'Two']]}])
        ci.write(self.root / 'web-installer/build-info.json', {'firmware': []})
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        subprocess.run(['git', '-C', str(self.root), 'add', '.'], check=True)
        self.artifacts = self.root / 'artifacts'
        self.make_artifacts()

    def tearDown(self):
        ci.ROOT, ci.FW, ci.CI, ci.WORK = self.saved
        self.temp.cleanup()

    def make_artifacts(self):
        for key in ['one', 'two']:
            base = self.artifacts / key
            factory, ota = (key + '-factory').encode(), (key + '-ota').encode()
            record = {'board': key, 'sha256': ci.sha(factory), 'input_digest': ci.input_digest(),
                      'ota': {'path': 'ota/pax-' + key + '.ota.bin', 'sha256': ci.sha(ota)}}
            ci.write(base / 'records' / (key + '.json'), record)
            for name, data in [('firmware/pax-' + key + '.factory.bin', factory),
                               ('ota/pax-' + key + '.ota.bin', ota)]:
                p = base / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)
            ci.write(base / 'updates' / (key + '.json'), dict(profile=key, bytes=len(ota),
                sha256=ci.sha(ota),url='https://nonasuomy.github.io/esphomebrew/install/ota/pax-' + key + '.ota.bin'))
            ci.write(base / 'manifests' / (key + '.json'), {})
            (base / 'kits').mkdir()
            with zipfile.ZipFile(base / 'kits' / ('pax-' + key + '-flash-kit.zip'), 'w') as z:
                z.writestr('pax-' + key + '.factory.bin', factory)

    def test_esptool_wrapper_cannot_shadow_stable_package(self):
        core = self.root / 'platformio'
        stable = core / 'tools/tool-esptoolpy'
        (stable / 'esptool').mkdir(parents=True)
        (stable / 'esptool/__init__.py').write_text('FROM_STABLE_SOURCE = True\n')
        scripts = self.root / 'bin'; scripts.mkdir()
        (scripts / 'esptool.py').write_text('raise RuntimeError("shadow wrapper imported")\n')
        entry = scripts / 'esptool'
        entry.write_text('from esptool import FROM_STABLE_SOURCE\nassert FROM_STABLE_SOURCE\n')
        env = os.environ.copy(); env['PLATFORMIO_CORE_DIR'] = str(core)
        env['PYTHONPATH'] = str(TOOLS / 'ci')
        subprocess.run([sys.executable, str(entry)], env=env, check=True)
        env['PYTHONPATH'] = str(stable)
        bad = subprocess.run([sys.executable, str(entry)], env=env, capture_output=True)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn(b'shadow wrapper', bad.stderr)

    def test_update_manifest_uses_actual_image_and_layout(self):
        image = bytearray(100)
        image[0] = 0xe9
        struct.pack_into('<H', image, 12, 18)
        struct.pack_into('<HH', image, 15, 100, 299)
        table = b''.join(struct.pack('<HBBII16sI', 0x50aa, 0, 16+i,
                         0x10000+i*0x7c0000, 0x7c0000, ('app'+str(i)).encode(), 0) for i in range(2))
        with patch.dict(os.environ, {'PAX_FIRMWARE_BUILD_ID': '39969382944'}):
            manifest = ci.update_manifest('nano-lcd43', image, table)
        self.assertEqual(manifest['build_id'], 39969382944)
        self.assertEqual((manifest['min_revision'], manifest['max_revision']), (100, 299))
        self.assertEqual(manifest['partitions'][1]['address'], 0x7d0000)
        self.assertEqual(manifest['sha256'], ci.sha(image))
        with self.assertRaisesRegex(ValueError, 'two sufficiently'):
            ci.update_manifest('nano-lcd43', image, table[:32])
        image[12]=0
        with self.assertRaisesRegex(ValueError, 'ESP32-P4'):
            ci.update_manifest('nano-lcd43', image, table)

    def test_wrong_update_profile_rejected(self):
        path=self.artifacts/'one/updates/one.json'
        manifest=ci.load(path);manifest['profile']='two';ci.write(path,manifest)
        with self.assertRaisesRegex(ValueError,'manifest mismatch'):
            ci.collect(self.artifacts,self.root/'overlay')

    def test_complete_matrix(self):
        out = self.root / 'overlay'
        ci.collect(self.artifacts, out)
        info = ci.load(out / 'build-info.json')
        self.assertEqual(info['ci']['profiles'], 2)
        self.assertEqual({x['board'] for x in info['firmware']}, {'one', 'two'})
        self.assertEqual((out / 'ota/pax-two.ota.bin').read_bytes(), b'two-ota')

    def test_missing_profile_does_not_publish(self):
        (self.artifacts / 'two/records/two.json').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing matrix'):
            ci.collect(self.artifacts, self.root / 'overlay')
        self.assertFalse((self.root / 'overlay').exists())

    def test_corrupt_image_does_not_publish(self):
        (self.artifacts / 'one/firmware/pax-one.factory.bin').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            ci.collect(self.artifacts, self.root / 'overlay')

    def test_ota_corruption_does_not_publish(self):
        (self.artifacts / 'one/ota/pax-one.ota.bin').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            ci.collect(self.artifacts, self.root / 'overlay')

    def test_changed_sources_reject_old_matrix(self):
        (ci.FW / 'one.yaml').write_text('changed: true\n')
        with self.assertRaisesRegex(ValueError, 'stale'):
            ci.collect(self.artifacts, self.root / 'overlay')

    def test_duplicate_profile_rejected(self):
        ci.write(self.artifacts / 'duplicate/records/one.json', ci.load(self.artifacts / 'one/records/one.json'))
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            ci.collect(self.artifacts, self.root / 'overlay')

    def test_private_include_rejected(self):
        (ci.FW / 'one.yaml').write_text('packages:\n  private: !include private.yaml\n')
        (ci.FW / 'private.yaml').write_text('wifi:\n  ssid: PRIVATE\n  password: PRIVATE\n')
        with self.assertRaisesRegex(ValueError, 'Private'):
            ci.profiles()

    def test_secret_include_rejected(self):
        (ci.FW / 'one.yaml').write_text('api:\n  password: !secret api_password\n')
        with self.assertRaisesRegex(ValueError, 'Private'):
            ci.profiles()

    def test_registry_missing_advertised_profile(self):
        ci.write(ci.FW / 'installer-profiles.json', {'one': 'one.yaml'})
        with self.assertRaisesRegex(ValueError, 'registry'):
            ci.profiles()

    def test_promotion_rejects_changed_inputs_before_writes(self):
        out = self.root / 'overlay'; ci.collect(self.artifacts, out)
        original = (self.root / 'web-installer/build-info.json').read_bytes()
        (ci.FW / 'two.yaml').write_text('changed: true\n')
        with self.assertRaisesRegex(ValueError, 'inputs changed'):
            ci.promote(out)
        self.assertEqual((self.root / 'web-installer/build-info.json').read_bytes(), original)

if __name__ == '__main__':
    unittest.main()
