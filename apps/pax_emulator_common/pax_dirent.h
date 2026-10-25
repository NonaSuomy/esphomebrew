#pragma once
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "psram_app.h"

/* POSIX directory iteration through the current loader's folder-list service.
 * The buffer belongs to this handle, so callers can iterate multiple folders.
 * Newlib has no VFS implementation in a standalone PAX binary. */
#ifdef __cplusplus
extern "C" {
#endif
extern const app_services_t *_pax_svc;
typedef struct {
    char *listing;
    size_t offset;
    int remaining;
    int count;
    struct dirent entry;
} pax_posix_dir_t;
DIR *opendir(const char *path) {
    if (!_pax_svc || !_pax_svc->file_list_dir || !path) { errno=ENOSYS; return NULL; }
    pax_posix_dir_t *dir=(pax_posix_dir_t *)calloc(1,sizeof(*dir));
    if (!dir) { errno=ENOMEM; return NULL; }
    dir->listing=(char *)calloc(1,65536);
    if (!dir->listing) { free(dir); errno=ENOMEM; return NULL; }
    dir->count=_pax_svc->file_list_dir(path,dir->listing,65536);
    if (dir->count<0) { free(dir->listing); free(dir); errno=ENOENT; return NULL; }
    dir->remaining=dir->count;
    return (DIR *)dir;
}
struct dirent *readdir(DIR *handle) {
    if (!handle) { errno=EBADF; return NULL; }
    pax_posix_dir_t *dir=(pax_posix_dir_t *)handle;
    if (dir->remaining<=0) return NULL;
    const char *name=dir->listing+dir->offset;
    size_t length=strlen(name);
    dir->offset+=length+1;
    dir->remaining--;
    memset(&dir->entry,0,sizeof(dir->entry));
    int folder=length>0 && name[length-1]=='/';
    if (folder) length--;
    if (length>=sizeof(dir->entry.d_name)) length=sizeof(dir->entry.d_name)-1;
    memcpy(dir->entry.d_name,name,length);
    dir->entry.d_type=folder ? DT_DIR : DT_REG;
    return &dir->entry;
}
int closedir(DIR *handle) {
    if (!handle) { errno=EBADF; return -1; }
    pax_posix_dir_t *dir=(pax_posix_dir_t *)handle;
    free(dir->listing); free(dir); return 0;
}
void rewinddir(DIR *handle) {
    if (handle) { pax_posix_dir_t *dir=(pax_posix_dir_t *)handle; dir->offset=0; dir->remaining=dir->count; }
}
long telldir(DIR *handle) {
    if (!handle) { errno=EBADF; return -1; }
    pax_posix_dir_t *dir=(pax_posix_dir_t *)handle;
    return dir->count-dir->remaining;
}
void seekdir(DIR *handle,long location) {
    if (!handle || location<0) return;
    rewinddir(handle);
    while (location-- > 0 && readdir(handle)) {}
}
#ifdef __cplusplus
}
#endif
