"""Exercise the firmware date service without ESP-IDF, using its actual body."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class DateServiceTest(unittest.TestCase):
 def test_timezone_boundaries_and_output_contract(self):
  source=(ROOT/'components/pax_loader/pax_loader.cpp').read_text()
  body=source.split('int PaxLoader::svc_device_date_get(char *out, uint32_t capacity) {',1)[1].split('\nint PaxLoader::svc_device_status_get',1)[0]
  body=body.replace('std::time(nullptr)','test_now')
  program=r'''#include <ctime>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <cstdio>
static time_t test_now;
static const time_t MIN_VALID_TLS_TIME=1609459200;
static int date_get(char *out,uint32_t capacity) {
'''+body+r'''
int main() {
 setenv("TZ","America/Toronto",1);tzset();
 char date[64];test_now=1792584000; // 2026-10-21 12:00 UTC
 assert(date_get(date,sizeof(date))==0);assert(strcmp(date,"Wednesday\n21 October 2026")==0);
 test_now=1709184600; // 2024-02-29 06:50 UTC
 assert(date_get(date,sizeof(date))==0);assert(strcmp(date,"Thursday\n29 February 2024")==0);
 test_now=1767240000; // 2026-01-01 04:00 UTC, previous local day
 assert(date_get(date,sizeof(date))==0);assert(strcmp(date,"Wednesday\n31 December 2025")==0);
 test_now+=3600;assert(date_get(date,sizeof(date))==0);assert(strcmp(date,"Thursday\n01 January 2026")==0);
 char tiny[2]={'X','Z'};assert(date_get(tiny,1)==-1 && tiny[0]==0 && tiny[1]=='Z');
 assert(date_get(nullptr,64)==-1);assert(date_get(tiny,0)==-1);
 test_now=0;strcpy(date,"stale");assert(date_get(date,sizeof(date))==-2 && date[0]==0);
 puts("PASS local date, leap day, local midnight/year rollover, short buffers and unsynchronized time");
}
'''
  with tempfile.TemporaryDirectory() as temp:
   p=Path(temp);(p/'test.cpp').write_text(program)
   subprocess.run(['c++','-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
