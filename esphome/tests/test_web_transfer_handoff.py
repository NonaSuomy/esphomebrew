"""Test the production HTTP storage reservation across consecutive uploads."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'components/pax_web/pax_web.cpp').read_text()
method=source.split('bool PaxWeb::reserve_() {',1)[1].split('void PaxWeb::report_',1)[0]
code=r'''#include <atomic>
#include <cassert>
#include <cstdint>
constexpr int pdTRUE=1;
#define pdMS_TO_TICKS(x) (x)
uint32_t clock_ms=0; int waits=0; bool release_completed=true, grant=true, accepted=true, accept_on_timeout=false;
std::atomic<int> *state;
uint32_t millis(){return clock_ms;}
void vTaskDelay(int ticks){clock_ms+=ticks;++waits;if(release_completed)state->store(0);}
int xSemaphoreTake(int,int ticks){
 if(!ticks)return 0;
 if(grant){state->store(accepted?2:3);return pdTRUE;}
 if(accept_on_timeout)state->store(2);
 return 0;
}
struct PaxWeb {std::atomic<int> transfer_{0};int granted_=0;bool reserve_();};
bool PaxWeb::reserve_() {''' + method + r'''
int main(){
 PaxWeb web;state=&web.transfer_;
 web.transfer_=4;assert(web.reserve_());assert(waits==1&&web.transfer_==2);
 waits=0;web.transfer_=2;assert(!web.reserve_());assert(waits==0&&web.transfer_==2);
 web.transfer_=4;release_completed=false;clock_ms=0;assert(!web.reserve_());assert(clock_ms==1000&&web.transfer_==4);
 web.transfer_=0;grant=false;assert(!web.reserve_());assert(web.transfer_==0);
 web.transfer_=0;accept_on_timeout=true;assert(!web.reserve_());assert(web.transfer_==4);
 web.transfer_=0;grant=true;accepted=false;assert(!web.reserve_());assert(web.transfer_==0);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS web upload handoff: completed transfer, active transfer, bounded wait, grant rejection and timeout cleanup')
