from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
import threading
import unittest
import urllib.request
from functools import partial
from http.server import ThreadingHTTPServer
ROOT=Path(__file__).resolve().parents[1]

class NetworkDetailsTests(unittest.TestCase):
    def test_server_live_metadata_and_launcher_parser(self):
        spec=importlib.util.spec_from_file_location('serve_dev',ROOT/'tools/serve_dev.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);file=path/'demo.pax';file.write_bytes(b'fixture');os.utime(file,(951849000,951849000))
            server=ThreadingHTTPServer(('127.0.0.1',0),partial(mod.PaxRequestHandler,directory=str(path)));thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                url=f'http://127.0.0.1:{server.server_port}/'
                with urllib.request.urlopen(url) as response:page=response.read().decode()
                self.assertIn('data-pax-size="7" data-pax-mtime="951849000"',page)
                self.assertIn('2000-02-29 18:30 UTC',page)
                with urllib.request.urlopen(url+'demo.pax') as response:self.assertEqual(response.read(),b'fixture')
                file.write_bytes(b'new file bytes')
                with urllib.request.urlopen(url) as response:self.assertIn('data-pax-size="14"',response.read().decode())
            finally:server.shutdown();thread.join();server.server_close()
            harness=r'''
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "network_file_info.h"
int main(){pax_file_stat_t info={0};
 const char *tag="<a href=\"demo.pax\" data-pax-size=\"5368709120\" data-pax-mtime=\"951849000\">";
 assert(pax_network_file_info(tag,strlen(tag),&info));assert(info.size==5368709120ULL && info.mtime==951849000);
 const char *old="<a href=\"demo.pax\">";assert(!pax_network_file_info(old,strlen(old),&info));
 const char *bad="<a data-pax-size=\"18446744073709551616\">";assert(!pax_network_file_info(bad,strlen(bad),&info));
 const char *incomplete="<a data-pax-size=\"1";assert(!pax_network_file_info(incomplete,strlen(incomplete),&info));
 const char *unknown="<a data-pax-size='12' data-pax-mtime='999999999999'>";
 assert(pax_network_file_info(unknown,strlen(unknown),&info) && info.size==12 && info.mtime==0);
 return 0;}
'''
            sdk=ROOT.parent/'pax-esphome/components/pax_loader'
            if not sdk.exists():sdk=ROOT/'esphome/components/pax_loader'
            (path/'test.c').write_text(harness)
            subprocess.run(['cc','-std=gnu11','-I'+str(sdk),'-I'+str(ROOT/'apps/launcher_ui'),str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
