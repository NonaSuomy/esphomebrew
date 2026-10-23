#!/usr/bin/env python3
"""Build public PAX firmware and promote only complete, matching matrix results."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
_spec = importlib.util.spec_from_file_location('firmware_config', Path(__file__).with_name('firmware-config.py'))
_config = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_config)
public_configuration = _config.public_configuration

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'esphome'
CI = ROOT / 'tools/ci'
WORK = ROOT / '.firmware-ci'
STARTER_FILES = ('launcher_ui.pax', 'launcher_ui.json', 'recipe.pax', 'recipe.json',
                 'backgrounds/bluegamingbackground1024x600.png')

def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def load(path):
    return json.loads(Path(path).read_text())

def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')

def profiles():
    result = load(FW / 'installer-profiles.json')
    boards = load(ROOT / 'web-installer/boards.json')
    advertised = [p[0] for board in boards for p in board['profiles']]
    if len(set(advertised)) != len(advertised) or set(result) != set(advertised):
        raise ValueError('Installer registry must match the advertised board profiles exactly')
    for key, entry in result.items():
        if not re.fullmatch(r'[a-z0-9-]+', key):
            raise ValueError('Invalid profile key')
        public_configuration(FW / entry)
    return result

def input_digest():
    """Hash tracked build inputs, excluding mutable download products and caches."""
    names = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z', '--',
        'esphome', 'tools/ci', 'tools/firmware_ci.py', 'tools/firmware-config.py',
        'web-installer/boards.json', 'web-installer/sd'], text=True).split('\0')
    h = hashlib.sha256()
    for name in sorted(n for n in names if n):
        p = ROOT / name
        if p.is_symlink() or not p.is_file():
            raise ValueError('Build input is not a regular file: ' + name)
        h.update(name.encode() + b'\0' + p.read_bytes() + b'\0')
    return h.hexdigest()

def bootstrap():
    spec = load(CI / 'firmware-sources.json')
    stamp = WORK / 'sources.json'
    receipts = {}
    for name, destination in [('esphome', FW / 'dependencies/esphome'),
                              ('esp_idf', WORK / 'esp-idf')]:
        item = spec[name]
        patch = CI / item['patch']
        if sha(patch.read_bytes()) != item['patch_sha256']:
            raise ValueError('Dependency patch hash mismatch: ' + name)
        receipt = {'base': item['base'], 'patch_sha256': item['patch_sha256']}
        receipts[name] = receipt
        if destination.exists():
            if not stamp.is_file() or load(stamp).get(name) != receipt:
                raise ValueError('Refusing mismatched dependency cache: ' + name)
            actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
            if actual != item['base']:
                raise ValueError('Dependency cache base mismatch: ' + name)
            run('git', '-C', destination, 'apply', '--reverse', '--check', patch)
            continue
        destination.mkdir(parents=True)
        run('git', 'init', '-q', destination)
        run('git', '-C', destination, 'remote', 'add', 'origin', item['repo'])
        run('git', '-C', destination, 'fetch', '--depth=1', 'origin', item['base'])
        run('git', '-C', destination, 'checkout', '--detach', 'FETCH_HEAD')
        if name == 'esp_idf':
            run('git', '-C', destination, 'submodule', 'update', '--init', '--recursive', '--depth=1', '--jobs=4')
        run('git', '-C', destination, 'apply', '--check', patch)
        run('git', '-C', destination, 'apply', patch)
        if name == 'esp_idf':
            (destination / 'version.txt').write_text('v' + item['version'] + '\n')
        # Write after each dependency so an interrupted second checkout remains clear.
        write(stamp, receipts)
    print('Pinned ESPHome and ESP-IDF sources ready')

def environment():
    return dict(PYTHONPATH=str(FW / 'dependencies/esphome'),
                PAX_IDF_SOURCE='symlink://' + str(WORK / 'esp-idf'),
                PAX_IDF_COMPAT='pre:' + str(FW / 'tools/idf61_compat.py'),
                PLATFORMIO_CORE_DIR=str(WORK / 'platformio'),
                IDF_TOOLS_PATH=str(WORK / 'idf-tools'),
                ESPHOME_DEFAULT_COMPILE_PROCESS_LIMIT='2')

def build(profile):
    registry = profiles()
    if profile not in registry:
        raise ValueError('Unknown public profile: ' + profile)
    env = os.environ.copy()
    env.update(environment())
    run(sys.executable, '-m', 'esphome', 'compile', registry[profile], cwd=FW, env=env)
    package(profile)

def starter_root():
    pins = (FW / 'components/pax_web/pax_web_starter.h').read_text()
    match = re.search(r'https://nonasuomy.github.io/esphomebrew/install/(sd/[^" ]*)/launcher_ui.pax', pins)
    if not match:
        raise ValueError('Missing immutable starter URL')
    relative = Path(match[1])
    if relative.is_absolute() or '..' in relative.parts:
        raise ValueError('Invalid starter path')
    folder = ROOT / 'web-installer' / relative
    for filename in STARTER_FILES:
        if sha((folder / filename).read_bytes()) not in pins:
            raise ValueError('Starter hash mismatch: ' + filename)
    return folder

def package(profile):
    registry = profiles()
    yaml = registry[profile]
    sources = public_configuration(FW / yaml)
    node = re.search(r'^  device_name: (\S+)', (FW / yaml).read_text(), re.M).group(1)
    build_dir = FW / '.esphome/build' / node
    binary_dir = build_dir / '.pioenvs' / node
    factory = (binary_dir / 'firmware.factory.bin').read_bytes()
    ota = (binary_dir / 'firmware.ota.bin').read_bytes()
    # A staging operation must not silently package a stale loader or board source.
    for file in (FW / 'components').rglob('*'):
        if file.is_file() and file.suffix in {'.h', '.cpp', '.c'}:
            relative = file.relative_to(FW / 'components')
            generated = build_dir / 'src/esphome/components' / relative
            if generated.exists() and generated.read_bytes() != file.read_bytes():
                raise ValueError('Generated component differs from source: ' + str(relative))
    if len(factory) < 0x10001 or factory[:0x2000] != b'\xff' * 0x2000 or factory[0x2000] != 0xe9 or factory[0x10000] != 0xe9:
        raise ValueError('Invalid merged ESP32-P4 image')
    out = ROOT / 'firmware-dist' / profile
    if out.exists():
        shutil.rmtree(out)
    for name in ['firmware', 'ota', 'kits', 'manifests', 'records']:
        (out / name).mkdir(parents=True)
    factory_name = 'pax-' + profile + '.factory.bin'
    ota_name = 'pax-' + profile + '.ota.bin'
    (out / 'firmware' / factory_name).write_bytes(factory)
    (out / 'ota' / ota_name).write_bytes(ota)
    now = datetime.now(timezone.utc).isoformat()
    manifest = {'name': 'PAX ' + profile, 'version': now[:10].replace('-', '.'),
        'new_install_prompt_erase': True, 'new_install_improv_wait_time': 60,
        'builds': [{'chipFamily': 'ESP32-P4', 'parts': [{'path': '../firmware/' + factory_name, 'offset': 0}]}]}
    write(out / 'manifests' / (profile + '.json'), manifest)
    starter = starter_root()
    with zipfile.ZipFile(out / 'kits' / ('pax-' + profile + '-flash-kit.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(factory_name, factory)
        z.writestr(ota_name, ota)
        z.writestr(factory_name + '.sha256', sha(factory) + '  ' + factory_name + '\n')
        z.writestr(ota_name + '.sha256', sha(ota) + '  ' + ota_name + '\n')
        z.writestr('README.txt', 'PAX ' + profile + '\nCheck your board and chip revision before flashing.\n'
            'Factory install: python -m esptool --chip esp32p4 --port YOUR_PORT write-flash 0x0 ' + factory_name + '\n'
            'The OTA image is for an existing compatible PAX installation.\n'
            'Copy roms/ and backgrounds/ to the SD card root, then choose STORAGE.\n'
            'Public firmware contains no saved station credentials. Provision Wi-Fi or use supported Ethernet.\n'
            'Compilation is verified by CI; hardware validation is separate.\n')
        for filename in STARTER_FILES:
            destination = filename if filename.startswith('backgrounds/') else 'roms/pax/' + filename
            z.write(starter / filename, destination)
        for file in sources:
            z.write(file, 'source-reference/' + file.relative_to(FW).as_posix())
        for folder in ['components', 'patches', 'tools']:
            for file in (FW / folder).rglob('*'):
                if file.is_file() and file.suffix in {'.py', '.h', '.cpp', '.c', '.patch'}:
                    z.write(file, 'source-reference/' + file.relative_to(FW).as_posix())
        for file in FW.glob('*.h'):
            z.write(file, 'source-reference/' + file.name)
    record = next(x for x in load(ROOT / 'web-installer/build-info.json')['firmware'] if x['board'] == profile)
    # Preserve previous hardware test evidence under its original image, not on this new image.
    historical = record.get('historical_hardware_validation', {})
    historical.update({k: record[k] for k in ['hardware_validation', 'runtime_validation'] if k in record})
    for field in ['hardware_validation', 'runtime_validation', 'repositories']:
        record.pop(field, None)
    record.update(built_at=now[:10], build_timestamp=now, yaml=yaml, bytes=len(factory), sha256=sha(factory),
        hardware_tested=False, sd_transfer_tested=False, serial_sd_transfer_tested=False,
        source_commit=os.environ.get('GITHUB_SHA', ''), input_digest=input_digest(),
        ci_run_id=os.environ.get('GITHUB_RUN_ID', ''),
        ota={'path': 'ota/' + ota_name, 'bytes': len(ota), 'sha256': sha(ota)},
        historical_hardware_validation=historical)
    sdkconfig = (build_dir / ('sdkconfig.' + node)).read_text()
    clock = re.search(r'^CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=(\d+)$', sdkconfig, re.M)
    if clock is None:
        raise ValueError('Compile before packaging: SDK configuration is incomplete')
    mhz = int(clock.group(1))
    if mhz == 400 and not re.search(r'^CONFIG_ESP32P4_REV_MIN_300=y$', sdkconfig, re.M):
        raise ValueError('400 MHz requires revision 3')
    record['cpu_frequency_mhz'] = mhz
    write(out / 'records' / (profile + '.json'), record)
    print('Packaged', profile, sha(factory))

def collect(artifacts, output):
    """Validate the entire matrix before producing any publishable overlay."""
    artifacts, output = Path(artifacts), Path(output)
    registry = profiles()
    digest = input_digest()
    found = {}
    for path in artifacts.rglob('records/*.json'):
        record = load(path)
        key = record['board']
        if key not in registry or key in found or record['input_digest'] != digest:
            raise ValueError('Duplicate, unexpected or stale profile: ' + key)
        base = path.parent.parent
        for relative, expected in [('firmware/pax-' + key + '.factory.bin', record['sha256']),
                                   (record['ota']['path'], record['ota']['sha256'])]:
            if sha((base / relative).read_bytes()) != expected:
                raise ValueError('Firmware hash mismatch: ' + key)
        with zipfile.ZipFile(base / 'kits' / ('pax-' + key + '-flash-kit.zip')) as z:
            if z.testzip() is not None or z.read('pax-' + key + '.factory.bin') != (base / 'firmware' / ('pax-' + key + '.factory.bin')).read_bytes():
                raise ValueError('Invalid flash kit: ' + key)
        found[key] = (record, base)
    if set(found) != set(registry):
        raise ValueError('Missing matrix profiles: ' + ', '.join(sorted(set(registry) - set(found))))
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    info = load(ROOT / 'web-installer/build-info.json')
    info['built_at'] = datetime.now(timezone.utc).date().isoformat()
    info['firmware'] = [found[key][0] for key in registry]
    info['ci'] = {'input_digest': digest, 'source_commit': os.environ.get('GITHUB_SHA', ''),
                  'run_id': os.environ.get('GITHUB_RUN_ID', ''), 'profiles': len(registry)}
    for record, base in found.values():
        for folder in ['firmware', 'ota', 'kits', 'manifests']:
            shutil.copytree(base / folder, output / folder, dirs_exist_ok=True)
    write(output / 'build-info.json', info)
    print('Collected complete matrix:', len(found), 'profiles')

def promote(overlay):
    """Called on latest main; never install an artifact for changed build inputs."""
    overlay = Path(overlay)
    info = load(overlay / 'build-info.json')
    if info['ci']['input_digest'] != input_digest():
        raise ValueError('Firmware inputs changed since this matrix build; refusing publication')
    if {x['board'] for x in info['firmware']} != set(profiles()):
        raise ValueError('Incomplete matrix overlay')
    for record in info['firmware']:
        for path, expected in [('firmware/pax-' + record['board'] + '.factory.bin', record['sha256']),
                               (record['ota']['path'], record['ota']['sha256'])]:
            if sha((overlay / path).read_bytes()) != expected:
                raise ValueError('Promotion hash mismatch')
    for folder in ['firmware', 'ota', 'kits', 'manifests']:
        shutil.copytree(overlay / folder, ROOT / 'web-installer' / folder, dirs_exist_ok=True)
    shutil.copy2(overlay / 'build-info.json', ROOT / 'web-installer/build-info.json')
    run(sys.executable, ROOT / 'tools/check_web_installer.py', ROOT / 'web-installer')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ['matrix', 'bootstrap', 'environment', 'digest']:
        sub.add_parser(name)
    for name in ['build', 'package']:
        sub.add_parser(name).add_argument('profile')
    p = sub.add_parser('collect'); p.add_argument('artifacts'); p.add_argument('output')
    sub.add_parser('promote').add_argument('overlay')
    args = parser.parse_args()
    if args.command == 'matrix':
        print(json.dumps({'include': [{'profile': k, 'yaml': v} for k, v in profiles().items()]}))
    elif args.command == 'bootstrap': bootstrap()
    elif args.command == 'environment':
        for key, value in environment().items(): print(key + '=' + value)
    elif args.command == 'digest': print(input_digest())
    elif args.command == 'build': build(args.profile)
    elif args.command == 'package': package(args.profile)
    elif args.command == 'collect': collect(args.artifacts, args.output)
    elif args.command == 'promote': promote(args.overlay)

if __name__ == '__main__':
    main()
