#!/usr/bin/env bash
# Private snapshot of source, compiled apps and every owned repository's local Git state.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
dest="${1:-$root/pax-github/backups/$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$dest"
python "$root/pax-tools/workspace-status.py" > "$dest/status.json"
python - "$root" "$dest" <<'BACKUP'
from pathlib import Path
import sys, tarfile
root, dest = map(Path, sys.argv[1:])
skip = {'.cache','.esphome','.venv','venv','backups','build_pax','__pycache__'}
with tarfile.open(dest / 'workspace.tar.gz','w:gz',compresslevel=1) as tar:
    def include(info):
        parts = Path(info.name).parts
        if any(p in skip for p in parts): return None
        if parts[:2] in {('pax-tools','idf'),('pax-tools','platformio'),('pax-dev','data')}: return None
        if info.name.endswith(('.pyc','.o','.obj')):return None
        return info
    for path in root.iterdir():
        tar.add(path,arcname=path.name,filter=include)
print(dest / 'workspace.tar.gz')
BACKUP
printf 'Backup saved: %s\n' "$dest"
