#!/usr/bin/env python3
"""Build the approved AIO C6 SPI target from an ESP-Hosted 2.12.12 component.

Activate ESP-IDF 5.5.2 first. Output is a candidate: approve its new SHA-256 in
c6-firmware.json before offering it to devices. No flashing is performed.
"""
import argparse, hashlib, json, re, shutil, subprocess
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--component',type=Path,required=True)
parser.add_argument('--work',type=Path,required=True)
parser.add_argument('--defaults',type=Path,required=True)
args=parser.parse_args()
source=args.component.resolve();work=args.work.resolve()
if work.exists():raise SystemExit('Use a fresh work directory')
receipt=source/'CHECKSUMS.json'
if hashlib.sha256(receipt.read_bytes()).hexdigest()!='b86d6f7152c0a3417bbc7295d5038db2e61b75cc4db3203dff06d869fcca7e79':
    raise SystemExit('Requires the pinned ESP-Hosted 2.12.12 component archive')
checks=json.loads(receipt.read_text())
for item in checks['files']:
    # ESPHome patches host-only SPI DMA code; it is not compiled into the slave.
    if item['path'].startswith('host/'):continue
    file=source/item['path']
    if not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest()!=item['hash']:
        raise SystemExit('Component checksum mismatch: '+item['path'])
version=(source/'slave/main/esp_hosted_coprocessor_fw_ver.h').read_text()
parts=re.findall(r'^#define PROJECT_VERSION_(?:MAJOR|MINOR|PATCH)_1\s+(\d+)$',version,re.M)
if parts!=['2','12','12']:raise SystemExit('Requires ESP-Hosted 2.12.12')
idf=subprocess.check_output(['idf.py','--version'],text=True)
if '5.5.2' not in idf:raise SystemExit('Activate ESP-IDF 5.5.2')
shutil.copytree(source,work)
project=work/'slave';shutil.copy2(args.defaults,project/'sdkconfig.defaults')
subprocess.run(['idf.py','-C',str(project),'set-target','esp32c6'],check=True)
subprocess.run(['idf.py','-C',str(project),'build'],check=True)
config=dict(re.findall(r'^(CONFIG_\w+)=(.+)$',(project/'sdkconfig').read_text(),re.M))
expected={'CONFIG_ESP_SPI_HOST_INTERFACE':'y','CONFIG_ESP_SPI_MODE':'3',
 **{'CONFIG_ESP_SPI_GPIO_'+k:str(v) for k,v in {'MOSI':20,'MISO':21,'CLK':18,'CS':19,'HANDSHAKE':23,'DATA_READY':22}.items()}}
if any(config.get(k)!=v for k,v in expected.items()) or config.get('CONFIG_ESP_SDIO_HOST_INTERFACE')=='y':
    raise SystemExit('Refusing incorrect effective transport or slave pins')
data=(project/'build/network_adapter.bin').read_bytes()
print(json.dumps({'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'effective_config':expected},indent=2))
