"""Portable C6 image integrity and OTA fault-injection checks."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class C6UpdateTests(unittest.TestCase):
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
