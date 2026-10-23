"""Portable C6 image integrity and OTA fault-injection checks."""
from pathlib import Path
import os
import importlib.util
import json
import hashlib
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class C6UpdateTests(unittest.TestCase):
    def test_transport_selection_rejects_other_wiring(self):
        spec=importlib.util.spec_from_file_location('c6_profiles',ROOT/'components/pax_loader/c6_profiles.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        select=module.select_profile
        aio=dict(variant='ESP32C6',type='spi',active_high=True,reset_pin=2,cs_pin=14,clk_pin=15,mosi_pin=7,miso_pin=9,data_ready_pin=10,handshake_pin=11)
        self.assertEqual(select(aio),'c6-spi-aio')
        for key in ['reset_pin','cs_pin','clk_pin','mosi_pin','miso_pin','data_ready_pin','handshake_pin']:
            self.assertEqual(select({**aio,key:99}),'')
        self.assertEqual(select({**aio,'spi_mode':2}),'')
        self.assertEqual(select({**aio,'active_high':False}),'')
        self.assertEqual(select(dict(variant='ESP32C6',type='sdio')),'c6-sdio')
        self.assertEqual(select(dict(variant='ESP32C6',type='sdio',bus_width=1)),'')
        self.assertEqual(select(dict(variant='ESP32',type='sdio')),'')
        self.assertEqual(select({}), '')

    def test_image_and_transfer_failures(self):
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "c6-update-test"
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-missing-field-initializers", "-fsanitize=address,undefined",
                "-I" + str(ROOT / "components/pax_loader"),
                str(ROOT / "tests/components/pax_loader/c6_update_test.cpp"),
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

if __name__ == "__main__":
    unittest.main()
