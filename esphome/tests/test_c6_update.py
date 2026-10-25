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

    def test_hosted_ap_info_missing_payload_is_rejected(self):
        import runpy
        patch=runpy.run_path(str(ROOT/'tools/hosted_rpc_compat.py'))['patch_hosted_rpc_source']
        code=r"""#include <cassert>
#include <cstring>
#include <type_traits>
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=-2, SUCCESS=0;
constexpr int RPC_ID__Resp_WifiStaGetApInfo=42;
struct wifi_ap_record_t { int channel; };
struct ctrl_cmd_t { int resp_event_status, msg_id; struct {struct {wifi_ap_record_t *out_list;} wifi_scan_ap_list;} u; };
ctrl_cmd_t response{},request{};ctrl_cmd_t *reply;int releases;
ctrl_cmd_t *RPC_DEFAULT_REQ(){return &request;}
ctrl_cmd_t *rpc_slaveif_wifi_sta_get_ap_info(ctrl_cmd_t*){return reply;}
int rpc_rsp_callback(ctrl_cmd_t *p){if(p)releases++;return p && p->resp_event_status==SUCCESS ? ESP_OK : ESP_FAIL;}
struct {struct {void *(*_h_memcpy)(void*,const void*,unsigned long);} *funcs;} g_h;
int rpc_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
 if (!ap_info) return ESP_ERR_INVALID_ARG;
 ctrl_cmd_t *req = RPC_DEFAULT_REQ();
 ctrl_cmd_t *resp = rpc_slaveif_wifi_sta_get_ap_info(req);
 if (resp && resp->resp_event_status == SUCCESS) {
   g_h.funcs->_h_memcpy(ap_info, resp->u.wifi_scan_ap_list.out_list, sizeof(wifi_ap_record_t));
 }
 return rpc_rsp_callback(resp);
}
int sentinel(){return 0;}
int main(){
 wifi_ap_record_t out{},record{11};
 assert(rpc_wifi_sta_get_ap_info(nullptr)==ESP_ERR_INVALID_ARG);
 reply=nullptr;assert(rpc_wifi_sta_get_ap_info(&out)==ESP_FAIL && releases==0);
 reply=&response;response.msg_id=RPC_ID__Resp_WifiStaGetApInfo;
 assert(rpc_wifi_sta_get_ap_info(&out)==ESP_FAIL && releases==1);
 response.msg_id=0;response.u.wifi_scan_ap_list.out_list=&record;
 assert(rpc_wifi_sta_get_ap_info(&out)==ESP_FAIL && releases==2);
 response.msg_id=RPC_ID__Resp_WifiStaGetApInfo;
 decltype(g_h.funcs) funcs=new std::remove_pointer_t<decltype(g_h.funcs)>{std::memcpy};g_h.funcs=funcs;
 assert(rpc_wifi_sta_get_ap_info(&out)==ESP_OK && out.channel==11 && releases==3);
 delete funcs;
}
"""
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder);source=path/'rpc_wrap.cpp';source.write_text(code)
            patch(source);once=source.read_text();patch(source);self.assertEqual(source.read_text(),once)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(source),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

    def test_control_rpc_runs_on_main_loop(self):
        source=(ROOT/'components/pax_loader/pax_c6_update.cpp').read_text()
        dispatcher=source[source.index('struct MainCall {'):source.index('static esp_err_t query_version(')]
        pump=source[source.index('void loop() {'):source.index('  portENTER_CRITICAL(&lock); int64_t deadline')]+"}\n"
        code=r"""#include <cassert>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
using TaskHandle_t=int;
constexpr int ESP_FAIL=-1,pdTRUE=1,portMAX_DELAY=-1;
std::mutex lock,notify_lock;std::condition_variable notification;bool notified=false;
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
int xTaskGetCurrentTaskHandle(){return 1;}
void ulTaskNotifyTake(int,int){std::unique_lock<std::mutex> g(notify_lock);notification.wait(g,[]{return notified;});notified=false;}
void xTaskNotifyGive(int){std::lock_guard<std::mutex> g(notify_lock);notified=true;notification.notify_one();}
namespace esphome::watchdog {struct WatchdogManager {explicit WatchdogManager(int){}};}
"""+dispatcher+pump+r"""
int main(){
 auto main_thread=std::this_thread::get_id();std::atomic<bool> done=false;int calls=0;
 std::thread worker([&]{
   for(int i=0;i<2000;i++) assert(main_call([&]{assert(std::this_thread::get_id()==main_thread);calls++;return i;})==i);
   done=true;
 });
 while(!done){loop();std::this_thread::yield();}
 worker.join();assert(calls==2000 && pending_call==nullptr);
}
"""
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder);(path/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True,timeout=20)

    def test_unknown_version_requires_explicit_recovery(self):
        code=r"""#include "pax_c6_update.h"
#include <cassert>
int main(){
 static_assert(sizeof(pax_c6_request_t)==520);
 static_assert(sizeof(pax_c6_status_t)==176);
 using pax_c6::prepare_state;
 assert(prepare_state(PAX_C6_PREPARE,false,false)==PAX_C6_ERROR);
 assert(prepare_state(PAX_C6_PREPARE_UNKNOWN,false,false)==PAX_C6_READY);
 for(auto command : {PAX_C6_PREPARE,PAX_C6_PREPARE_UNKNOWN}) {
   assert(prepare_state(command,true,true)==PAX_C6_READY);
   assert(prepare_state(command,true,false)==PAX_C6_CURRENT);
 }
}
"""
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder);(path/'test.cpp').write_text('#include <initializer_list>\n'+code)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'components/pax_loader'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
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
