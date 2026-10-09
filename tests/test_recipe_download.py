"""Exercise the real recipe parser/downloader with mocked firmware services."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class RecipeDownloadTests(unittest.TestCase):
    def test_download_failure_and_existing_file_handling(self):
        source = (ROOT / 'apps/recipe/main.c').read_text()
        # UI rendering is irrelevant to the file-transfer contract tested here.
        for name in ('update_ui_on_host', 'pump_ui_on_host'):
            start = source.index('static int ' + name + '(')
            opening = source.index('{', start)
            level, end = 1, opening + 1
            while level:
                if source[end] == '{': level += 1
                elif source[end] == '}': level -= 1
                end += 1
            source = source[:opening] + '{ (void)unused; return 0; }' + source[end:]
        fixture = json.loads((ROOT / 'apps/redalert/install-recipe.json').read_text())
        good = json.dumps({'extract': fixture})
        bad = json.loads(good)
        bad['extract']['media'][0]['files'][0]['url'] = 'http://other.example/file'
        header = f'\nstatic const char good_json[] = {json.dumps(good)};\nstatic const char bad_json[] = {json.dumps(json.dumps(bad))};\n'
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int mode, starts, removes, polls;
static uint64_t clock_us;
static int fake_stat(const char *path, pax_file_stat_t *out) {
    (void)path;
    if (mode == 1 || mode == 5 || polls >= 2) {
        memset(out, 0, sizeof(*out));
        out->size = mode == 5 ? 0 : mode == 2 ? 9 : 10;
        return 0;
    }
    return -1;
}
static int fake_start(const char *url, const char *path) {
    assert(strncmp(url, "https://archive.org/download/", 29) == 0);
    assert(strcmp(path, "/sd/roms/test/file.mix") == 0);
    starts++; return mode == 4 ? -2 : 0;
}
static int fake_status(pax_install_progress_t *p) {
    polls++; p->state = polls < 2 ? 1 : mode == 3 ? 3 : 2;
    p->done = 10; p->total = 10;
    return 0;
}
static int fake_mkdir(const char *p) { (void)p; return 0; }
static int fake_remove(const char *p) { (void)p; removes++; return 0; }
static int fake_ui(pax_ui_call_fn_t fn, void *arg, uint32_t timeout) {
    (void)fn; (void)arg; (void)timeout; return 0;
}
static int64_t fake_time(void) { clock_us += 250000; return clock_us; }
static void fake_delay(int n) { (void)n; }
int main(int argc, char **argv) {
    assert(parse_recipe(good_json, strlen(good_json)));
    assert(g_recipe.media_count == 2 && g_recipe.file_count == 3);
    assert(incoming_media_index("CD1.iso") == 1);
    assert(incoming_media_index("CD2.iso") == 0);
    assert(incoming_media_index("CD1.zip") == 1);
    assert(incoming_media_index("cncredalertalliedcd1.zip") == 1);
    assert(!parse_recipe(bad_json, strlen(bad_json)));
    app_services_t svc = {0};
    svc.archive_download_start = fake_start; svc.store_install_status = fake_status;
    svc.file_stat = fake_stat; svc.file_mkdir = fake_mkdir; svc.file_remove = fake_remove;
    svc.ui_call_sync = fake_ui; svc.get_time_us = fake_time; svc.delay_ms = fake_delay;
    g_svc = &svc;
    assert(parse_recipe(good_json, strlen(good_json)));
    (void)scan_incoming_files();
    assert(g_download_rows == 3 && g_download_media[0] == -1);
    for (mode = 0; mode < 6; mode++) {
        memset(&g_recipe, 0, sizeof(g_recipe));
        g_recipe.media[0].file_count = 1;
        strcpy(g_recipe.files[0].target, "/sd/roms/test/file.mix");
        strcpy(g_recipe.files[0].url, "https://archive.org/download/item/file.mix");
        g_recipe.files[0].expected_size = 10;
        g_exit_requested = false; starts = removes = polls = 0;
        bool success = download_media(0);
        assert(success == (mode == 0 || mode == 1));
        assert(starts == (mode == 1 || mode == 5 ? 0 : 1));
        assert(removes == (mode == 2 ? 1 : 0));
    }
    recipe_sha256_t sha; char digest[65];
    recipe_sha256_init(&sha); recipe_sha256_final(&sha,digest);
    assert(strcmp(digest,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")==0);
    recipe_sha256_init(&sha); recipe_sha256_update(&sha,"abc",3); recipe_sha256_final(&sha,digest);
    assert(strcmp(digest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")==0);
    const char *long_input="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    recipe_sha256_init(&sha); recipe_sha256_update(&sha,long_input,strlen(long_input)); recipe_sha256_final(&sha,digest);
    assert(strcmp(digest,"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")==0);
    for (int arg=1; arg<argc; arg++) {
        FILE *input=fopen(argv[arg],"rb"); assert(input);
        fseek(input,0,SEEK_END); long length=ftell(input); rewind(input);
        char *json=malloc(length+1); assert(json);
        assert(fread(json,1,length,input)==(size_t)length);json[length]=0;fclose(input);
        if(!parse_recipe(json,length)){fprintf(stderr,"Rejected recipe: %s\n",argv[arg]);abort();}
        free(json);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)
            (path / 'test.c').write_text(source + header + harness)
            sdk = ROOT.parent / 'pax-esphome/components/pax_loader'
            # The standalone GitHub snapshot has the same header under esphome/.
            if not sdk.exists(): sdk = ROOT / 'esphome/components/pax_loader'
            lvgl = ROOT / 'apps/_shared/lvgl'
            if not lvgl.exists(): lvgl = ROOT.parent / 'pax-modules/apps/_shared/lvgl'
            if not lvgl.exists():
                self.skipTest('Native LVGL checkout required for host recipe test')
            subprocess.run(['cc', '-std=gnu11', '-ffunction-sections', '-fdata-sections',
                '-I'+str(sdk), '-I'+str(ROOT / 'apps/recipe'), '-I'+str(lvgl),
                '-DLV_CONF_INCLUDE_SIMPLE', '-Wl,--gc-sections', str(path/'test.c'),
                '-o', str(path/'test')], check=True)
            fixtures=[]
            for entry in json.loads((ROOT/'install-recipes.json').read_text()):
                fixture_path=path/(entry['app']+'.json')
                fixture_path.write_text(json.dumps({'extract':json.loads((ROOT/entry['path']).read_text())}))
                fixtures.append(str(fixture_path))
            legacy=json.loads(good);legacy['extract']['version']=1
            fixture_path=path/'legacy.json';fixture_path.write_text(json.dumps(legacy));fixtures.append(str(fixture_path))
            subprocess.run([str(path/'test'),*fixtures], check=True)
