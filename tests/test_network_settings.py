from pathlib import Path
import re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
def function(source,name):
    start=re.search(r'static (?:bool|void|int) '+name+r'\(',source).start()
    opening=source.index('{',start);end=opening+1;level=1
    while level:
        if source[end]=='{':level+=1
        elif source[end]=='}':level-=1
        end+=1
    return source[start:end]
class NetworkSettingsTests(unittest.TestCase):
    def sdk(self):
        path=ROOT.parent/'pax-esphome/components/pax_loader'
        return path if path.exists() else ROOT/'esphome/components/pax_loader'
    def test_server_url_validation_and_persistence(self):
        source=(ROOT/'apps/launcher_ui/main.c').read_text()
        code=r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PAX_APP_SIDE 1
#include "psram_app.h"
#define PATH_BYTES 256
static char g_network_url[256]="http://old:8000/",stored[256];
static app_services_t services;static const app_services_t *g_svc=&services;
static bool has_prefix(const char *s,const char *p){return !strncmp(s,p,strlen(p));}
static void copy_text(char *out,size_t n,const char *in){snprintf(out,n,"%s",in);}
static void *open_file(const char *path,const char *mode){assert(!strcmp(path,"/sd/roms/pax/.launcher_ui_network_url.txt"));assert(!strcmp(mode,"wb"));return stored;}
static size_t write_file(const void *p,size_t size,size_t count,void *f){memcpy(f,p,size*count);return count;}
static int close_file(void *f){(void)f;return 0;}
"""+function(source,'normalize_network_url')+function(source,'save_network_url')+r"""
int main(){char out[256];
 assert(normalize_network_url("http://192.168.0.162:8000",out,sizeof(out)));assert(!strcmp(out,"http://192.168.0.162:8000/"));
 assert(normalize_network_url("https://server.example/apps/\n",out,sizeof(out)));assert(!strcmp(out,"https://server.example/apps/"));
 const char *bad[]={"file:///sd/","http://","http:///apps/","http://bad host/","http://user:pass@host/","http://[::1]/","http://host/?q=x","http://host/#x","http://:8000/","http://host:abc/","http://host:0/","http://host:65536/","http://host:/"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!normalize_network_url(bad[i],out,sizeof(out)));
 services.file_open=open_file;services.file_write=write_file;services.file_close=close_file;
 assert(save_network_url("http://new:8000"));assert(!strcmp(stored,"http://new:8000/"));assert(!strcmp(g_network_url,stored));
 assert(!save_network_url("http://"));assert(!strcmp(g_network_url,"http://new:8000/"));
 return 0;}
"""
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.c').write_text(code)
            subprocess.run(['cc','-std=gnu11','-I'+str(self.sdk()),str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
    def test_radio_status_summary_and_missing_details(self):
        source=(ROOT/'apps/launcher_ui/main.c').read_text()
        code=r"""
