"""External asset readers: bounds, ownership and absence of embedded game banks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys
import os
import json

APPS = Path(__file__).resolve().parents[1] / 'apps'


class ExternalAssetTests(unittest.TestCase):
    def test_asset_readers_with_sanitizers(self):
        code = r'''
#include <cassert>
#include <cstdlib>
#include <vector>
#include "pax_asset_pack.h"
#include "pax_data.h"
static int allocations;
static void *allocate(size_t n) { ++allocations; return malloc(n); }
static void release(void *p) { --allocations; free(p); }
static std::vector<unsigned char> pack() {
    std::vector<unsigned char> b={'P','A','P','A','S','S','T','1',1,0,0,0,5,0,16,0,0,0,'a','.','p','n','g',2,0,1,0,2,0};
    b.insert(b.end(), {0,0,0,0,255,255,255,255,0,1}); return b;
}
static bool read(const std::vector<unsigned char> &b, const char *name, PaxIndexedAsset &out) {
    FILE *file=tmpfile(); assert(file); fwrite(b.data(),1,b.size(),file); rewind(file);
    const bool result=pax_read_asset_pack(file,name,allocate,release,out); fclose(file); return result;
}
int main() {
    const auto bytes=pack(); PaxIndexedAsset out;
    assert(read(bytes,"a.png",out)); assert(out.width==2 && out.height==1 && out.colors==2);
    assert(out.pixels[0]==0 && out.pixels[1]==1); release(out.pixels); assert(allocations==0);
    for(size_t i=0;i<bytes.size();++i) {PaxIndexedAsset bad; assert(!read({bytes.begin(),bytes.begin()+i},"a.png",bad)); assert(allocations==0);}
    PaxIndexedAsset missing; assert(!read(bytes,"missing.png",missing)); assert(allocations==0);
    auto bad=bytes; bad[8]=255; assert(!read(bad,"a.png",missing));
    bad=bytes; bad[23]=255; bad[24]=255; assert(!read(bad,"a.png",missing));
    bad=bytes; bad[14]=255; bad[15]=255; assert(!read(bad,"a.png",missing));
    assert(!pax_aw_validate_memlist(nullptr,0));
    unsigned char table[40]{}; table[7]=1; table[15]=1; table[19]=1; table[20]=255;
    assert(pax_aw_validate_memlist(table,sizeof(table)));
    for(size_t i=0;i<21;++i) assert(!pax_aw_validate_memlist(table,i));
    table[1]=7; assert(!pax_aw_validate_memlist(table,sizeof(table))); table[1]=0;
    table[15]=2; assert(!pax_aw_validate_memlist(table,sizeof(table)));
    table[15]=0; table[19]=0; assert(pax_aw_validate_memlist(table,sizeof(table)));
}
'''
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / 'test.cpp'
            source.write_text(code)
            output = Path(td) / 'test'
            subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I' + str(APPS / 'stratagus'), '-I' + str(APPS / 'anotherworld'),
                            str(source), str(APPS / 'anotherworld/pax_data.cpp'), '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    @unittest.skipUnless(os.environ.get('PAX_ANOTHERWORLD_TEST_DATA'), 'private game data not supplied')
    def test_private_dos_resources(self):
        folder = Path(os.environ['PAX_ANOTHERWORLD_TEST_DATA']).resolve()
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / 'banks'
            subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined',
                            '-DPAX_AW_DATA_DIR=' + json.dumps(str(folder)),
                            '-I' + str(APPS / 'anotherworld'), '-I' + str(APPS / 'anotherworld/core'),
                            str(Path(__file__).with_name('anotherworld_bank_fixture.cpp')),
                            str(APPS / 'anotherworld/core/bank.cpp'), str(APPS / 'anotherworld/pax_data.cpp'),
                            '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    @unittest.skipUnless(os.environ.get('PAX_STARGUS_TEST_PACK'), 'private sprite pack not supplied')
    def test_private_sprite_payloads(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / 'sprites'
            subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined',
                            '-I' + str(APPS / 'stratagus'),
                            str(Path(__file__).with_name('stargus_pack_fixture.cpp')), '-o', str(output)], check=True)
            subprocess.run([str(output), os.environ['PAX_STARGUS_TEST_PACK']], check=True,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_anotherworld_core_contains_no_game_bank_headers(self):
        core = APPS / 'anotherworld/core'
        self.assertFalse(core.is_symlink())
        self.assertFalse(list(core.glob('databank*.h')))
        self.assertFalse((core / 'datamemlistbin.h').exists())
        for source in core.glob('*.cpp'):
            self.assertNotIn('#include "databank', source.read_text())
            self.assertNotIn('#include "datamemlistbin.h"', source.read_text())


if __name__ == '__main__':
    unittest.main()
