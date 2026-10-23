"""Fault-inject the same streaming transaction used by the on-device updater."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class FirmwareUpdateTests(unittest.TestCase):
    def test_only_verified_complete_image_becomes_bootable(self):
        with tempfile.TemporaryDirectory() as folder:
            executable=Path(folder)/'firmware-update-test'
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'components/pax_loader'),
                str(ROOT/'tests/components/pax_loader/firmware_update_test.cpp'),'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True)
if __name__=='__main__': unittest.main()
