#!/usr/bin/env python3
"""Build pinned libaom's portable, single-threaded 8-bit AV1 decoder for PAX."""
from pathlib import Path
import argparse,shutil,subprocess

def main():
 p=argparse.ArgumentParser();p.add_argument('--src',type=Path,required=True);p.add_argument('--gen',type=Path,required=True);p.add_argument('--jobs',type=int,default=2);a=p.parse_args()
 source=a.src/'third_party/aom';build=a.gen/'aom-build'
 cc=shutil.which('riscv32-esp-elf-gcc');cxx=shutil.which('riscv32-esp-elf-g++')
 if not cc or not cxx:raise SystemExit('ESP32-P4 cross compiler not found')
 flags='-march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany -ffunction-sections -fdata-sections -fno-strict-aliasing'
 command=['cmake','-S',str(source),'-B',str(build),'-G','Ninja','-DCMAKE_SYSTEM_NAME=Generic','-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',f'-DCMAKE_C_COMPILER={cc}',f'-DCMAKE_CXX_COMPILER={cxx}',f'-DCMAKE_C_FLAGS={flags}',f'-DCMAKE_CXX_FLAGS={flags}','-DCMAKE_BUILD_TYPE=Release','-DAOM_TARGET_CPU=generic','-DCONFIG_AV1_ENCODER=0','-DCONFIG_AV1_DECODER=1','-DCONFIG_MULTITHREAD=0','-DCONFIG_RUNTIME_CPU_DETECT=0','-DCONFIG_AV1_HIGHBITDEPTH=0','-DCONFIG_PIC=0','-DENABLE_DOCS=0','-DENABLE_EXAMPLES=0','-DENABLE_TESTS=0','-DENABLE_TOOLS=0']
 subprocess.run(command,check=True);subprocess.run(['cmake','--build',str(build),'--target','aom','--parallel',str(min(max(a.jobs,1),8))],check=True)
 if not (build/'libaom.a').is_file():raise SystemExit('libaom archive was not produced')
 print('gen_aom: generic C, 8-bit AV1, decoder only, one thread, no assembly')
if __name__=='__main__':main()
