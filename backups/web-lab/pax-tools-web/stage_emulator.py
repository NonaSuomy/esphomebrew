#!/usr/bin/env python3
"""Stage the pinned Unicorn RISC-V engine, ABI schema and verified boot apps."""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import tarfile
import urllib.request
ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, default=ROOT / 'pax-dev/web')
args = parser.parse_args()
VERSION = '2.1.4'
INTEGRITY = 'UAml23tgHyXEuP2A7Y50qDCNYMIRHJobsjlhetPRrtH+PXg0+BNAyvPQEKoCd+VHAF1nDfPHE4yJCqSLVtOrXA=='
cache = ROOT / 'pax-tools/.cache/web/unicorn'
package = cache / 'package/dist/unicorn_riscv.js'
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
if not package.exists():
    raw = urllib.request.urlopen(f'https://registry.npmjs.org/@alexaltea/unicorn-js/-/unicorn-js-{VERSION}.tgz', timeout=60).read()
    if base64.b64encode(hashlib.sha512(raw).digest()).decode() != INTEGRITY:
        raise ValueError('Unicorn package integrity mismatch')
    cache.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:gz') as archive:
        archive.extractall(cache, filter='data')
if hashlib.sha256(package.read_bytes()).hexdigest() != 'a26ba3712dac69d7ee93e181e8d7ae1bd8c327c12f74e819a8e380792587d11a':
    raise ValueError('Cached Unicorn RISC-V build integrity mismatch')
s = package.read_text()
if s.count('function getValue(') != 1:
    raise ValueError('Pinned Unicorn wrapper has changed')
s = s.replace('function getValue(', 'Module.paxHeap=()=>HEAPU8;function getValue(')
(out / 'unicorn_riscv.js').write_text(s)
shutil.copy2(cache / 'package/LICENSE', out / 'UNICORN-LICENSE.txt')
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
for name in ['launcher_ui', 'touchtest']:
    source = ROOT / 'pax-dev' / ('launcher_ui.pax' if name == 'launcher_ui' else 'apps/touchtest.pax')
    raw = source.read_bytes()
    shutil.copy2(source, apps / (name + '.pax'))
    metadata = ROOT / 'pax-dev/apps' / (name + '.json')
    if metadata.exists():
        shutil.copy2(metadata, apps / metadata.name)
    boot.append({'name': name, 'url': 'pax-apps/' + name + '.pax', 'size': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
(out / 'emulator-build.json').write_text(json.dumps({'unicorn_version': VERSION, 'npm_integrity': 'sha512-' + INTEGRITY, 'source_commit': '560284c59c11c8d6c94d537f4354339f069ddcdf', 'wrapper_patch': 'Expose live heap view for bounded bulk memory access', 'apps': boot}, indent=2) + '\n')
for name in ['emulator.js', 'emulator-host.js']:
    shutil.copy2(Path(__file__).parent / name, out / name)
print('Staged Unicorn RISC-V 2.1.4 and ABI schema:', len(fields), 'services')
