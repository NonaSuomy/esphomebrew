"""Stage a build and its matching metadata/ELF under the single debug name."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path, help='build_pax.py output directory')
parser.add_argument('elf', type=Path, help='ELF from that exact build')
parser.add_argument('destination', type=Path)
args = parser.parse_args()
binary = (args.build / 'redalert.pax').read_bytes()
info = json.loads((args.build / 'redalert.json').read_text())
assert hashlib.sha256(binary).hexdigest() == info['sha256'], 'PAX/metadata mismatch'
args.destination.mkdir(parents=True, exist_ok=True)
info.update(name='redalert-debug', file='redalert-debug.pax',
            version='0.2.56-dev-balanced-av-movie-tail', publish=False)
(args.destination / 'redalert-debug.pax').write_bytes(binary)
(args.destination / 'redalert-debug.json').write_text(json.dumps(info, indent=2) + '\n')
shutil.copy2(args.elf, args.build / 'redalert-debug.elf')
# Debug port has no bundled data; discard misleading historical release info.
listing = {'name': 'redalert-debug', 'version': info['version'], 'files': []}
(args.destination / 'redalert-debug.files').write_text(json.dumps(listing, indent=2) + '\n')
print('Staged redalert-debug.pax:', info['sha256'])
