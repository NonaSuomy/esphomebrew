#!/usr/bin/env python3
"""Convert the user's extracted Stargus intro/MNG artwork for the PAX.

Generated data is private game content, never a redistributable source asset.
MNG is coalesced on the host; the P4 inflates indexed stills or RGB565
keyframes/XOR deltas. The original engine caps zero-tick MNGs at 15 FPS
(mng.cpp).
"""
import argparse
from array import array
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib
from PIL import Image


def indexed(image, black_key=False):
    colors, indices = {}, bytearray()
    for r, g, b, a in image.convert("RGBA").get_flattened_data():
        color = (r, g, b, 0 if black_key and not (r or g or b) else a)
        if color not in colors:
            if len(colors) == 256:
                raise ValueError("Menu artwork exceeds lossless indexed palette")
            colors[color] = len(colors)
        indices.append(colors[color])
    palette = b"".join(bytes(c) for c in colors)
    return b"\x01" + struct.pack("<H", len(colors)) + zlib.compress(palette + indices, 9)


def rgb565_bytes(raw):
    pixels = array("H", (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                          for r, g, b in zip(raw[0::3], raw[1::3], raw[2::3])))
    if sys.byteorder != "little":
        pixels.byteswap()
    return pixels.tobytes()


def delta_frames(raw, w, h, transparent=False, independent=False):
    size = w * h * 3
    previous, frames = None, []
    for i, start in enumerate(range(0, len(raw), size)):
        pixels = rgb565_bytes(raw[start:start+size])
        if independent or previous is None or i % 15 == 0:
            frame = bytes((3 if transparent else 2,)) + zlib.compress(pixels, 9)
        else:
            delta = bytes(a ^ b for a, b in zip(previous, pixels))
            frame = bytes((4 if transparent else 5,)) + zlib.compress(delta, 9)
        frames.append(frame)
        previous = pixels
    return frames


def asset(w, h, frames, milliseconds):
    offset, table = 0, bytearray()
    for frame in frames:
        table += struct.pack("<I", offset)
        offset += len(frame)
    table += struct.pack("<I", offset)
    return struct.pack("<HHHH", w, h, len(frames), milliseconds) + table + b"".join(frames)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    sources = [args.data / "videos/blizzard.ogv"]
    sources += [args.data / "graphics" / name for name in (
        "ui/Menu background without title.png", "ui/title.png", "ui/readyt/butterr.png",
        "ui/fonts/font16x.png", "ui/fonts/font10.png", "ui/cursors/arrow.png", "ui/panels/264x288.png", "ui/panels/384x256.png",
        "ui/terran/agg/button_large.png", "ui/terran/agg/button_large_pressed.png",
        "terran/victory screen.png", "terran/defeat screen.png")]
    sources += [args.data / "videos" / name for name in (
        "mainmenu/single.mng", "mainmenu/singleon.mng", "mainmenu/multi.mng",
        "mainmenu/multion.mng", "mainmenu/exit.mng", "mainmenu/exiton.mng",
        "campaign/disk.mng", "campaign/terr.mng", "campaign/terron.mng",
        "campaign/prot.mng", "campaign/proton.mng", "campaign/zerg.mng", "campaign/zergon.mng",
        "portrait/tadvisor/tadfid00.mng", "portrait/tmarine/tmafid00.mng",
        "portrait/tscv/tscfid00.mng", "portrait/ujim/urafid00.mng", "portrait/zzergl/zzefid00.mng")]
    manifest = args.output.with_suffix(".json")
    fingerprint = {str(p): [p.stat().st_size, p.stat().st_mtime_ns] for p in sources}
    fingerprint["__generator__"] = Path(__file__).stat().st_mtime_ns
    if (args.output.exists() and args.output.with_name("stargus_intro.ogg").exists()
            and manifest.exists() and json.loads(manifest.read_text()) == fingerprint):
        print(f"Reusing private Stargus menu pack: {args.output.stat().st_size} bytes", flush=True)
        return
    entries = {}
    for source in sources:
        name = str(source.relative_to(args.data))
        print(f"Converting Stargus menu media: {name}", flush=True)
        if source.suffix == ".ogv":
            w, h = 320, 308
            raw = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(source),
                                           "-an", "-vf", f"fps=15,scale={w}:{h}",
                                           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"])
            size = w * h * 3
            assert len(raw) % size == 0
            frames = delta_frames(raw, w, h, independent=True)
            # The extracted Blizzard movie is 15 FPS. Do not discard every
            # fifth frame for the PAX; 67 ms is the pack's integer-ms cadence.
            entries[name] = asset(w, h, frames, 67 | 0x8000)
            audio = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(source), "-vn",
                                             "-ar", "22050", "-ac", "2", "-c:a", "libvorbis",
                                             "-q:a", "3", "-f", "ogg", "-"])
            args.output.with_name("stargus_intro.ogg").write_bytes(audio)
        elif source.suffix == ".mng":
            dimensions = subprocess.check_output(["magick", "identify", "-format", "%w %h",
                                                  str(source) + "[0]"], text=True)
            w, h = map(int, dimensions.split())
            # Match native Mng's RGB/black color-key surface. Keep a stable
            # RGB565 palette representation across frames, then compress
            # XOR deltas; palette-index renumbering would hide redundancy.
            raw = subprocess.check_output(["magick", str(source), "-coalesce", "-background", "black",
                                           "-alpha", "remove", "-depth", "8", "rgb:-"])
            size = w * h * 3
            assert len(raw) % size == 0
            independent = name.startswith("videos/campaign/")
            frames = delta_frames(raw, w, h, True, independent=independent)
            entries[name] = asset(w, h, frames, 67 | (0x8000 if independent else 0))
        else:
            with Image.open(source) as image:
                if name.endswith("arrow.png"):
                    image = image.crop((48, 48, 80, 80))
                entries[name] = asset(*image.size, [indexed(image)], 67)
    pack = bytearray(b"PAPMENU1" + struct.pack("<I", len(entries)))
    for name, payload in entries.items():
        encoded = name.encode()
        pack += struct.pack("<HI", len(encoded), len(payload)) + encoded + payload
    args.output.write_bytes(pack)
    manifest.write_text(json.dumps(fingerprint, sort_keys=True))
    print(f"Packed {len(entries)} original menu/intro assets: {len(pack)} bytes", flush=True)


if __name__ == "__main__":
    main()
