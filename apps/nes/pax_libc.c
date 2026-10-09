/* libc/file ABI adapters. PAX code never touches the launcher's VFS directly. */
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const app_services_t *_pax_svc;

FILE *__wrap_fopen(const char *p, const char *m) { return (FILE *)_pax_svc->file_open(p, m); }
int __wrap_fclose(FILE *f) { return _pax_svc->file_close((void *)f); }
size_t __wrap_fread(void *p, size_t s, size_t n, FILE *f) { return _pax_svc->file_read(p,s,n,(void *)f); }
size_t __wrap_fwrite(const void *p, size_t s, size_t n, FILE *f) { return _pax_svc->file_write(p,s,n,(void *)f); }
int __wrap_fseek(FILE *f, long o, int w) { return _pax_svc->file_seek((void *)f,o,w); }
long __wrap_ftell(FILE *f) { return _pax_svc->file_tell((void *)f); }
