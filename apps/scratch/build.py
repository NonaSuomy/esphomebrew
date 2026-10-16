#!/usr/bin/env python3
"""Compile the PAX port without upstream's desktop-only dependency discovery."""
import os,sys,json,subprocess,concurrent.futures,hashlib,struct
from pathlib import Path
root=Path(__file__).resolve().parent
workspace=Path(os.environ['PAX_ROOT']) if 'PAX_ROOT' in os.environ else root.parents[2]
input_test='--input-test' in sys.argv
host='--host' in sys.argv or input_test
out=root/('build-host' if host else 'build-pax');out.mkdir(exist_ok=True)
paths=['port','core/include','core/include/runtime','core/include/runtime/parser','core/include/runtime/blocks','core/include/platforms','core/include/menus','core/include/renderers/headless','core/source','core/external-include','deps/json','deps/expected/include','deps/miniz','deps/ryu','deps/lunasvg/include','deps/lunasvg/plutovg/include','deps/stb','deps/dr_libs','deps/tsf']
units=[]
for folder in ['core/source','core/source/runtime','core/source/runtime/blocks','core/source/runtime/parser','core/source/runtime/zip/miniz','port','deps/miniz','deps/ryu/ryu','deps/lunasvg/source','deps/lunasvg/plutovg/source']:
 for p in sorted((root/folder).glob('*')):
  if p.suffix not in ['.c','.cpp']:continue
  if folder=='core/source' and p.name in ['main.cpp','libretro.cpp','log.cpp','filesystem.cpp','inspector.cpp']:continue
  if p.name=='pax_syscalls.c' and host:continue
  units.append(p)
units.append(root/'core/source/runtime/time.cpp') if root/'core/source/runtime/time.cpp' not in units else None
# Vorbis implementation is a distinct translation unit.
units.append(root/'port/vorbis.c') if root/'port/vorbis.c' not in units else None
includes=['-I'+str(root/p) for p in paths]+['-I'+str(workspace/'pax-esphome/components/pax_loader')]
flags=['-Os','-ffunction-sections','-fdata-sections','-fno-tree-loop-distribute-patterns','-DRENDERER_HEADLESS','-DENABLE_BITMAP','-DENABLE_SVG','-DENABLE_AUDIO','-DSTBI_NO_THREAD_LOCALS','-DJSON_NOEXCEPTION','-DMINIZ_NO_TIME','-DLUNASVG_BUILD_STATIC','-DPLUTOVG_BUILD_STATIC','-DLUNASVG_DISABLE_LOAD_SYSTEM_FONTS','-DLUNASVG_DISABLE_EXTERNAL_RESOURCES','-DPLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD','-DPAX_APP_SIDE=1','-DENABLE_CLOUDVARS']
arch=[] if host else ['-march=rv32imafc_zicsr_zifencei','-mabi=ilp32f','-mcmodel=medany','-mno-relax']
if host:flags+=['-DPAX_HOST_TEST','-g']
cc='gcc' if host else 'riscv32-esp-elf-gcc';cxx='g++' if host else 'riscv32-esp-elf-g++'
def compile(p):
 obj=out/('_'.join(p.relative_to(root).parts)+'.o')
 cmd=[cxx if p.suffix=='.cpp' else cc,*arch,*flags,*includes, '-std=gnu++17' if p.suffix=='.cpp' else '-std=gnu11']
 if p.suffix=='.cpp':cmd+=['-fno-exceptions','-fno-rtti','-fno-use-cxa-atexit','-fno-threadsafe-statics']
 cmd+=['-c',str(p),'-o',str(obj)]
 stamp=obj.with_suffix('.cmd');key=' '.join(cmd)
 if obj.exists() and obj.stat().st_mtime>max(p.stat().st_mtime,latest_header) and stamp.exists() and stamp.read_text()==key:return obj,0,''
 r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 if r.returncode==0:stamp.write_text(key)
 return obj,r.returncode,r.stdout
latest_header=max((p.stat().st_mtime for folder in paths for p in (root/folder).rglob('*.h*') if p.is_file()),default=0)
print('Compiling',len(units),'sources',flush=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:results=list(pool.map(compile,units))
errors=[r for r in results if r[1]]
for obj,code,output in errors:print(obj.name,output)
if errors:sys.exit(1)
objs=[str(r[0]) for r in results]
if host:
 test=root/('tests/playback_input.cpp' if input_test else 'tests/host.cpp');r=subprocess.run([cxx,*flags,*includes,'-std=gnu++17','-c',str(test),'-o',str(out/'test.o')]);
 if r.returncode:sys.exit(r.returncode)
 r=subprocess.run([cxx,*flags,*includes,'-std=gnu++17','-c',str(root/'tests/editor.cpp'),'-o',str(out/'editor-test.o')]);
 if r.returncode:sys.exit(r.returncode)
 r=subprocess.run([cxx,*flags,*includes,'-std=gnu++17','-c',str(root/'tests/cloud.cpp'),'-o',str(out/'cloud-test.o')]);
 if r.returncode:sys.exit(r.returncode)
 r=subprocess.run([cxx,*flags,*includes,'-std=gnu++17','-c',str(root/'tests/picker.cpp'),'-o',str(out/'picker-test.o')]);
 if r.returncode:sys.exit(r.returncode)
 cmd=[cxx,*objs,str(out/'test.o'),str(out/'editor-test.o'),str(out/'cloud-test.o'),str(out/'picker-test.o'),'-Wl,--gc-sections','-o',str(out/'scratch-test')]
else:
 import importlib.util
 spec=importlib.util.spec_from_file_location('pax_builder',str(workspace/'pax-modules/tools/build_pax.py'));builder=importlib.util.module_from_spec(spec);sys.modules[spec.name]=builder;spec.loader.exec_module(builder)
 cmd=[cxx,*arch,'-nostartfiles','-nostdlib','-Wl,--gc-sections','-Wl,--no-relax','-Wl,--no-warn-rwx-segments','-Wl,--entry=app_entry','-T'+str(root/'port/pax_cpp.ld'),*objs,*['-Wl,--wrap='+s for s in builder.NEWLIB_WRAPS],'-lstdc++','-lc','-lm','-lgcc','-o',str(out/'scratch.elf')]
print('Linking',flush=True)
r=subprocess.run(cmd)
if r.returncode:sys.exit(r.returncode)
if not host:
 subprocess.run(['riscv32-esp-elf-objcopy','-O','binary',str(out/'scratch.elf'),str(out/'scratch.bin')],check=True)
 nm=subprocess.check_output(['riscv32-esp-elf-nm','-n',str(out/'scratch.elf')],text=True);(out/'scratch.sym').write_text(nm)
 syms={line.split()[2]:int(line.split()[0],16) for line in nm.splitlines() if len(line.split())==3}
 data=(out/'scratch.bin').read_bytes();bss=syms['_bss_end']-(0x4a000000+len(data));(out/'scratch.pax').write_bytes(struct.pack('<8I',0x50415050,1,0,len(data),0,bss,0,0)+data)
 print('PAX:',len(data),'bytes, BSS:',bss,'SHA256:',hashlib.sha256((out/'scratch.pax').read_bytes()).hexdigest())
