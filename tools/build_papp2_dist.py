#!/usr/bin/env python3
"""Validate staged PAPP2 release packages and assemble the Store build artifact.

The source tree keeps the verified PAPP2 packages and their release metadata in
``papp2-packages/``. This script deliberately rejects ABI-1 packages so a future
Store build cannot silently put them back into the catalog.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ROOT / "papp2-packages"
PAPP2_MAGIC = 0x32504150  # "PAP2"
PAPP2_HEADER_SIZE = 64
HEADER = struct.Struct("<IHHIIIIIIIIIIII8x")


def validate_package(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < PAPP2_HEADER_SIZE:
        raise ValueError(f"{path.name}: shorter than a PAPP2 header")
    (magic, fmt, header_size, flags, image_size, bss_size, entry, imports_off,
     imports_count, manifest_off, reloc_count, image_crc, reloc_crc,
     fast_size, fast_bss) = HEADER.unpack_from(data)
    if magic != PAPP2_MAGIC or fmt != 1 or header_size != PAPP2_HEADER_SIZE or flags != 0:
        raise ValueError(f"{path.name}: invalid PAPP2 header")
    expected_size = header_size + image_size + fast_size + reloc_count * 8
    if expected_size != len(data):
        raise ValueError(f"{path.name}: expected {expected_size} bytes, found {len(data)}")
    image_start = header_size
    fast_start = image_start + image_size
    reloc_start = fast_start + fast_size
    image = data[image_start:fast_start]
    fast = data[fast_start:reloc_start]
    relocs = data[reloc_start:]
    if zlib.crc32(fast, zlib.crc32(image)) & 0xFFFFFFFF != image_crc:
        raise ValueError(f"{path.name}: image CRC mismatch")
    if zlib.crc32(relocs) & 0xFFFFFFFF != reloc_crc:
        raise ValueError(f"{path.name}: relocation CRC mismatch")
    if image_size == 0 or entry >= image_size + bss_size:
        raise ValueError(f"{path.name}: invalid image or entry point")
    if imports_count and imports_off + imports_count * 12 > image_size:
        raise ValueError(f"{path.name}: import table exceeds the image")
    if manifest_off >= image_size:
        raise ValueError(f"{path.name}: manifest offset exceeds the image")


def normalize_icon(app: dict) -> None:
    icon = app.get("icon")
    if not isinstance(icon, str):
        return
    app_dir = ROOT / "apps" / app["name"]
    path = (app_dir / icon).resolve()
    if app_dir.resolve() not in path.parents or not path.is_file():
        raise ValueError(f"{app['name']}: icon must be a PNG inside apps/{app['name']}/")
    data = path.read_bytes()
    if len(data) > 64 * 1024 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError(f"{app['name']}: icon must be a PNG no larger than 64 KB")
    width, height = struct.unpack_from(">II", data, 16)
    if not (0 < width <= 256 and 0 < height <= 256):
        raise ValueError(f"{app['name']}: icon dimensions {width}x{height} exceed 256x256")
    app["icon"] = {"type": "image/png", "width": width, "height": height,
                   "base64": base64.b64encode(data).decode("ascii")}


def build_dist(packages: Path, out: Path) -> list[dict]:
    out.mkdir(parents=True, exist_ok=True)
    apps: list[dict] = []
    seen: set[str] = set()
    for sidecar_path in sorted(packages.glob("*.json")):
        app = json.loads(sidecar_path.read_text())
        name = app.get("name")
        filename = app.get("file")
        if not isinstance(name, str) or not name or name in seen:
            raise ValueError(f"{sidecar_path.name}: missing or duplicate app name")
        if filename != f"{name}.pax":
            raise ValueError(f"{sidecar_path.name}: release file must be {name}.pax")
        package = packages / filename
        if not package.is_file():
            raise ValueError(f"{sidecar_path.name}: missing {filename}")
        if app.get("abi") != 2:
            raise ValueError(f"{filename}: Store packages must use ABI 2")
        if not app.get("version") or not app.get("title") or not isinstance(app.get("source"), dict):
            raise ValueError(f"{sidecar_path.name}: missing version, title or source metadata")
        validate_package(package)
        normalize_icon(app)
        payload = package.read_bytes()
        app["size"] = len(payload)
        app["sha256"] = hashlib.sha256(payload).hexdigest()
        shutil.copyfile(package, out / filename)
        apps.append(app)
        seen.add(name)
    if not apps:
        raise ValueError(f"no PAPP2 packages found in {packages}")
    (out / "build.json").write_text(json.dumps({"apps": apps}, indent=2) + "\n")
    return apps


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packages", type=Path, default=PACKAGES)
    parser.add_argument("--out", type=Path, default=Path("dist"))
    args = parser.parse_args()
    try:
        apps = build_dist(args.packages, args.out)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"PAPP2 Store build failed: {error}", file=sys.stderr)
        return 1
    print(f"Validated and staged {len(apps)} ABI-2 PAX packages")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
