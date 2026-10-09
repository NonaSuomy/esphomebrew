#include "pax_ownership.h"
#include <cassert>
using namespace esphome::pax_loader::ownership;
int main() {
 Table<2> table;
 Record first{reinterpret_cast<void *>(16),8,nullptr};
 Record second{reinterpret_cast<void *>(48),64,nullptr};
 table.insert(&first);table.insert(&second);
 assert(table.take(nullptr)==nullptr);
 assert(table.take(reinterpret_cast<void *>(99))==nullptr);
 // Colliding buckets retain the other live block when one is freed.
 assert(table.take(first.pointer)==&first);
 assert(table.take(first.pointer)==nullptr);
 table.insert(&first);
 // Failed realloc restores ownership of the original block.
 Record *pending=table.take(first.pointer);table.insert(pending);
 assert(table.take(first.pointer)==&first);
 // A successful realloc changes the key without leaving the old pointer.
 first.pointer=reinterpret_cast<void *>(80);first.bytes=128;table.insert(&first);
 assert(table.take(reinterpret_cast<void *>(16))==nullptr);
 Record *all=table.detach();unsigned count=0;size_t bytes=0;
 while(all){++count;bytes+=all->bytes;all=all->next;}
 assert(count==2&&bytes==192&&table.detach()==nullptr);
 assert(table.take(first.pointer)==nullptr);
 table.insert(&first);assert(table.take(first.pointer)==&first);
}
