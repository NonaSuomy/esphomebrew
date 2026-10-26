#!/usr/bin/env python3
"""Run the real BLE Remote LVGL UI against deterministic touch/BLE services."""
from pathlib import Path
import subprocess,concurrent.futures
module_root=Path(__file__).resolve().parents[1]
root=module_root.parent
app=module_root/'apps/ble_remote'
lv=module_root/'apps/_shared/lvgl'
sdk=root/'pax-esphome/components/pax_loader'
if not sdk.exists(): sdk=module_root/'esphome/components/pax_loader'
out=Path('/tmp/pax-ble-remote/host');out.mkdir(parents=True,exist_ok=True)
flags=['-O1','-g','-DLV_CONF_INCLUDE_SIMPLE=1','-DPAX_APP_SIDE=1','-Wno-cpp',f'-I{app}',f'-I{lv}',f'-I{sdk}']
sources=[s for s in (lv/'src').rglob('*.c') if 'drivers' not in s.relative_to(lv/'src').parts]
# Test includes the app so its real LVGL pointer callbacks and UI state are exercised.
test=module_root/'tests/ble_remote_ui.c'
sources.append(test)
def build(s):
 obj=out/(str(s.relative_to(lv) if s.is_relative_to(lv) else s.name).replace('/','_')+'.o')
 if not obj.exists() or obj.stat().st_mtime<max(s.stat().st_mtime,(app/'main.c').stat().st_mtime,(app/'lv_conf.h').stat().st_mtime,(sdk/'psram_app.h').stat().st_mtime):
  subprocess.run(['cc',*flags,'-c',str(s),'-o',str(obj)],check=True)
 return str(obj)
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:objects=list(pool.map(build,sources))
subprocess.run(['cc',*objects,'-lm','-o',str(out/'test_app')],check=True)
subprocess.run([str(out/'test_app')],check=True)
