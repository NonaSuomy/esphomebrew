#!/usr/bin/env python3
"""Generate private SD companions from a user's converted Stargus data tree.

All generated sprite/audio/menu/terrain files stay outside the PAX executable.
Copy the complete converted tree (including media/) to /sd/roms/stargus/.
"""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('data', type=Path, help='Complete converted game tree')
    args = parser.parse_args()
    tools = Path(__file__).resolve().parent
    data = args.data.resolve()
    media = data / 'media'
    media.mkdir(parents=True, exist_ok=True)
    commands = [
        ['prepare_stargus_runtime_assets.py', data / 'graphics', media / 'runtime.paxassets'],
        ['prepare_stargus_menu.py', data, media / 'menu.paxmenu'],
        ['prepare_stargus_terrain.py', data / 'graphics', media / 'terrain.paxtile'],
    ]
    for tool, source, output in commands:
        subprocess.run([sys.executable, str(tools / tool), str(source), str(output)], check=True)
    print(f'Private SD media prepared in {media}; no assets were embedded in a PAX.')


if __name__ == '__main__':
    main()
