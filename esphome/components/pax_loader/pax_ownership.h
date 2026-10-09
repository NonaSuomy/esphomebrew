#pragma once
#include <cstddef>
#include <cstdint>
namespace esphome { namespace pax_loader { namespace ownership {
struct Record { void *pointer; size_t bytes; Record *next; };
// Caller serializes table operations. Records are allocated outside the lock.
template<size_t N=256> class Table {
 public:
  void insert(Record *record) {
    const size_t bucket=index(record->pointer);
    record->next=buckets_[bucket];buckets_[bucket]=record;
  }
  Record *take(void *pointer) {
    if(pointer==nullptr)return nullptr;
    Record **link=&buckets_[index(pointer)];
    while(*link!=nullptr) {
      if((*link)->pointer==pointer) {
        Record *found=*link;*link=found->next;found->next=nullptr;return found;
      }
      link=&(*link)->next;
    }
    return nullptr;
  }
  Record *detach() {
    Record *all=nullptr;
    for(auto &bucket:buckets_) {
      while(bucket!=nullptr) {
        Record *node=bucket;bucket=node->next;node->next=all;all=node;
      }
    }
    return all;
  }
 private:
  static size_t index(void *pointer) {return (reinterpret_cast<uintptr_t>(pointer)>>4)%N;}
  Record *buckets_[N]={};
};
}}}
