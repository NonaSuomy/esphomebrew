#!/usr/bin/env python3
"""Losslessly deduplicate private terrain tiles; retain every original frame ID."""
import argparse
from pathlib import Path
import struct
from PIL import Image


def pack_terrain(graphics):
    packed = bytearray(b"PAPTILE1" + struct.pack("<I", 2))
    for name in ("badlands", "platform"):
        path = f"tilesets/{name}/{name}.png"
        with Image.open(graphics / path) as original:
            # DefineTileset declares 32x32 frames. Subtile deduplication would
            # preserve pixels but change frame IDs and break engine sampling.
            tile_size, columns = 32, 8
            assert original.mode == "P" and original.width % tile_size == original.height % tile_size == 0
            tiles, indices, lookup = [], {}, []
            for y in range(0, original.height, tile_size):
                for x in range(0, original.width, tile_size):
                    tile = original.crop((x, y, x+tile_size, y+tile_size)).tobytes()
                    if tile not in indices:
                        indices[tile] = len(tiles)
                        tiles.append(tile)
                    lookup.append(indices[tile])
            assert len(tiles) < 65536
            width, height = 256, ((len(tiles)+columns-1)//columns)*tile_size
            pixels = bytearray(width*height)
            for i, tile in enumerate(tiles):
                x, y = (i % columns)*tile_size, (i//columns)*tile_size
                for row in range(tile_size):
                    pixels[(y+row)*width+x:(y+row)*width+x+tile_size] = tile[row*tile_size:(row+1)*tile_size]
            # Verify every tile, not just a sample, before generating the pack.
            for frame, compact in enumerate(lookup):
                x, y = (compact % columns)*tile_size, (compact//columns)*tile_size
                rebuilt = b"".join(pixels[(y+r)*width+x:(y+r)*width+x+tile_size] for r in range(tile_size))
                ox, oy = (frame % (original.width//tile_size))*tile_size, (frame//(original.width//tile_size))*tile_size
                assert rebuilt == original.crop((ox, oy, ox+tile_size, oy+tile_size)).tobytes()
            rgb = original.getpalette("RGB")
            transparency = original.info.get("transparency")
            def alpha(i):
                if isinstance(transparency, bytes):
                    return transparency[i] if i < len(transparency) else 255
                return 0 if i == transparency else 255
            palette = b"".join(bytes(rgb[i*3:i*3+3]) + bytes((alpha(i),)) for i in range(256))
            payload = (struct.pack("<HHIHH", width, height, len(lookup), 256, tile_size) + palette +
                       struct.pack("<"+"H"*len(lookup), *lookup) + pixels)
            encoded = path.encode()
            packed += struct.pack("<HI", len(encoded), len(payload)) + encoded + payload
            print(f"Terrain {name}: {len(lookup)} frames, {len(tiles)} unique, {len(payload)} bytes; every tile verified", flush=True)
    return packed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(pack_terrain(args.graphics))
