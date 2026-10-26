#pragma once
/* Catalog records grow in 128-entry batches in PSRAM. A refresh drops the
 * previous allocation; five visible row widgets remain independent of size. */
typedef struct {
  void *block;
  int capacity;
  char (*names)[NAME_BYTES], (*titles)[NAME_BYTES], (*paths)[PATH_BYTES], (*files)[128];
  pax_file_stat_t *stats;
} CatalogMemory;
static size_t catalog_region(size_t bytes) { return (bytes + 15U) & ~(size_t)15U; }
static bool catalog_memory_reserve(CatalogMemory *memory, int needed,
                                  void *(*allocate)(size_t), void (*release)(void *))
{
  if (needed <= memory->capacity) return true;
  if (needed > MAX_APPS) return false;
  int capacity = memory->capacity ? memory->capacity*2 : 128;
  while(capacity<needed && capacity<MAX_APPS)capacity*=2;
  if (capacity > MAX_APPS) capacity = MAX_APPS;
  const size_t n=catalog_region((size_t)capacity*NAME_BYTES);
  const size_t p=catalog_region((size_t)capacity*PATH_BYTES);
  const size_t f=catalog_region((size_t)capacity*128);
  const size_t s=catalog_region((size_t)capacity*sizeof(pax_file_stat_t));
  unsigned char *block=allocate(n*2+p+f+s);
  if (!block) return false; /* Preserve the complete old catalog on failure. */
  memset(block,0,n*2+p+f+s);
  CatalogMemory next={.block=block,.capacity=capacity,
    .names=(void *)block,.titles=(void *)(block+n),.paths=(void *)(block+n*2),
    .files=(void *)(block+n*2+p),.stats=(void *)(block+n*2+p+f)};
  if(memory->block){
    size_t count=(size_t)memory->capacity;
    memcpy(next.names,memory->names,count*NAME_BYTES);
    memcpy(next.titles,memory->titles,count*NAME_BYTES);
    memcpy(next.paths,memory->paths,count*PATH_BYTES);
    memcpy(next.files,memory->files,count*128);
    memcpy(next.stats,memory->stats,count*sizeof(pax_file_stat_t));
    release(memory->block);
  }
  *memory=next;return true;
}
static void catalog_memory_release(CatalogMemory *memory,void (*release)(void *))
{
  if(memory->block)release(memory->block);
  memset(memory,0,sizeof(*memory));
}
