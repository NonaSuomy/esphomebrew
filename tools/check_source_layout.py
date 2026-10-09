#!/usr/bin/env python3
"""Validate active recipe inputs and dependency links without compiling."""
from pathlib import Path
import json, argparse
p = argparse.ArgumentParser(); p.add_argument("--recipes-only", action="store_true", help="validate the standalone publishing snapshot without native Git checkouts"); args = p.parse_args()
import build_pax as bp
root = Path(__file__).resolve().parents[1]
count = 0
for manifest in sorted((root / 'apps').glob('*/pax.json')):
    def check(value):
        global count
        if isinstance(value, str) and value.startswith('local:'):
            path = bp.source_file(root, value)
            if any(mark in value for mark in '*?['):
                directory, _, pattern = value[6:].rpartition('/')
                assert list(bp.source_file(root, 'local:' + directory).glob(pattern)), value
            else: assert path.exists(), value
            count += 1
        elif isinstance(value, dict):
            for item in value.values(): check(item)
        elif isinstance(value, list):
            for item in value: check(item)
    check(json.loads(manifest.read_text()))
layout = json.loads((root / 'source-layout-migration.json').read_text())
for link, target in ([] if args.recipes_only else layout['dependency_links'].items()):
    assert (root / link).is_symlink() and (root / link).is_dir(), link
    assert (root / link).resolve() == (root / target).resolve(), link
assert not list((root / 'apps').glob('*/variants/legacy'))
print(f'Validated {count} local recipe inputs and {0 if args.recipes_only else len(layout["dependency_links"])} dependency links; no reference variants under apps')
