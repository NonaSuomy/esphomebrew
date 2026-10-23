"""Keep esptool importable while pioarduino relocates its package directory."""
import os
import importlib
from pathlib import Path
import sys

core = os.environ.get('PLATFORMIO_CORE_DIR')
if core:
    source = Path(core) / 'tools/tool-esptoolpy'
    if (source / 'esptool/__init__.py').is_file():
        # penv/bin/esptool.py can otherwise shadow the real esptool package.
        sys.path.insert(0, str(source))
        # Python adds the script directory after site initialization. Preload
        # the package so a later bin/esptool.py cannot replace this resolution.
        importlib.import_module("esptool")
