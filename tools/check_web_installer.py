#!/usr/bin/env python3
"""Verify deployable firmware manifests, downloads and archive contents."""
from pathlib import Path
import hashlib,json,sys,zipfile
root=Path(sys.argv[1]);info=json.loads((root/'build-info.json').read_text());boards=json.loads((root/'boards.json').read_text());keys={p[0] for b in boards for p in b['profiles']};assert keys=={x['board'] for x in info['firmware']}
assert {b['manufacturer'] for b in boards} == {'Elecrow','Waveshare','M5Stack','Guition','Espressif','Olimex','DFRobot'}
assert len({b['id'] for b in boards}) == len(boards)
assert all(b['manufacturer'] == 'Waveshare' for b in boards if b['id'] in ('nano','waveshare7b','waveshare'))
for entry in info['firmware']:
 key=entry['board'];manifest_path=root/'manifests'/(key+'.json');m=json.loads(manifest_path.read_text());assert len(m['builds'])==1;build=m['builds'][0];assert build['chipFamily']=='ESP32-P4';assert len(build['parts'])==1;part=build['parts'][0];assert part['offset']==0
 binary=(manifest_path.parent/part['path']).resolve();assert root.resolve() in binary.parents;data=binary.read_bytes();assert len(data)==entry['bytes'];assert hashlib.sha256(data).hexdigest()==entry['sha256'];assert data[:0x2000] == b'\xff'*0x2000 and data[0x2000]==0xe9 and data[0x10000]==0xe9,'Invalid P4 merged image layout'
 with zipfile.ZipFile(root/'kits'/('pax-'+key+'-flash-kit.zip')) as z:
  assert z.testzip() is None;assert z.read(binary.name)==data
  assert 'roms/pax/launcher_ui.pax' in z.namelist();assert not any(Path(n).name=='secrets.yaml' for n in z.namelist())
with zipfile.ZipFile(root/'sd/pax-sd-starter.zip') as z:
 assert z.testzip() is None
 for name in ('launcher_ui','recipe'):
  for suffix in ('.pax','.json'):assert z.read('roms/pax/'+name+suffix)==(root/'sd'/(name+suffix)).read_bytes()
for board in boards:
 for key,relative in board.get('starters',{}).items():
  assert key in {profile[0] for profile in board['profiles']}
  archive=(root/relative).resolve();assert root.resolve() in archive.parents
  with zipfile.ZipFile(archive) as z:
   assert z.testzip() is None
   for name in ('launcher_ui','recipe'):
    for suffix in ('.pax','.json'):assert z.read('roms/pax/'+name+suffix)==(archive.parent/(name+suffix)).read_bytes()
print('Validated '+str(len(keys))+' ESP32-P4 manifests, firmware hashes, flash kits and SD files')
