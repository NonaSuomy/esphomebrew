#!/usr/bin/env python3
"""Apply the PAX RGB565 patch to the Switch fork and exercise its pixel code."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
app = root / 'apps/netsurf/tests'
fork = root / 'upstream/netsurf-switch'
if not (fork / 'netsurf/frontends/framebuffer/fbfx.c').exists():
    raise SystemExit('Switch fork checkout is missing: ' + str(fork))
with tempfile.TemporaryDirectory(prefix='pax-fbfx-test-') as tmp:
    temp = Path(tmp)
    target = temp / 'frontends/framebuffer/fbfx.c'
    target.parent.mkdir(parents=True)
    shutil.copyfile(fork / 'netsurf/frontends/framebuffer/fbfx.c', target)
    subprocess.run(['patch', '--batch', '-p1', '-i', str(app / 'switch-rgb565.patch')], cwd=temp, check=True)
    dirs = [target.parent, fork/'netsurf', fork/'netsurf/include', fork/'netsurf/frontends',
            fork/'netsurf/content/handlers', fork/'libnsfb/include', fork/'libwapcaplet/include',
            fork/'libdom/include', fork/'libcss/include']
    exe = temp/'test'
    subprocess.run(['cc','-std=gnu11','-Os','-ffunction-sections','-fdata-sections',
                    *['-I'+str(p) for p in dirs], str(app/'fbfx_rgb565_test.c'),
                    '-Wl,--gc-sections','-lm','-o',str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('RGB565 pixel, gradient and layer tests passed')
