"""Exercise Recipe's real FAT and ZIP reader with synthetic DOS media."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]

class DosFloppyTests(unittest.TestCase):
 def test_zip_multiple_disks_and_bad_image(self):
  if not all(shutil.which(x) for x in ('mkfs.fat','mcopy','unzip','cc')):
   self.skipTest('dosfstools, mtools, unzip and C compiler required')
  sdk=ROOT.parent/'pax-esphome/components/pax_loader'
  if not sdk.exists(): sdk=ROOT/'esphome/components/pax_loader'
  lvgl=ROOT/'apps/_shared/lvgl'
  if not lvgl.exists(): lvgl=ROOT.parent/'pax-modules/apps/_shared/lvgl'
  if not lvgl.exists(): self.skipTest('LVGL checkout required')
  source=(ROOT/'apps/recipe/main.c').read_text()
  for name in ('update_ui_on_host','pump_ui_on_host'):
   start=source.index('static int '+name+'('); opening=source.index('{',start); depth=1; end=opening+1
   while depth:
    if source[end]=='{': depth+=1
    elif source[end]=='}': depth-=1
    end+=1
   source=source[:opening]+'{ (void)unused; return 0; }'+source[end:]
  harness=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *base;
static const char *hostpath(const char *p) { static char b[8][1024]; static int slot; char *s=b[(++slot)%8]; if(!strncmp(p,"/sd/",4)) snprintf(s,1024,"%s/%s",base,p+4); else snprintf(s,1024,"%s",p); return s; }
static void *op(const char *p,const char *m){return fopen(hostpath(p),m);}
static int cl(void *f){return fclose(f);}
static size_t rd(void *p,size_t a,size_t b,void *f){return fread(p,a,b,f);}
static size_t wr(const void *p,size_t a,size_t b,void *f){return fwrite(p,a,b,f);}
static int sk(void *f,long p,int w){return fseek(f,p,w);}
static long tl(void *f){return ftell(f);}
static int st(const char *p,pax_file_stat_t *o){struct stat s;if(stat(hostpath(p),&s))return -1;memset(o,0,sizeof(*o));o->size=s.st_size;o->is_dir=S_ISDIR(s.st_mode);return 0;}
static int rm(const char *p){return remove(hostpath(p));}
static int rn(const char *a,const char *b){return rename(hostpath(a),hostpath(b));}
static int md(const char *p){struct stat s;const char *v=hostpath(p);return (!stat(v,&s) && S_ISDIR(s.st_mode))?0:mkdir(v,0777);}
static int unzip_file(const char *z,const char *entry,const char *out,uint32_t max,uint32_t *written){char cmd[4096],zp[1024],dest[1024];snprintf(zp,sizeof(zp),"%s",hostpath(z));snprintf(dest,sizeof(dest),"%s",hostpath(out));snprintf(cmd,sizeof(cmd),"unzip -p '%s' '%s' > '%s'",zp,entry,dest);if(system(cmd))return -1;struct stat s;if(stat(dest,&s)||(uint64_t)s.st_size>max)return -1;*written=s.st_size;return 0;}
int main(int argc,char **argv){assert(argc==3);base=argv[1];app_services_t svc={0};svc.file_open=op;svc.file_close=cl;svc.file_read=rd;svc.file_write=wr;svc.file_seek=sk;svc.file_tell=tl;svc.file_stat=st;svc.file_remove=rm;svc.file_rename=rn;svc.file_mkdir=md;svc.mem_alloc=malloc;svc.mem_free=free;svc.file_zip_extract_named=unzip_file;g_svc=&svc;
 FILE *j=fopen(argv[2],"rb");assert(j);char json[10000];size_t n=fread(json,1,sizeof(json),j);fclose(j);assert(parse_recipe(json,n));unsigned found=0;assert(df_copy_zip("/sd/roms/anotherworld/incoming/disks.zip",&g_recipe.media[0],&found));assert(found==2);assert(df_game_ready());found=0;assert(df_copy_zip("/sd/roms/anotherworld/incoming/disks.zip",&g_recipe.media[0],&found));assert(found==2 && df_game_ready());found=0;assert(!df_copy_image("/sd/roms/anotherworld/incoming/bad.ima",&g_recipe.media[0],&found));return 0;}
'''
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp); incoming=p/'roms/anotherworld/incoming';incoming.mkdir(parents=True)
   mem=bytearray(21);mem[1]=1;mem[7]=1;mem[15]=4;mem[19]=4;mem[20]=255
   (p/'MEMLIST.BIN').write_bytes(mem);(p/'BANK01').write_bytes(b'abcd')
   for name,filename in [('disk1.ima','MEMLIST.BIN'),('disk2.ima','BANK01')]:
    img=p/name;img.write_bytes(bytes(1440*1024));subprocess.run(['mkfs.fat','-F','12',str(img)],check=True,stdout=subprocess.DEVNULL);subprocess.run(['mcopy','-i',str(img),str(p/filename),'::/'],check=True)
   with zipfile.ZipFile(incoming/'disks.zip','w',zipfile.ZIP_DEFLATED) as z:
    for name in ('disk1.ima','disk2.ima'):z.write(p/name,'disk images/'+name)
   (incoming/'bad.ima').write_bytes(bytes(512))
   recipe=json.loads((ROOT/'apps/anotherworld/install-recipe.json').read_text());(p/'recipe.json').write_text(json.dumps({'extract':recipe}))
   (p/'test.c').write_text(source+'\n'+harness)
   subprocess.run(['cc','-std=gnu11','-ffunction-sections','-fdata-sections','-I'+str(sdk),'-I'+str(ROOT/'apps/recipe'),'-I'+str(lvgl),'-DLV_CONF_INCLUDE_SIMPLE','-Wl,--gc-sections',str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test'),str(p),str(p/'recipe.json')],check=True)
   self.assertEqual((p/'roms/anotherworld/memlist.bin').read_bytes(),mem)
   self.assertEqual((p/'roms/anotherworld/bank01').read_bytes(),b'abcd')
if __name__=='__main__':unittest.main()
