#!/usr/bin/env python3
"""Pack private, extracted sprites in PSRAM-friendly indexed form.

No quantization: RGBA inputs must fit in a palette with player-color slots reserved. The
compact walking sheets keep eight directions (five plus native mirroring).
Never distribute this generated pack: it contains the user's game assets.
"""
import argparse
from pathlib import Path
import struct
from PIL import Image


def indexed(image):
    if image.mode == "P":
        rgb = image.getpalette("RGB")
        colors = bytearray()
        transparency = image.info.get("transparency", 0)
        for i in range(256):
            alpha = (transparency[i] if i < len(transparency) else 255) if isinstance(transparency, bytes) else (0 if i == transparency else 255)
            colors += bytes(rgb[i*3:i*3+3]) + bytes((alpha,))
        return struct.pack("<HHH", *image.size, 256) + colors + image.tobytes()
    rgba = image.convert("RGBA")
    palette = {}
    data = bytearray()
    # Native CPlayerColorGraphic remaps slots 8..15. Original RGBA command
    # artwork has no player-color indices, so leave those slots unreferenced.
    slots = list(range(8)) + list(range(16, 256))
    for color in rgba.get_flattened_data():
        if color not in palette:
            if len(palette) == len(slots):
                raise ValueError("Asset exceeds the lossless 256-color limit")
            palette[color] = slots[len(palette)]
        data.append(palette[color])
    colors = bytearray(bytes((0, 0, 0, 255)) * 256)
    for color, slot in palette.items():
        colors[slot*4:slot*4+4] = bytes(color)
    return struct.pack("<HHH", *image.size, 256) + colors + data


def frames(image, size, bases):
    out = Image.new("P", (size * 5, size * len(bases)))
    out.putpalette(image.getpalette())
    out.info["transparency"] = image.info.get("transparency", 0)
    columns = image.width // size
    for y, base in enumerate(bases):
        for x, direction in enumerate((0, 4, 8, 12, 16)):
            n = base + direction
            sx, sy = n % columns * size, n // columns * size
            out.paste(image.crop((sx, sy, sx+size, sy+size)), (x*size, y*size))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    images = {name: Image.open(args.graphics / name)
              for name in ("cmdicons.png", "pcmdbtns.png")}
    # Match the health-icon remap in icons.cpp; loading the full desktop
    # wireframe sheets would consume the remaining game/audio headroom.
    for name, size in (("wirefram.png", 64), ("grpwire.png", 32), ("tranwire.png", 64)):
        source = Image.open(args.graphics / name)
        out = Image.new("P", (size, size * 7))
        out.putpalette(source.getpalette())
        out.info["transparency"] = source.info.get("transparency", 0)
        for y, n in enumerate((0, 7, 19, 37, 106, 109, 111)):
            sx, sy = n % (source.width // size) * size, n // (source.width // size) * size
            out.paste(source.crop((sx, sy, sx+size, sy+size)), (0, y*size))
        images[name] = out
    images["unit/terran/marine.png"] = frames(Image.open(args.graphics / "unit/terran/marine.png"),
                                           64, (68, 85, 102, 119, 136, 153, 170, 187, 204, 51, 34))
    images["unit/zerg/zergling.png"] = frames(Image.open(args.graphics / "unit/zerg/zergling.png"),
                                            128, (85, 102, 119, 136, 153, 17, 34))
    # Preserve SCV standing and two work rows; the vulture needs its standing
    # headings. Avoid allocating/flipping whole desktop sheets at runtime.
    for name, size in (("scv",72),("vulture",100)):
        images[f"unit/terran/{name}.png"] = frames(Image.open(args.graphics / f"unit/terran/{name}.png"),size,(0,17,34) if name=="scv" else (0,))
    images["unit/neutral/geyser.png"] = Image.open(args.graphics / "unit/neutral/geyser.png")
    # A single cropped pointer is shared by the native cursor roles. This
    # avoids loading >1 MiB of desktop cursor animations just to enable input.
    images["ui/cursors/pax.png"] = Image.open(args.graphics / "ui/cursors/arrow.png").crop((48, 48, 80, 80))
    # Buildings and the two shared construction sheets retain original frame
    # numbers. Read-only indexed pixels avoid new SD/mirror allocations when
    # an SCV builds a depot/barracks on the already loaded mission heap.
    for name in ("control", "depot", "tbarrack", "tbldlrg", "tbldmed"):
        images[f"unit/terran/{name}.png"] = Image.open(args.graphics / f"unit/terran/{name}.png")
    pack = bytearray(b"PAPASST1" + struct.pack("<I", len(images)))
    for name, image in images.items():
        payload = indexed(image)
        encoded = name.encode()
        pack += struct.pack("<HI", len(encoded), len(payload)) + encoded + payload
    args.output.write_bytes(pack)
    print(f"Packed {len(images)} lossless indexed runtime assets: {len(pack)} bytes", flush=True)


if __name__ == "__main__":
    main()
