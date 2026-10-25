#!/usr/bin/env python3
"""Prepare privately owned DOS Another World data for /sd/roms/anotherworld.

Accept original memlist.bin/bankXX files, or --from-headers to migrate the
legacy local embedded-data headers. Generated game files must not go in Git.
"""
import argparse
from pathlib import Path
import re


def header_bytes(path, symbol):
    text = path.read_text()
    match = re.search(r'\b' + re.escape(symbol) + r'\s*\[\s*\]\s*=\s*\{(.*?)\}', text, re.S)
    if not match:
        raise ValueError(f'Missing {symbol} in {path.name}')
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', match[1], flags=re.S)
    return bytes(int(token.strip(), 0) for token in body.split(',') if token.strip())


def validate(files):
    data = files.get('memlist.bin', b'')
    if not data or len(data) > 3000:
        raise ValueError('Missing or oversized DOS memlist.bin')
    for i in range(150):
        offset = i * 20
        if offset >= len(data):
            break
        if data[offset] == 255:
            if i:
                return
            break
        if len(data) - offset < 20:
            break
        row = data[offset:offset + 20]
        packed, size = int.from_bytes(row[14:16], 'big'), int.from_bytes(row[18:20], 'big')
        if row[1] > 6 or bool(packed) != bool(size) or packed > size:
            raise ValueError(f'Invalid resource entry {i}')
        if packed:
            bank = files.get(f'bank{row[7]:02x}')
            start = int.from_bytes(row[8:12], 'big')
            if bank is None or start + packed > len(bank):
                raise ValueError(f'Missing or truncated bank{row[7]:02x} for resource {i}')
    raise ValueError('Truncated DOS memlist or missing end marker')


def prepare(source, output, from_headers=False):
    files = {}
    if from_headers:
        files['memlist.bin'] = header_bytes(source / 'datamemlistbin.h', 'gb_memlist_bin')
        for path in source.glob('databank??.h'):
            bank = path.stem[-2:].lower()
            files['bank' + bank] = header_bytes(path, 'gb_bank' + bank + '_bin')
    else:
        for path in source.iterdir():
            name = path.name.lower()
            if path.is_file() and (name == 'memlist.bin' or re.fullmatch(r'bank[0-9a-f]{2}', name)):
                files[name] = path.read_bytes()
    validate(files)
    output.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        path = output / name
        if not path.exists() or path.read_bytes() != data:
            temporary = path.with_name(path.name + '.tmp')
            temporary.write_bytes(data)
            temporary.replace(path)
    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--from-headers', action='store_true')
    args = parser.parse_args()
    try:
        count = prepare(args.source, args.output, args.from_headers)
    except (ValueError, OSError) as error:
        parser.exit(1, f'Another World data: {error}\n')
    print(f'Prepared {count} private data files in {args.output}; copy to /sd/roms/anotherworld/')


if __name__ == '__main__':
    main()
