"""Portable C6 image integrity and OTA fault-injection checks."""
from pathlib import Path
import os
import importlib.util
import json
import hashlib
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class C6UpdateTests(unittest.TestCase):
    def test_published_spi_artifact_matches_approved_hash(self):
        registry=json.loads((ROOT/'components/pax_loader/c6-firmware.json').read_text())
        image=registry['images']['c6-spi-aio']
        artifact=ROOT/'c6/2.12.12/aio-spi/network_adapter.bin'
        if not artifact.exists():artifact=ROOT.parent/'web-installer/c6/2.12.12/aio-spi/network_adapter.bin'
        self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(),image['sha256'])
        self.assertEqual(image['transport'],'spi-full-duplex')
        self.assertIn('/aio-spi/',image['url'])
        self.assertEqual(registry['images']['c6-sdio']['transport'],'sdio')
        self.assertNotEqual(image['sha256'],registry['images']['c6-sdio']['sha256'])

    def test_transport_selection_rejects_other_wiring(self):
        spec=importlib.util.spec_from_file_location('c6_profiles',ROOT/'components/pax_loader/c6_profiles.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        select=module.select_profile
        aio=dict(variant='ESP32C6',type='spi',active_high=True,reset_pin=2,cs_pin=14,clk_pin=15,mosi_pin=7,miso_pin=9,data_ready_pin=10,handshake_pin=11)
        self.assertEqual(select(aio),'c6-spi-aio')
        for key in ['reset_pin','cs_pin','clk_pin','mosi_pin','miso_pin','data_ready_pin','handshake_pin']:
            self.assertEqual(select({**aio,key:99}),'')
        self.assertEqual(select({**aio,'spi_mode':2}),'')
        self.assertEqual(select({**aio,'active_high':False}),'')
        self.assertEqual(select(dict(variant='ESP32C6',type='sdio')),'c6-sdio')
        self.assertEqual(select(dict(variant='ESP32C6',type='sdio',bus_width=1)),'')
        self.assertEqual(select(dict(variant='ESP32',type='sdio')),'')
        self.assertEqual(select({}), '')

    def test_ethernet_query_connects_lazy_hosted_transport(self):
        source=(ROOT/'components/pax_loader/pax_c6_update.cpp').read_text()
        start=source.index('static esp_err_t query_version(')
        helper=source[start:source.index('\n}\n',start)+2]
        code=r"""#include <cassert>
#define PAX_C6_BUSY 1
using esp_err_t=int;constexpr int ESP_OK=0;
struct esp_hosted_coprocessor_fwver_t {};
int queries,initializations,connections,init_error,connect_error;bool connected;
int esp_hosted_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t*){queries++;return connected ? 0 : -1;}
int esp_hosted_init(){initializations++;return init_error;}
int esp_hosted_connect_to_slave(){connections++;connected=!connect_error;return connect_error;}
void report(unsigned,const char*){}
"""+helper+r"""
int main(){
 esp_hosted_coprocessor_fwver_t current;
 assert(query_version(current)==0 && queries==2 && initializations==1 && connections==1);
 assert(query_version(current)==0 && queries==3 && initializations==1 && connections==1);
 connected=false;init_error=-2;assert(query_version(current)==-2 && connections==1);
 init_error=0;connect_error=-3;assert(query_version(current)==-3 && connections==2);
}
"""
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder);(path/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

    def test_image_and_transfer_failures(self):
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "c6-update-test"
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-missing-field-initializers", "-fsanitize=address,undefined",
                "-I" + str(ROOT / "components/pax_loader"),
                str(ROOT / "tests/components/pax_loader/c6_update_test.cpp"),
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

if __name__ == "__main__":
    unittest.main()
