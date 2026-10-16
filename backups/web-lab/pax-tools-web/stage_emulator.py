#!/usr/bin/env python3
"""Stage the pinned Unicorn RISC-V engine, ABI schema and verified boot apps."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, default=ROOT / 'pax-dev/web')
args = parser.parse_args()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(Path(__file__).parent / 'build_emulator.py')], check=True)
cache = ROOT / 'pax-tools/.cache/web/unicorn-fixed'
engine = json.loads((cache / 'engine.json').read_text())
package = cache / 'unicorn_riscv.js'
if hashlib.sha256(package.read_bytes()).hexdigest() != engine['sha256']:
    raise ValueError('Corrected Unicorn engine integrity mismatch')
shutil.copy2(package, out / 'unicorn_riscv.js')
shutil.copy2(ROOT / 'pax-tools/upstream/unicorn-js/LICENSE', out / 'UNICORN-LICENSE.txt')
header = (ROOT / 'pax-esphome/components/pax_loader/psram_app.h').read_text()
header = re.sub('/\\*.*?\\*/', '', header, flags=re.DOTALL)
table = header[header.index('uint32_t abi_version;'):header.index('} app_services_t;')]
fields = []
for ret, name, args in re.findall('([\\w\\s*]+?)\\(\\*(\\w+)\\)\\((.*?)\\);', table, re.DOTALL):
    types = [] if args.strip() == 'void' else [re.sub('\\b\\w+$', '', v.strip()).strip() for v in args.split(',')]
    fields.append({'name': name, 'ret': ret.strip(), 'args': types})
(out / 'pax-abi.json').write_text(json.dumps(fields, indent=2) + '\n')
apps = out / 'pax-apps'
apps.mkdir(exist_ok=True)
boot = []
for name in ['launcher_ui', 'touchtest', 'nes']:
    source = ROOT / 'pax-dev' / ('launcher_ui.pax' if name == 'launcher_ui' else 'apps/' + name + '.pax')
    raw = source.read_bytes()
    shutil.copy2(source, apps / (name + '.pax'))
    metadata = ROOT / 'pax-dev/apps' / (name + '.json')
    if metadata.exists():
        shutil.copy2(metadata, apps / metadata.name)
    boot.append({'name': name, 'managed': name == 'nes', 'url': 'pax-apps/' + name + '.pax', 'size': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
(out / 'emulator-build.json').write_text(json.dumps({'engine': engine, 'unicorn_version': engine['version'], 'source_commit': engine['source_commit'], 'apps': boot}, indent=2) + '\n')
for name in ['emulator.js', 'emulator-host.js']:
    shutil.copy2(Path(__file__).parent / name, out / name)
print('Staged corrected Unicorn RISC-V and ABI schema:', len(fields), 'services')
