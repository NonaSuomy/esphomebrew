"""Check actual pinned MicroPython freezing against reversed directory enumeration."""
import contextlib
import importlib.util
import io
import json
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch


def main():
    root = Path(__file__).resolve().parent.parent
    manifest_path = root / 'apps/tulip/pax.json'
    if not manifest_path.exists():
        manifest_path = root / 'apps/tulip/papp.json'
    ref = json.loads(manifest_path.read_text())['source']['ref']
    tools = root / '.cache' / ('src-tulip-' + ref[:12]) / 'micropython/tools'
    sys.path.insert(0, str(tools))
    import manifestfile
    import makemanifest
    with tempfile.TemporaryDirectory() as folder:
        home = Path(folder)
        for name in ('z.py', 'a.py', 'pkg/z.py', 'pkg/a.py', 'other/m.py'):
            p = home / name
            p.parent.mkdir(exist_ok=True)
            p.write_text('value = ' + repr(name) + '\n')
        original_walk = manifestfile.os.walk
        def reversed_walk(*args, **kwargs):
            for directory, dirs, files in original_walk(*args, **kwargs):
                dirs.sort(reverse=True)
                yield directory, dirs, sorted(files, reverse=True)
        outputs = []
        for walker in (original_walk, reversed_walk):
            m = manifestfile.ManifestFile(manifestfile.MODE_FREEZE, {'MPY_LIB_DIR': ''})
            with patch.object(manifestfile.os, 'walk', walker):
                m.freeze_as_str(str(home))
            files = m.files()
            assert [f.target_path for f in files] == sorted(f.target_path for f in files)
            assert all(f.kind == manifestfile.KIND_FREEZE_AS_STR for f in files)
            with contextlib.redirect_stdout(io.StringIO()):
                outputs.append(makemanifest.generate_frozen_str_content([(f.full_path, f.target_path) for f in files]))
        assert outputs[0] == outputs[1], 'Filesystem ordering changed frozen C output'
    print('Tulip freezing is identical with reversed directory and file enumeration')


if __name__ == '__main__':
    main()
