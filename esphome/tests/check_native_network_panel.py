#!/usr/bin/env python3
"""Exercise the production native panel using an already compiled SDL LVGL library.

Run pax-tools/build-firmware.sh compile desktop first. This check uses synthetic
network data; physical radios, credentials and the display are not required.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
build = ROOT / ".esphome/build/pax-desktop"
libraries = list((build / ".pioenvs/pax-desktop").glob("*/liblvgl.a"))
if len(libraries) != 1:
    raise SystemExit("Compile the desktop firmware profile first (one liblvgl.a required)")
with tempfile.TemporaryDirectory() as folder:
    executable = Path(folder) / "native-network-panel"
    subprocess.run([
        "c++", "-std=c++17", "-DLV_CONF_INCLUDE_SIMPLE",
        "-I" + str(ROOT / "components/pax_loader"), "-I" + str(build / "src"),
        "-I" + str(build / ".piolibdeps/pax-desktop/lvgl"),
        str(ROOT / "tests/components/pax_loader/native_network_panel_test.cpp"),
        str(libraries[0]), "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
