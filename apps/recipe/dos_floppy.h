/* DOS FAT12/16 floppy reader for user-supplied game data. */
#ifndef PAX_RECIPE_DOS_FLOPPY_H
#define PAX_RECIPE_DOS_FLOPPY_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static uint16_t df_u16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t df_u32(const unsigned char *p) { return (uint32_t)df_u16(p) | ((uint32_t)df_u16(p + 2) << 16); }
static bool df_read(void *f, uint32_t pos, void *buf, size_t n)
{
  return pos <= 0x7fffffffU && g_svc->file_seek(f, (long)pos, 0) == 0 && g_svc->file_read(buf, 1, n, f) == n;
}

/* Only regular root-directory files are accepted; FAT12 and FAT16 use 8.3
 * names. No path from the image is ever used as a destination. */
static bool df_copy_image(const char *image, recipe_media_t *media, unsigned *found)
{
  unsigned char boot[512], entry[32], buf[512], fatbytes[2];
  pax_file_stat_t info = {0};
  if (g_svc->file_stat(image, &info) || info.is_dir || info.size < 512 || info.size > 0x7fffffffU) return false;
  void *in = g_svc->file_open(image, "rb");
  if (!in) return false;
  bool ok = false;
  if (!df_read(in, 0, boot, sizeof(boot)) || boot[510] != 0x55 || boot[511] != 0xaa) goto done;
  uint32_t bps = df_u16(boot+11), spc = boot[13], reserved = df_u16(boot+14), fats = boot[16];
  uint32_t roots = df_u16(boot+17), sectors = df_u16(boot+19), fatsz = df_u16(boot+22);
  if (!sectors) sectors = df_u32(boot+32);
  if (bps != 512 || !spc || (spc & (spc-1)) || spc > 128 || !reserved || !fats || fats > 2 || !roots || !fatsz || !sectors || (uint64_t)sectors*bps > info.size) goto done;
  uint32_t rootsecs = (roots*32 + bps-1)/bps, rootstart = reserved + fats*fatsz;
  uint32_t data = rootstart + rootsecs;
  if (data >= sectors) goto done;
  uint32_t clusters = (sectors-data)/spc;
  if (!clusters || clusters >= 65525) goto done;
  bool fat12 = clusters < 4085;
  if ((uint64_t)(clusters+2)*(fat12 ? 3 : 4) > (uint64_t)fatsz*bps*(fat12 ? 2 : 2)) goto done;
  for (uint32_t i=0; i<roots && !g_exit_requested; ++i) {
    if (!df_read(in, rootstart*bps+i*32, entry, 32)) goto done;
    if (entry[0] == 0) break;
    if (entry[0] == 0xe5 || (entry[11] & 0x18) || (entry[11] & 0x0f) == 0x0f) continue;
    char name[13]; unsigned n=0;
    for (unsigned j=0;j<8 && entry[j]!=' ';++j) { char c=(char)entry[j]; name[n++]=(c>='A'&&c<='Z')?c+32:c; }
    if (entry[8]!=' ') { name[n++]='.'; for(unsigned j=8;j<11 && entry[j]!=' ';++j) { char c=(char)entry[j]; name[n++]=(c>='A'&&c<='Z')?c+32:c; } }
    name[n]=0;
    recipe_file_t *mapping=NULL;
    for(int j=0;j<media->file_count;j++) { recipe_file_t *m=&g_recipe.files[media->first_file+j]; if(text_equal_ci(name,m->source)) { mapping=m; break; } }
    if (!mapping) continue;
    ++*found;
    if (file_exists(mapping->target)) continue;
    pax_file_stat_t occupied={0}; if (g_svc->file_stat(mapping->target,&occupied)==0) goto done;
    uint32_t length=df_u32(entry+28), cluster=df_u16(entry+26);
    if (!length || length > 16U*1024U*1024U || cluster<2 || cluster>=clusters+2 || !make_parent_dirs(mapping->target)) goto done;
    char temp[PATH_CAP]; if (!text_copy(temp,sizeof(temp),mapping->target) || !text_append(temp,sizeof(temp),".pax-part")) goto done;
    if (g_svc->file_stat(temp,&occupied)==0) goto done;
    void *out=g_svc->file_open(temp,"wb"); if(!out) goto done;
    bool copied=true; uint32_t left=length, hops=0;
    while(left && copied && !g_exit_requested) {
      if (cluster<2 || cluster>=clusters+2 || ++hops>clusters) { copied=false; break; }
      uint32_t offset=(data+(cluster-2)*spc)*bps;
      for(uint32_t s=0;s<spc && left;s++) {
        size_t count=left<bps?left:bps;
        if(!df_read(in,offset+s*bps,buf,count) || g_svc->file_write(buf,1,count,out)!=count) { copied=false; break; }
        left-=count;
      }
      if(left && copied) {
        uint32_t fatpos=fat12?cluster+cluster/2:cluster*2;
        if(fatpos+2>fatsz*bps || !df_read(in,reserved*bps+fatpos,fatbytes,2)) { copied=false; break; }
        uint32_t value=df_u16(fatbytes); cluster=fat12?((cluster&1)?value>>4:value&0xfff):value;
      }
    }
    if(g_svc->file_close(out)!=0) copied=false;
    if(!copied || left || g_exit_requested || file_exists(mapping->target) || g_svc->file_rename(temp,mapping->target)!=0) {
      g_svc->file_remove(temp); goto done;
    }
  }
  ok=!g_exit_requested;
done:
  g_svc->file_close(in); return ok;
}

