#!/usr/bin/env bash
# Local, private backup including committed history and uncommitted source.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
dest="${1:-$root/pax-github/backups/$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$dest"
python "$root/pax-tools/workspace-status.py" > "$dest/status.json"
python - "$root" "$dest" <<'BACKUP'
from pathlib import Path
import subprocess, sys, tarfile
root, dest = map(Path, sys.argv[1:])
for repo in sorted(p.parent for p in root.rglob('.git') if not any(x in p.parts for x in ['idf','data','.cache','.esphome','platformio','.venv','venv'])):
    name = str(repo.relative_to(root)).replace('/', '--') or 'workspace'
    subprocess.run(['git','-C',str(repo),'bundle','create',str(dest / (name + '.bundle')),'--all'],check=True)
with tarfile.open(dest / 'working-source.tar.gz','w:gz') as tar:
    def include(info):
        parts = Path(info.name).parts
        if any(p in {'.git','.cache','.esphome','idf','platformio','data','.venv','venv','backups','build_pax','firmware','__pycache__'} or p.startswith('build-') for p in parts): return None
        return info
    for path in root.iterdir():
        if path.name != '.git': tar.add(path,arcname=path.name,filter=include)
BACKUP
printf 'Backup saved: %s\n' "$dest"
