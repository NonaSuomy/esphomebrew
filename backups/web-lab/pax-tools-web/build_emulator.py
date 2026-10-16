#!/usr/bin/env python3
"""Build the pinned PAX Unicorn.js fork in an isolated, ignored source cache."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'pax-tools/upstream/unicorn-js'
CACHE = ROOT / 'pax-tools/.cache/web/unicorn-fixed'
parser = argparse.ArgumentParser()
parser.add_argument('--emcc', default=shutil.which('emcc') or str(Path.home() / 'code/emsdk/upstream/emscripten/emcc'))
parser.add_argument('--jobs', type=int, default=4)
args = parser.parse_args()
if args.jobs < 1:
    parser.error('--jobs must be positive')

def git(*arguments):
    return subprocess.check_output(['git', '-C', str(SOURCE), *arguments]).decode().strip()

commit = git('rev-parse', 'HEAD')
core_commit = git('ls-tree', 'HEAD', 'unicorn').split()[2]
metadata_path = CACHE / 'engine.json'
output = CACHE / 'unicorn_riscv.js'
if output.exists() and metadata_path.exists():
    metadata = json.loads(metadata_path.read_text())
    if metadata['source_commit'] == commit and metadata['sha256'] == hashlib.sha256(output.read_bytes()).hexdigest():
        print('Corrected Unicorn engine already built:', output)
        sys.exit(0)
compiler = Path(args.emcc).resolve()
if not compiler.exists():
    raise SystemExit('Emscripten is required to build the corrected engine; use --emcc or install the workspace SDK.')
if git('status', '--porcelain').strip():
    raise SystemExit('Commit Unicorn.js source changes before building a pinned engine.')
core = SOURCE / 'unicorn'
if not (core / 'uc.c').exists():
    subprocess.run(['git', '-C', str(SOURCE), 'submodule', 'update', '--init', '--depth', '1', 'unicorn'], check=True)
checkout = CACHE / ('source-' + commit[:12])
if not checkout.exists():
    checkout.mkdir(parents=True)
    archive = subprocess.check_output(['git', '-C', str(SOURCE), 'archive', commit])
    with tarfile.open(fileobj=io.BytesIO(archive)) as source_tar:
        source_tar.extractall(checkout, filter='data')
    subprocess.run(['git', 'clone', '--no-hardlinks', '--no-checkout', str(core), str(checkout / 'unicorn')], check=True)
    subprocess.run(['git', '-C', str(checkout / 'unicorn'), 'checkout', '--detach', core_commit], check=True)
env = os.environ.copy()
env['PATH'] = str(compiler.parent) + os.pathsep + env['PATH']
env['PAX_EMULATOR_JOBS'] = str(args.jobs)
subprocess.run([sys.executable, 'build.py', 'riscv'], cwd=checkout, env=env, check=True)
text = (checkout / 'dist/unicorn_riscv.js').read_text()
if text.count('function getValue(') != 1:
    raise ValueError('Unexpected Unicorn wrapper; bulk memory hook was not applied')
text = text.replace('function getValue(', 'Module.paxHeap=()=>HEAPU8;function getValue(')
output.write_text(text)
metadata = {
    'version': '2.1.4+pax.1',
    'source_commit': commit,
    'upstream_base_commit': '560284c59c11c8d6c94d537f4354339f069ddcdf',
    'unicorn_commit': core_commit,
    'compiler': subprocess.check_output([str(compiler), '--version'], env=env).decode().splitlines()[0],
    'patch': 'src/patches/pax-helper-temporaries.patch',
    'patch_sha256': hashlib.sha256((SOURCE / 'src/patches/pax-helper-temporaries.patch').read_bytes()).hexdigest(),
    'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
    'heap_view': 'Expose live heap view for bounded bulk memory access',
}
metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
print('Built corrected Unicorn RISC-V engine:', output)
