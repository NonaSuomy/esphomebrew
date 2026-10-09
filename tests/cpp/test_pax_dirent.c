#define PAX_APP_SIDE 1
#include "pax_dirent.h"
#include <assert.h>
static int listing(const char *path,char *buf,int size) {
 const char data[]="game.dat\0saves/\0";
 if (strcmp(path,"/sd/roms/test")) return -1;
 assert(size >= (int)sizeof(data)); memcpy(buf,data,sizeof(data));return 2;
}
static app_services_t services;
const app_services_t *_pax_svc=&services;
int main(void) {
 assert(opendir("/sd/roms/test")==NULL && errno==ENOSYS);
 services.file_list_dir=listing;
 assert(opendir("/missing")==NULL && errno==ENOENT);
 DIR *one=opendir("/sd/roms/test"),*two=opendir("/sd/roms/test");assert(one&&two);
 struct dirent *entry=readdir(one);assert(!strcmp(entry->d_name,"game.dat")&&entry->d_type==DT_REG);
 entry=readdir(one);assert(!strcmp(entry->d_name,"saves")&&entry->d_type==DT_DIR);
 assert(readdir(one)==NULL);assert(telldir(one)==2);
 assert(!strcmp(readdir(two)->d_name,"game.dat"));
 seekdir(one,1);assert(!strcmp(readdir(one)->d_name,"saves"));
 rewinddir(one);assert(telldir(one)==0);assert(!strcmp(readdir(one)->d_name,"game.dat"));
 assert(closedir(one)==0&&closedir(two)==0);
 assert(readdir(NULL)==NULL&&errno==EBADF);
 return 0;
}
