#!/usr/bin/env python3
"""Preview a three-way upstream core update without changing active sources."""
from pathlib import Path
import argparse, json, subprocess, tempfile, difflib
ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('app'); p.add_argument('--ref', required=True); p.add_argument('--out', type=Path)
a = p.parse_args()
bases = json.loads((ROOT / 'upstream-bases.json').read_text())
if a.app not in bases: p.error('app has no copied core; use its linked Git repository or manifest pin')
b = bases[a.app]; repo = ROOT / b['repository']
def git(*args):
    return subprocess.check_output(['git', '-C', str(repo), *args])
revision = git('rev-parse', '--verify', a.ref + '^{commit}').decode().strip()
out = a.out or ROOT / '.cache' / 'upstream-preview' / (a.app + '-' + revision[:12])
out.mkdir(parents=True, exist_ok=True)
report = dict(app=a.app, base=b['ref'], incoming=revision, changed=[], conflicts=[], added=[], deleted=[])
patch = []
new_files = set(git('ls-tree', '-r', '--name-only', revision, '--', b['upstream_path']).decode().splitlines())
selected = {b['upstream_path'] + '/' + f for f in b['files']}
old_files = set(git('ls-tree', '-r', '--name-only', b['ref'], '--', b['upstream_path']).decode().splitlines())
report['added'] = sorted(new_files - old_files)
for rel in b['files']:
    upstream = b['upstream_path'] + '/' + rel
    active = ROOT / b['active_path'] / rel
    if upstream not in new_files:
        report['deleted'].append(rel); continue
    old = git('show', b['ref'] + ':' + upstream)
    new = git('show', revision + ':' + upstream)
    current = active.read_bytes()
    if old == new: continue
    if b'\0' in old + new + current:
        report['conflicts'].append(rel + ' (binary; review manually)'); continue
    with tempfile.TemporaryDirectory() as td:
        paths = [Path(td) / name for name in ('ours', 'base', 'theirs')]
        for path, data in zip(paths, (current, old, new)): path.write_bytes(data)
        proc = subprocess.run(['git', 'merge-file', '-p', '-L', 'PAX', '-L', 'recorded upstream base', '-L', 'incoming upstream', *map(str, paths)], capture_output=True)
        if proc.returncode < 0 or proc.returncode > 127: raise RuntimeError(proc.stderr.decode())
        candidate = out / 'candidates' / rel
        candidate.parent.mkdir(parents=True, exist_ok=True); candidate.write_bytes(proc.stdout)
        if proc.returncode: report['conflicts'].append(rel)
        else:
            report['changed'].append(rel)
            local = b['active_path'] + '/' + rel
            patch.extend(difflib.unified_diff(current.decode().splitlines(True), proc.stdout.decode().splitlines(True), fromfile='a/' + local, tofile='b/' + local))
patch_path = out / 'update.patch'
if report['conflicts']:
    if patch_path.exists(): patch_path.unlink()
else: patch_path.write_text(''.join(patch))
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(dict(preview=str(out), **report), indent=2))
raise SystemExit(bool(report['conflicts']))
