// SPDX-License-Identifier: MIT
#pragma once
#include "pax_controller_remap.h"
#include "esphome/core/preferences.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <new>
namespace pax_controller {
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static pax_controller_map_t mapping,request;
static bool pending,failed;
static uint32_t (*sample)();
static esphome::ESPPreferenceObject preference;
static void setup(uint32_t (*read)()) {
 sample=read;pax_controller_defaults(&mapping);
 if(esphome::global_preferences){
  preference=esphome::global_preferences->make_preference<pax_controller_map_t>(0x50434d31,true);
  pax_controller_map_t saved{};if(preference.load(&saved) && pax_controller_valid(&saved))mapping=saved;
 }
}
static void loop() {
 pax_controller_map_t next{};bool save=false;
 portENTER_CRITICAL(&lock);if(pending){next=request;save=true;}portEXIT_CRITICAL(&lock);
 if(!save)return;
 bool ok=esphome::global_preferences && preference.save(&next) && esphome::global_preferences->sync();
 portENTER_CRITICAL(&lock);if(ok)mapping=next;failed=!ok;pending=false;portEXIT_CRITICAL(&lock);
}
static uint32_t apply(uint32_t raw) {
 pax_controller_map_t m;portENTER_CRITICAL(&lock);m=mapping;portEXIT_CRITICAL(&lock);
 return pax_controller_apply(&m,raw);
}
struct File {bool write;size_t offset; pax_controller_status_t status;};
static ssize_t read(void *cookie,char *out,size_t n) {
 auto *f=static_cast<File*>(cookie);if(f->write)return -1;
 size_t left=sizeof(f->status)-f->offset;n=std::min(n,left);memcpy(out,reinterpret_cast<char*>(&f->status)+f->offset,n);f->offset+=n;return n;
}
static ssize_t write(void *cookie,const char *data,size_t n) {
 auto *f=static_cast<File*>(cookie);if(!f->write || n>sizeof(f->status.map)-f->offset)return -1;
 memcpy(reinterpret_cast<char*>(&f->status.map)+f->offset,data,n);f->offset+=n;return n;
}
static int close(void *cookie) {
 auto *f=static_cast<File*>(cookie);int result=0;
 if(f->write){
  if(f->offset!=sizeof(f->status.map) || !pax_controller_valid(&f->status.map))result=-1;
  else {portENTER_CRITICAL(&lock);if(pending)result=-1;else{request=f->status.map;pending=true;failed=false;}portEXIT_CRITICAL(&lock);}
 }delete f;return result;
}
static FILE *open(const char *mode) {
 if(!mode || (mode[0]!='r' && mode[0]!='w'))return nullptr;
 auto *f=new(std::nothrow) File{};if(!f)return nullptr;f->write=mode[0]=='w';
 portENTER_CRITICAL(&lock);f->status.map=mapping;f->status.pending=pending;f->status.save_failed=failed;portEXIT_CRITICAL(&lock);
 f->status.raw=sample?sample():0;
#ifdef ESP_PLATFORM
 auto read_fn=+[](void *c,void *out,size_t n)->ssize_t{return read(c,static_cast<char*>(out),n);};
 auto write_fn=+[](void *c,const void *data,size_t n)->ssize_t{return write(c,static_cast<const char*>(data),n);};
 FILE *file=funopen(f,f->write?nullptr:read_fn,f->write?write_fn:nullptr,nullptr,close);
#else
 cookie_io_functions_t ops{};ops.read=read;ops.write=write;ops.close=close;
 FILE *file=fopencookie(f,mode,ops);
#endif
 if(!file)delete f;else setvbuf(file,nullptr,_IONBF,0);return file;
}
}
