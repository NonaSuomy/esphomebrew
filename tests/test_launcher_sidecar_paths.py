from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

def function(source, name):
    match = re.search(r'static (?:bool|void) '+name+r'\([^;]*?\)\s*\{', source)
    opening = source.index('{', match.start())
    end = opening + 1
    level = 1
    while level:
        if source[end] == '{': level += 1
        elif source[end] == '}': level -= 1
        end += 1
    return source[match.start():end]

class LauncherSidecarTests(unittest.TestCase):
    def test_json_paths_preserve_the_complete_package_basename(self):
        source = (ROOT/'apps/launcher_ui/main.c').read_text()
        code = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
""" + '\n'.join(function(source, name) for name in ('copy_text', 'ends_with', 'replace_package_suffix')) + r"""
int main(void) {
 const char *paths[]={"gb.pax","a.pax","launcher_ui.pax","/sd/roms/pax/gb.pax","/usb0/roms/pax/nes-debug.pax","http://192.168.0.162:8000/gb.pax","http://host/apps/scummvm/scumm/scummvm-scumm.pax"};
 char out[256],expected[256];
 for(size_t i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
  size_t base=strlen(paths[i])-4;memcpy(expected,paths[i],base);strcpy(expected+base,".json");
  assert(replace_package_suffix(paths[i],out,sizeof(out),".json"));assert(!strcmp(out,expected));
 }
 char exact[8],small[7];assert(replace_package_suffix("gb.pax",exact,sizeof(exact),".json"));assert(!strcmp(exact,"gb.json"));
 assert(!replace_package_suffix("gb.pax",small,sizeof(small),".json"));
 assert(!replace_package_suffix(NULL,out,sizeof(out),".json"));assert(!replace_package_suffix("gb.pax",NULL,sizeof(out),".json"));
 assert(!replace_package_suffix("gb.pax",out,0,".json"));assert(!replace_package_suffix("a",out,sizeof(out),".json"));
 assert(!replace_package_suffix("gb.zip",out,sizeof(out),".json"));
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.c').write_text(code)
            subprocess.run(['cc','-std=gnu11',str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