#include <assert.h>
#include <string.h>
#include "pax_wifi_settings.h"
"""+function(source,'append_text')+function(source,'append_uint')+function(source,'append_wifi_summary')+r"""
int main(){
 pax_wifi_status_t status={0};char out[256]="Wi-Fi | Connected | 192.168.0.241";
 append_wifi_summary(out,sizeof(out),&status);assert(!strcmp(out,"Wi-Fi | Connected | 192.168.0.241"));
 strcpy(status.ssid,"Test network");status.channel=6;status.frequency_mhz=2437;strcpy(status.security,"WPA2-PSK / AES-CCMP");
 append_wifi_summary(out,sizeof(out),&status);assert(!strcmp(out,"Wi-Fi | Connected | 192.168.0.241 | Test network | 2.4 GHz (2437 MHz, ch 6) | WPA2-PSK / AES-CCMP"));
 status.frequency_mhz=5180;status.channel=36;out[0]=0;append_wifi_summary(out,sizeof(out),&status);assert(strstr(out,"5 GHz (5180 MHz, ch 36)"));
 struct {char text[10];char guard;} tiny={{0},'x'};append_wifi_summary(tiny.text,sizeof(tiny.text),&status);assert(tiny.guard=='x' && tiny.text[9]==0);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.c').write_text(code)
            subprocess.run(['cc','-std=gnu11','-I'+str(self.sdk()),str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
    def test_wifi_stream_validation_commit_and_rollback(self):
        stub=r"""
#pragma once
#include <string>
#include <cstring>
namespace esphome { using StringRef=std::string; namespace wifi {
class WiFiAP {std::string ssid,pwd;public:void set_ssid(const std::string &s){ssid=s;}void set_password(const std::string &p){pwd=p;}const std::string &get_ssid()const{return ssid;}const std::string &get_password()const{return pwd;}};
class WiFiComponent {public:WiFiAP sta;bool connected=true;int saves=0;WiFiAP get_sta(){return sta;}void disable(){connected=false;}void enable(){}void set_sta(const WiFiAP &a){sta=a;}bool is_connected(){return connected;}void wifi_ssid_to(char *out){std::strcpy(out,sta.get_ssid().c_str());}void save_wifi_sta(const std::string &s,const std::string &p){saves++;sta.set_ssid(s);sta.set_password(p);}};
inline WiFiComponent device;inline WiFiComponent *global_wifi_component=&device;
}}
"""
        code=r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#define USE_WIFI
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int64_t now=0;static int64_t esp_timer_get_time(){return now;}
#include "pax_wifi_stream.h"
static pax_wifi_status_t status(){pax_wifi_status_t s{};FILE *f=pax_wifi::open("rb");assert(f);assert(fread(&s,1,sizeof(s),f)==sizeof(s));assert(fgetc(f)==EOF);assert(!fclose(f));return s;}
static int send(pax_wifi_request_t &r){FILE *f=pax_wifi::open("wb");assert(f);assert(fwrite(&r,1,sizeof(r),f)==sizeof(r));return fclose(f);}
int main(){
 static_assert(sizeof(pax_wifi_request_t)==108);static_assert(sizeof(pax_wifi_status_t)==104);static_assert(offsetof(pax_wifi_status_t,channel)==PAX_WIFI_STATUS_BASE_BYTES);
 assert(pax_wifi_channel_mhz(1)==2412 && pax_wifi_channel_mhz(6)==2437 && pax_wifi_channel_mhz(14)==2484 && pax_wifi_channel_mhz(36)==5180 && !pax_wifi_channel_mhz(0));
 assert(!strcmp(pax_wifi_auth_name(3),"WPA2-PSK") && !strcmp(pax_wifi_auth_name(0),"Open") && !strcmp(pax_wifi_cipher_name(4),"AES-CCMP"));
 FILE *legacy=pax_wifi::open("rb");unsigned char old_status[PAX_WIFI_STATUS_BASE_BYTES];assert(fread(old_status,1,sizeof(old_status),legacy)==sizeof(old_status));assert(!fclose(legacy));
 auto &device=esphome::wifi::device;device.sta.set_ssid("Original");device.sta.set_password("original-password");pax_wifi::loop();
 auto s=status();assert(!strcmp(s.ssid,"Original") && s.password_set==1);
 pax_wifi_request_t r{};r.magic=PAX_WIFI_SETTINGS_MAGIC;r.replace_password=1;strcpy(r.ssid,"New network");strcpy(r.password,"test-password");assert(pax_wifi_request_valid(&r));
 strcpy(r.password,"short");assert(!pax_wifi_request_valid(&r));memset(r.password,'a',64);r.password[64]=0;assert(pax_wifi_request_valid(&r));r.password[0]='z';assert(!pax_wifi_request_valid(&r));
 strcpy(r.password,"test-password");assert(send(r)==0);assert(status().status==1);assert(send(r)!=0);pax_wifi::loop();assert(device.sta.get_ssid()=="New network" && device.saves==0);
 device.connected=true;pax_wifi::loop();assert(status().status==2 && device.saves==1);
 strcpy(r.ssid,"Unreachable");r.replace_password=0;r.password[0]=0;assert(send(r)==0);pax_wifi::loop();assert(device.sta.get_password()=="test-password");now+=45000001;pax_wifi::loop();
 assert(status().status==-1 && device.sta.get_ssid()=="New network" && device.saves==1);
 FILE *f=pax_wifi::open("wb");assert(fwrite(&r,1,4,f)==4);assert(fclose(f)!=0);assert(!pax_wifi::queued);
 memset(r.ssid,'x',sizeof(r.ssid));assert(!pax_wifi_request_valid(&r));
 return 0;}
"""
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);header=path/'esphome/components/wifi/wifi_component.h';header.parent.mkdir(parents=True);header.write_text(stub);log=path/'esphome/core/log.h';log.parent.mkdir(parents=True);log.write_text('#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n');(path/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=gnu++17','-D_GNU_SOURCE','-I'+str(path),'-I'+str(self.sdk()),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
