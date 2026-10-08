#!/usr/bin/env python3
"""Build the automatic New Game test without changing the app manifest."""
from pathlib import Path
import argparse
import shutil
import sys

root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / "tools"))
import build_pax as builder

original_units = builder.custom_units
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--death-check", action="store_true", help="Exercise death/load menus with isolated test save slots")
parser.add_argument("--video-layout", choices=("baseline", "sram-depth"),
                    help="Override the release buffer layout for an A/B experiment")
parser.add_argument("--name", default="quake2-smoke", help="Distinct output directory and served PAX name")
args = parser.parse_args()
if not args.name.startswith("quake2-") or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.name):
    parser.error("--name must be a quake2- prefixed lowercase filename stem")


def smoke_units(manifest, source, build_dir):
    units, links = original_units(manifest, source, build_dir)
    for unit in units:
        unit.flags.append("-DQUAKE2_TEST_NEWGAME=1")
        if args.death_check:
            unit.flags.append("-DQUAKE2_TEST_DEATH=1")
        if args.video_layout:
            unit.flags.append(f"-DQUAKE2_SRAM_ZBUFFER={int(args.video_layout == 'sram-depth')}")
    return units, links


builder.custom_units = smoke_units
out = root / ".cache" / args.name
builder.build_app(root / "apps/quake2/pax.json", root / ".cache", out, 8)
binary = out / "quake2.pax"
assert b"QUAKE2 TEST ENTER" in binary.read_bytes(), "test flag was not compiled"
shutil.copy2(binary, root / "dev-paxs" / f"{args.name}.pax")
print(f"Staged automatic New Game test: {args.name}.pax", flush=True)
