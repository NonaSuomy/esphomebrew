/* FILE objects belong to the launcher: never let app-newlib dereference them.
 * Track handles so a fatal core error can unwind without leaking SD files. */
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <string.h>
#include <stdint.h>
extern const app_services_t *_pax_svc;
extern ssize_t _write(int fd, const void *buf, size_t count);
extern int pax_file_service_path(const char *path, char *out, size_t capacity);
static FILE *streams[32];
// File-open/close services can block on SD while the other core starts or
// stops a music decoder. Reserve the slot before crossing into firmware;
// publishing NULL early on close lets another open reuse a live cache.
#define RESERVED_STREAM ((FILE *)(uintptr_t)-1)
struct read_cache { unsigned char *bytes; size_t at, count; int read_only; };
static struct read_cache caches[32];
enum { READ_CACHE_BYTES=4096 };

static struct read_cache *stream_cache(FILE *f)
{
    for (unsigned i=0; i<32; ++i)
        if (__atomic_load_n(&streams[i],__ATOMIC_ACQUIRE)==f) return &caches[i];
    return NULL;
}

static int synchronize_read(FILE *f, struct read_cache *cache)
{
    if (cache && cache->count>cache->at &&
        _pax_svc->file_seek(f,-(long)(cache->count-cache->at),SEEK_CUR)) return -1;
    if (cache) cache->at=cache->count=0;
    return 0;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    char normalized[1024];
    if (pax_file_service_path(path,normalized,sizeof(normalized))) return NULL;
    if (!_pax_svc || !_pax_svc->file_open) { errno=ENOSYS; return NULL; }
    for (unsigned i=0; i<32; ++i) {
        FILE *empty=NULL;
        if (!__atomic_compare_exchange_n(&streams[i],&empty,RESERVED_STREAM,0,
                                        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) continue;
        FILE *opened=(FILE *)_pax_svc->file_open(normalized,mode);
        caches[i].read_only=mode && mode[0]=='r' && !strchr(mode,'+');
        __atomic_store_n(&streams[i],opened,__ATOMIC_RELEASE);
        return opened;
    }
    errno = EMFILE;
    return NULL;
}
int __wrap_fclose(FILE *f)
{
    if (!f) return EOF;
    for (unsigned i=0; i<32; ++i) {
        FILE *expected=f;
        if (!__atomic_compare_exchange_n(&streams[i],&expected,RESERVED_STREAM,0,
                                        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) continue;
        if (caches[i].bytes) _pax_svc->mem_free(caches[i].bytes);
        memset(&caches[i],0,sizeof(caches[i]));
        const int result=_pax_svc->file_close(f);
        __atomic_store_n(&streams[i],NULL,__ATOMIC_RELEASE);
        return result;
    }
    errno = EBADF;
    return EOF;
}
size_t __wrap_fread(void *p, size_t s, size_t n, FILE *f)
{
    if (!s || !n) return 0;
    if (s>SIZE_MAX/n) { errno=EOVERFLOW; return 0; }
    struct read_cache *cache=stream_cache(f);
    if (!cache || !cache->read_only) return _pax_svc->file_read(p,s,n,f);
    const size_t requested=s*n;
    // stb_vorbis frequently reads a byte at a time. Buffer the launcher
    // service calls instead of crossing the PAX boundary for every byte.
    if (!cache->bytes && requested<READ_CACHE_BYTES && _pax_svc->mem_caps_alloc)
        cache->bytes=(unsigned char *)_pax_svc->mem_caps_alloc(READ_CACHE_BYTES,PAX_MEM_CAP_SPIRAM);
    if (!cache->bytes) return _pax_svc->file_read(p,s,n,f);
    size_t copied=0;
    while (copied<requested) {
        if (cache->at==cache->count) {
            cache->at=cache->count=0;
            if (requested-copied>=READ_CACHE_BYTES) {
                copied+=_pax_svc->file_read((unsigned char *)p+copied,1,requested-copied,f);
                break;
            }
            cache->count=_pax_svc->file_read(cache->bytes,1,READ_CACHE_BYTES,f);
            if (!cache->count) break;
        }
        const size_t left=requested-copied, available=cache->count-cache->at;
        const size_t take=left<available ? left : available;
        memcpy((unsigned char *)p+copied,cache->bytes+cache->at,take);
        copied+=take; cache->at+=take;
    }
    return copied/s;
}
size_t __wrap_fwrite(const void *p, size_t s, size_t n, FILE *f)
{
    /* stdout/stderr are app-newlib FILEs, not launcher-owned SD tokens. */
    if (f == stdout || f == stderr) {
        if (!s || (n && s > (size_t)-1 / n)) return 0;
        const ssize_t bytes = _write(f == stderr ? 2 : 1, p, s * n);
        return bytes < 0 ? 0 : (size_t)bytes / s;
    }
    if (synchronize_read(f,stream_cache(f))) return 0;
    return _pax_svc->file_write(p, s, n, f);
}
int __wrap_fseek(FILE *f, long o, int w)
{
    struct read_cache *cache=stream_cache(f);
    // SF2 parsers skip short fields between reads. Seeking within read-ahead
    // must not throw away that buffer and force another SD read each time.
    if (cache && cache->bytes && cache->count) {
        if (w==SEEK_CUR && o>=-(long)cache->at && o<=(long)(cache->count-cache->at)) {
            cache->at=(size_t)((long)cache->at+o);
            return 0;
        }
        if (w==SEEK_SET) {
            const long physical=_pax_svc->file_tell(f);
            if (physical>=(long)cache->count && o>=physical-(long)cache->count && o<=physical) {
                cache->at=(size_t)(o-(physical-(long)cache->count));
                return 0;
            }
        }
    }
    // SEEK_CUR refers to the logical position, not the read-ahead position.
    if (w==SEEK_CUR && synchronize_read(f,cache)) return -1;
    const int result=_pax_svc->file_seek(f,o,w);
    if (!result && cache) cache->at=cache->count=0;
    return result;
}
long __wrap_ftell(FILE *f)
{
    const struct read_cache *cache=stream_cache(f);
    const long position=_pax_svc->file_tell(f);
    return position<0 || !cache ? position : position-(long)(cache->count-cache->at);
}
void __wrap_rewind(FILE *f)
{
    if (_pax_svc && f)
        (void)__wrap_fseek(f, 0, SEEK_SET);
}

/* Newlib's getc() normally dereferences its private FILE object. PAX file
 * handles are opaque launcher-owned tokens, so route byte reads through the
 * service table just like fread(). Spectrum's tape/config readers use getc
 * extensively and otherwise fault as soon as they open a sidecar ROM. */
int __wrap_getc(FILE *f)
{
    unsigned char c = 0;
    if (!f || !_pax_svc || !_pax_svc->file_read ||
        __wrap_fread(&c, 1, 1, f) != 1)
        return EOF;
    return (int)c;
}
int __wrap_fgetc(FILE *f) { return __wrap_getc(f); }

void pax_close_streams(void)
{
    for (unsigned i=0; i<32; ++i) {
        FILE *f=__atomic_load_n(&streams[i],__ATOMIC_ACQUIRE);
        if (f && f!=RESERVED_STREAM) __wrap_fclose(f);
    }
}
