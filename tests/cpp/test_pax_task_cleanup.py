#!/usr/bin/env python3
"""Regression: child cleanup waits for firmware services and display leases."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
source=(root/'pax-esphome/components/pax_loader/pax_loader.cpp').read_text()
a=source.index('struct PaxTaskStart');b=source.index('static void pax_child_task',a)
c=source.index('static void delete_registered_pax_tasks()',b);d=source.index('// The loader owns memory',c)
PREFIX=r"""
#include <cstdlib>
#include <cassert>
#include <vector>
using TaskHandle_t=void *;
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
struct Parked {};
static TaskHandle_t current=nullptr;
static TaskHandle_t xTaskGetCurrentTaskHandle(){return current;}
static void (*on_delay)()=nullptr;
static void vTaskDelay(int){if(on_delay)on_delay();else throw Parked{};}
static void xTaskNotifyGive(TaskHandle_t){}
static std::vector<TaskHandle_t> ordinary,caps;
static void vTaskDelete(TaskHandle_t h){ordinary.push_back(h);}
static void vTaskDeleteWithCaps(TaskHandle_t h){caps.push_back(h);}
"""
SUFFIX=r"""
static int echo(int value){assert(pax_service_calls==1);return value;}
int main(){
 auto a=reinterpret_cast<void *>(1),b=reinterpret_cast<void *>(2);
 assert(remember_pax_task(a,false,nullptr));
 assert(remember_pax_task(b,true,nullptr));
 current=a;
 assert(PaxServiceGate<echo>::call(7)==7);
 assert(pax_service_calls==0 && pax_tasks[0].services==0);
 {PaxServiceCallGuard guard;
  assert(!begin_pax_quiesce());
  {PaxServiceCallGuard nested;assert(pax_service_calls==2);}
  assert(pax_service_calls==1);
 }
 assert(begin_pax_quiesce());
 try{PaxServiceGate<echo>::call(1);assert(false);}catch(Parked&){}
 assert(pax_service_calls==0);
 pax_tasks_closing=false;
 {PaxServiceCallGuard guard;pax_display_lease(1);}
 assert(!begin_pax_quiesce());
 {PaxServiceCallGuard guard;pax_display_lease(-1);}
 assert(begin_pax_quiesce());
 delete_registered_pax_tasks();
 assert(ordinary.size()==1 && ordinary[0]==a);
 assert(caps.size()==1 && caps[0]==b);
 delete_pax_task(a);assert(ordinary.size()==1);
 assert(!remember_pax_task(a,false,nullptr));
 pax_tasks_closing=false;
 for(size_t i=0;i<MAX_PAX_TASKS;i++)
  assert(remember_pax_task(reinterpret_cast<void *>(i+1),false,nullptr));
 assert(!remember_pax_task(reinterpret_cast<void *>(99),false,nullptr));
 delete_registered_pax_tasks();assert(ordinary.size()==MAX_PAX_TASKS+1);
 pax_tasks_closing=false;assert(remember_pax_task(b,true,nullptr));
 pax_tasks[0].services=1;
 on_delay=[](){assert(caps.size()==1);pax_tasks[0].services=0;};
 delete_pax_task(b);assert(caps.size()==2);on_delay=nullptr;
 // GUI callbacks are not app tasks: they can finish a pending UI service.
 current=reinterpret_cast<void *>(100);pax_tasks_closing=true;
 assert(PaxServiceGate<echo>::call(9)==9);
 // Entry task calls use the same boundary check as child tasks.
 current=a;pax_entry_task=a;pax_tasks_closing=false;
 {PaxServiceCallGuard guard;assert(pax_entry_services==1);}
 assert(pax_entry_services==0 && pax_service_calls==0);
}
"""
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)/'test.cpp';binary=Path(directory)/'test'
 p.write_text(PREFIX+source[a:b]+source[c:d]+SUFFIX)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('PAX task/service cleanup regression passed')
