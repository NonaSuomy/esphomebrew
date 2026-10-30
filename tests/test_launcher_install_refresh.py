"""Exercise installed-detail refresh without refetching the Store listing."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_launcher_catalog_pages import function

ROOT = Path(__file__).resolve().parents[1]


class InstallRefresh(unittest.TestCase):
    def test_committed_metadata_replaces_version_and_update_action(self):
        source = (ROOT / 'apps/launcher_ui/main.c').read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PATH_BYTES 256
static bool g_detail_is_installed, g_detail_update_available, g_detail_uninstall_confirm;
static bool g_detail_is_remote=true, g_action_busy, g_transfer_busy;
static char g_detail_name[64]="sample", g_detail_local_path[256];
static char g_detail_local_version[32]="1.0",g_detail_version[32]="2.0",g_remote[256];
static uint32_t g_detail_size=1024;
static struct {bool valid;} g_detail_extract_recipe;
static bool present=true, metadata=true;
static const char *disk_version="2.0";
static bool detail_package_present(const char *name,char *path,size_t n){
 assert(!strcmp(name,"sample"));snprintf(path,n,"/sd/roms/pax/sample.pax");return present;
}
static bool replace_package_suffix(const char *in,char *out,size_t n,const char *suffix){
 assert(!strcmp(in,"/sd/roms/pax/sample.pax") && !strcmp(suffix,".json"));
 snprintf(out,n,"/sd/roms/pax/sample.json");return true;
}
static int read_json_file(const char *path){
 assert(!strcmp(path,"/sd/roms/pax/sample.json"));return metadata ? 20 : -1;
}
static bool json_string_field(const char *json,int len,const char *key,char *out,size_t n){
 assert(!strcmp(key,"version"));snprintf(out,n,"%s",disk_version);return true;
}
static int compare_package_versions(const char *a,const char *b){return strcmp(a,b);}
''' + '\n'.join(function(source, n) for n in (
            'copy_text', 'append_text', 'append_uint', 'refresh_installed_detail',
            'detail_version_text', 'detail_action_text')) + r'''
int main(void){
 char text[96];g_detail_update_available=true;g_detail_uninstall_confirm=true;
 refresh_installed_detail();detail_version_text(text,sizeof(text));
 assert(g_detail_is_installed && !strcmp(g_detail_local_version,"2.0"));
 assert(!g_detail_update_available && !g_detail_uninstall_confirm);
 assert(strstr(text,"LOCAL 2.0") && strstr(text,"CURRENT") && !strstr(text,"UPDATE AVAILABLE"));
 assert(!strcmp(detail_action_text(1),"REINSTALL"));
 disk_version="1.0";refresh_installed_detail();
 assert(g_detail_update_available && !strcmp(detail_action_text(1),"UPDATE"));
 metadata=false;refresh_installed_detail();
 assert(!g_detail_local_version[0] && !g_detail_update_available);
 present=false;refresh_installed_detail();
 assert(!g_detail_is_installed && !strcmp(detail_action_text(1),"INSTALL"));
}
'''
        with tempfile.TemporaryDirectory() as td:
            c = Path(td) / 'test.c'
            c.write_text(code)
            exe = Path(td) / 'test'
            subprocess.run(['cc', '-std=c11', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        # The successful-transfer path refreshes data before the host updates
        # existing widgets; failed installs never claim the Store version.
        poll = function(source, 'poll_install_status')
        self.assertIn('if(progress.state==2)g_transfer_verify_pending=true;', poll)
        self.assertIn('refresh_installed_detail();', source[source.index('if(g_transfer_verify_pending)'):])
        update = function(source, 'update_selection_on_host')
        self.assertIn('detail_version_text(version_text, sizeof(version_text))', update)
        self.assertIn('label_text_changed(g_detail_version_label, version_text)', update)
        self.assertIn('g_detail_version_label = NULL;', function(source, 'teardown_ui_on_host'))


if __name__ == '__main__':
    unittest.main()
