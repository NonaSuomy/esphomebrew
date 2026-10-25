"""Wi-Fi SD credential backup integrity and atomic replacement checks."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class WifiBackupTests(unittest.TestCase):
    def test_backup_integrity(self):
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "wifi-backup-test"
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-missing-field-initializers", "-fsanitize=address,undefined",
                "-I" + str(ROOT / "components/pax_loader"),
                str(ROOT / "tests/components/pax_loader/wifi_backup_test.cpp"),
                "-Wl,--wrap=rename", "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable), folder], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    def test_startup_restore_and_connected_backup(self):
        import json
        stream = (ROOT / "components/pax_loader/pax_wifi_stream.h").read_text()
        function = stream[stream.index("static void maintain_backup"):stream.index("// Discard SNTP")]
        with tempfile.TemporaryDirectory() as folder:
            card = Path(folder) / "card"
            function = function.replace("/sdcard", str(card))
            source = Path(folder) / "runtime.cpp"
            source.write_text(r'''
#include "pax_wifi_backup.h"
#include <string>
#include <cassert>
#include <filesystem>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int64_t clock_us=1;
static int64_t esp_timer_get_time(){return clock_us;}
namespace esphome { namespace wifi {
struct WiFiAP {
 std::string ssid,password;
 const std::string &get_ssid()const{return ssid;}
 const std::string &get_password()const{return password;}
};
struct WiFiComponent {
 WiFiAP current{"flash-network","flash-password"};bool connected=false,disabled=false;int saved=0;
 WiFiAP get_sta(){return current;}
 void disable(){connected=false;disabled=true;}
 void enable(){disabled=false;}
 bool is_disabled(){return disabled;}
 void save_wifi_sta(const char *ssid,const char *password){current={ssid,password};saved++;}
 void wifi_ssid_to(char *out){strcpy(out,connected?current.ssid.c_str():"");}
 bool is_connected(){return connected;}
};
}}
''' + "static constexpr const char *BACKUP=" + json.dumps(str(card / "roms/pax/wifi.bin")) + ";\n" + function + r'''
int main(int argc,char **argv){
 assert(argc==3);std::string mode=argv[1];std::filesystem::create_directories(std::string(argv[2])+"/roms/pax");
 esphome::wifi::WiFiComponent wifi;
 pax_wifi_request_t request{};request.magic=PAX_WIFI_SETTINGS_MAGIC;request.replace_password=1;
 strcpy(request.ssid,"sd-network");strcpy(request.password,"sd-password");
 if(mode!="new")assert(pax_wifi_backup::save(BACKUP,request));
 if(mode=="previous")assert(rename(BACKUP,(std::string(BACKUP)+".prev").c_str())==0);
 if(mode=="temporary")assert(rename(BACKUP,(std::string(BACKUP)+".tmp").c_str())==0);
 if(mode=="disabled")wifi.disabled=true;
 maintain_backup(&wifi,false,mode=="user");
 assert(wifi.disabled==(mode=="disabled"));
 if(mode=="restore" || mode=="previous" || mode=="temporary" || mode=="disabled"){
  assert(wifi.saved==1 && wifi.current.ssid=="sd-network" && wifi.current.password=="sd-password");
  // A failed candidate must not replace the last working backup.
  wifi.current={"bad-network","bad-password"};clock_us+=6000000;maintain_backup(&wifi,true,false);
  pax_wifi_request_t loaded{};assert(pax_wifi_backup::load((std::string(BACKUP)+(mode=="previous"?".prev":mode=="temporary"?".tmp":"")).c_str(),loaded));assert(!strcmp(loaded.ssid,"sd-network"));
 }else{assert(wifi.saved==0 && wifi.current.ssid=="flash-network");}
 // A successful change replaces or creates the SD copy.
 wifi.current={"working-network","working-password"};wifi.connected=true;clock_us+=6000000;maintain_backup(&wifi,false,false);
 pax_wifi_request_t loaded{};assert(pax_wifi_backup::load(BACKUP,loaded));assert(!strcmp(loaded.ssid,"working-network"));
 assert(!strcmp(loaded.password,"working-password"));
}
''')
            executable = Path(folder) / "runtime"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "components/pax_loader"), str(source), "-o", str(executable)], check=True)
            for mode in ("restore", "previous", "temporary", "user", "new", "disabled"):
                import shutil
                shutil.rmtree(card, ignore_errors=True)
                subprocess.run([str(executable), mode, str(card)], check=True)

if __name__ == "__main__":
    unittest.main()