/* Enumerate the ZIP central directory so all floppy images are processed.
 * Firmware performs decompression and CRC validation for each exact member. */
static bool df_copy_zip(const char *archive, recipe_media_t *media, unsigned *found)
{
  pax_file_stat_t info={0};
  if (!g_svc->file_zip_extract_named || !g_svc->file_remove || g_svc->file_stat(archive,&info) || info.is_dir || info.size<22 || info.size>0x7fffffffU) return false;
  void *f=g_svc->file_open(archive,"rb"); if(!f)return false;
  uint32_t tail=(uint32_t)(info.size<65557?info.size:65557);
  unsigned char *data=g_svc->mem_alloc(tail); if(!data){g_svc->file_close(f);return false;}
  bool ok=false; uint32_t end=(uint32_t)info.size-tail;
  if(!df_read(f,end,data,tail)) goto done;
  int32_t pos=(int32_t)tail-22;
  for(;pos>=0;--pos) if(df_u32(data+pos)==0x06054b50 && (uint32_t)pos+22+df_u16(data+pos+20)==tail) break;
  if(pos<0 || df_u16(data+pos+4) || df_u16(data+pos+6) || df_u16(data+pos+8)!=df_u16(data+pos+10)) goto done;
  uint32_t count=df_u16(data+pos+10), size=df_u32(data+pos+12), cursor=df_u32(data+pos+16);
  if(count>512 || (uint64_t)cursor+size>end+(uint32_t)pos) goto done;
  uint32_t limit=cursor+size, images=0;
  for(uint32_t i=0;i<count && !g_exit_requested;i++) {
    unsigned char h[46]; char name[ENTRY_CAP];
    if((uint64_t)cursor+46>limit || !df_read(f,cursor,h,46) || df_u32(h)!=0x02014b50) goto done;
    uint32_t namelen=df_u16(h+28), extra=df_u16(h+30), comment=df_u16(h+32);
    if(!namelen || namelen>=sizeof(name) || (uint64_t)cursor+46+namelen+extra+comment>limit || !df_read(f,cursor+46,name,namelen)) goto done;
    name[namelen]=0; cursor+=46+namelen+extra+comment;
    if(!ends_with_ci(name,".ima") && !ends_with_ci(name,".img")) continue;
    if(!relative_path_valid(name) || (df_u16(h+8)&1) || df_u32(h+24)>16U*1024U*1024U || !df_u32(h+24)) goto done;
    char cache[PATH_CAP];
    if(!join_path(cache,sizeof(cache),g_recipe.archive_dir,".pax-aw-floppy.ima")) goto done;
    pax_file_stat_t occupied={0}; if(g_svc->file_stat(cache,&occupied)==0) goto done;
    uint32_t written=0;
    if(g_svc->file_zip_extract_named(archive,name,cache,16U*1024U*1024U,&written)!=0 || written!=df_u32(h+24)) { g_svc->file_remove(cache); goto done; }
    bool copied=df_copy_image(cache,media,found); g_svc->file_remove(cache);
    if(!copied) goto done;
    images++;
  }
  ok=images>0 && cursor==limit && !g_exit_requested;
done:
  g_svc->mem_free(data); g_svc->file_close(f); return ok;
}
#endif
