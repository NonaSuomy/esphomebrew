// newlib glue for the Red Alert PAX: heap, files, clocks and exit all go
// through the loader's service table (pax_svc).
//
// Heap: every block carries a small header that links it into a list, so all
// memory the game still holds is returned to the loader when it quits. The
// loader does not reclaim an app's heap on its own.
#include "pax_port.h"

#include <errno.h>
#include <fcntl.h>
#include <reent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <sys/_pthreadtypes.h>

// ── Heap ─────────────────────────────────────────────────────────────────────

typedef struct block {
    struct block *prev;
    struct block *next;
    size_t size;
    uint32_t magic;   // HEAD_MAGIC while the block is live
    void *caller;     // who allocated it (an address in the app, see .sym)
    uint32_t pad[3];  // keeps the payload 16-byte aligned like the loader's blocks
} block_t;

// Heap guards: a magic in each header and GUARD_BYTES of GUARD_FILL after each
// payload, checked on free and every few seconds (pax_heap_check). A game
// buffer overrun then shows up in the log with the block's size and owner
// instead of as a crash somewhere else later.
#define HEAD_MAGIC 0x5241484bu
#define GUARD_BYTES 16
#define GUARD_FILL 0xfd

static block_t *s_blocks = NULL;

// PSRAM only. Plain malloc() puts blocks under 16 KiB in internal RAM first,
// and the game makes thousands of small allocations: internal RAM ran out and
// the firmware aborted when it could not create a mutex (lock_init_generic
// from fclose, right after "Scratch: video mode").
static void *heap_alloc(size_t bytes)
{
    if (pax_svc->mem_caps_alloc != NULL) {
        return pax_svc->mem_caps_alloc(bytes, PAX_MEM_CAP_SPIRAM);
    }
    return pax_svc->mem_alloc(bytes);
}

static int s_guard_reports = 0;
static unsigned s_alloc_failures = 0;

static int guard_intact(const block_t *b)
{
    const unsigned char *g = (const unsigned char *)(b + 1) + b->size;
    for (int i = 0; i < GUARD_BYTES; i++) {
        if (g[i] != GUARD_FILL) {
            return 0;
        }
    }
    return 1;
}

static void report_block(const block_t *b, const char *what)
{
    if (s_guard_reports++ < 8) {
        pax_svc->log_printf("Scratch: heap guard: %s: block %p, %u bytes, allocated from %p\n", what, (void *)(b + 1),
                             (unsigned)b->size, b->caller);
    }
}

static void *block_alloc(size_t size, void *caller)
{
    if (size > SIZE_MAX - sizeof(block_t) - GUARD_BYTES) {
        return NULL;
    }
    block_t *b = (block_t *)heap_alloc(sizeof(block_t) + size + GUARD_BYTES);
    if (b == NULL) {
        // Keep failures attributable to this exact build without flooding
        // the API log if a game cache repeatedly retries an allocation.
        if (++s_alloc_failures <= 8) {
            pax_svc->log_printf("Scratch: allocation failed: %u bytes from %p (failure %u)\n",
                                 (unsigned)size, caller, s_alloc_failures);
        }
        return NULL;
    }
    b->size = size;
    b->magic = HEAD_MAGIC;
    b->caller = caller;
    memset((unsigned char *)(b + 1) + size, GUARD_FILL, GUARD_BYTES);
    b->prev = NULL;
    b->next = s_blocks;
    if (s_blocks) {
        s_blocks->prev = b;
    }
    s_blocks = b;
    return b + 1;
}

static void block_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    block_t *b = (block_t *)ptr - 1;
    if (b->magic != HEAD_MAGIC) {
        // Not ours, freed twice, or its header was overwritten: leave it alone.
        if (s_guard_reports++ < 8) {
            pax_svc->log_printf("Scratch: heap guard: free of a bad block %p (header %08x)\n", ptr, (unsigned)b->magic);
        }
        return;
    }
    if (!guard_intact(b)) {
        report_block(b, "overrun found on free");
    }
    b->magic = 0;
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        s_blocks = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
    pax_svc->mem_free(b);
}

static void *block_realloc(void *ptr, size_t size, void *caller)
{
    if (ptr == NULL) {
        return block_alloc(size, caller);
    }
    if (size == 0) {
        block_free(ptr);
        return NULL;
    }
    block_t *old = (block_t *)ptr - 1;
    if (size <= old->size) {
        return ptr;
    }
    void *fresh = block_alloc(size, caller);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, old->size);
    block_free(ptr);
    return fresh;
}

// Walk every live block; report the first damaged ones. Game task only (the
// list is not locked).
void pax_heap_check(void)
{
    int count = 0;
    for (block_t *b = s_blocks; b != NULL; b = b->next) {
        if (b->magic != HEAD_MAGIC) {
            if (s_guard_reports++ < 8) {
                pax_svc->log_printf("Scratch: heap guard: header overwritten at %p after %d blocks\n", (void *)(b + 1), count);
            }
            return; // the links are not trustworthy past this point
        }
        if (!guard_intact(b)) {
            report_block(b, "overrun");
            // Re-arm so a second overrun of the same block is reported again.
            memset((unsigned char *)(b + 1) + b->size, GUARD_FILL, GUARD_BYTES);
        }
        if (b->next != NULL && b->next->prev != b) {
            if (s_guard_reports++ < 8) {
                pax_svc->log_printf("Scratch: heap guard: broken link after block %p (%u bytes)\n", (void *)(b + 1),
                                     (unsigned)b->size);
            }
            return;
        }
        count++;
    }
}

void pax_free_all_memory(void)
{
    while (s_blocks) {
        block_t *b = s_blocks;
        s_blocks = b->next;
        pax_svc->mem_free(b);
    }
}

void *__wrap_malloc(size_t size) { return block_alloc(size, __builtin_return_address(0)); }
void __wrap_free(void *ptr) { block_free(ptr); }
void *__wrap_realloc(void *ptr, size_t size) { return block_realloc(ptr, size, __builtin_return_address(0)); }
void *__wrap_calloc(size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = block_alloc(n * size, __builtin_return_address(0));
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}
void *__wrap__malloc_r(struct _reent *r, size_t size) { (void)r; return __wrap_malloc(size); }
void __wrap__free_r(struct _reent *r, void *ptr) { (void)r; __wrap_free(ptr); }
void *__wrap__realloc_r(struct _reent *r, void *ptr, size_t size) { (void)r; return __wrap_realloc(ptr, size); }
void *__wrap__calloc_r(struct _reent *r, size_t n, size_t size) { (void)r; return __wrap_calloc(n, size); }

// Single game task: newlib's locks have nothing to protect.
struct __lock;
void __wrap___retarget_lock_init(struct __lock **lock) { if (lock) *lock = NULL; }
void __wrap___retarget_lock_init_recursive(struct __lock **lock) { if (lock) *lock = NULL; }
void __wrap___retarget_lock_close(struct __lock *lock) { (void)lock; }
void __wrap___retarget_lock_close_recursive(struct __lock *lock) { (void)lock; }
void __wrap___retarget_lock_acquire(struct __lock *lock) { (void)lock; }
int __wrap___retarget_lock_try_acquire(struct __lock *lock) { (void)lock; return 0; }
void __wrap___retarget_lock_acquire_recursive(struct __lock *lock) { (void)lock; }
int __wrap___retarget_lock_try_acquire_recursive(struct __lock *lock) { (void)lock; return 0; }
void __wrap___retarget_lock_release(struct __lock *lock) { (void)lock; }
void __wrap___retarget_lock_release_recursive(struct __lock *lock) { (void)lock; }

struct _reent *__getreent(void) { return _impure_ptr; }

void *_sbrk(ptrdiff_t incr) { (void)incr; errno = ENOMEM; return (void *)-1; }
void *_sbrk_r(struct _reent *r, ptrdiff_t incr) { (void)r; return _sbrk(incr); }

#define MAX_FDS 64
static void *s_files[MAX_FDS];
void pax_close_all_files(void) {for(int i=3;i<MAX_FDS;i++)if(s_files[i]){pax_svc->file_close(s_files[i]);s_files[i]=NULL;}}
int _open(const char *path,int flags,int mode) {
 (void)mode;int fd=3;while(fd<MAX_FDS&&s_files[fd])fd++;if(fd==MAX_FDS){errno=EMFILE;return -1;}
 int access=flags&O_ACCMODE;
 const char *m=(flags&O_APPEND)?(access==O_RDWR?"a+b":"ab"):access==O_RDONLY?"rb":(flags&O_TRUNC)?(access==O_RDWR?"w+b":"wb"):"r+b";
 s_files[fd]=pax_svc->file_open(path,m);
 if(!s_files[fd]&&(flags&O_CREAT))s_files[fd]=pax_svc->file_open(path,access==O_RDWR?"w+b":"wb");
 if(!s_files[fd]){errno=ENOENT;return -1;}return fd;
}
int _close(int fd){if(fd<3||fd>=MAX_FDS||!s_files[fd]){errno=EBADF;return -1;}int r=pax_svc->file_close(s_files[fd]);s_files[fd]=NULL;return r;}
ssize_t _read(int fd,void *buf,size_t count){if(fd<3||fd>=MAX_FDS||!s_files[fd]){errno=EBADF;return -1;}return pax_svc->file_read(buf,1,count,s_files[fd]);}
ssize_t _write(int fd,const void *buf,size_t count){if(fd==1||fd==2){pax_svc->log_printf("%.*s",(int)count,(const char*)buf);return count;}if(fd<3||fd>=MAX_FDS||!s_files[fd]){errno=EBADF;return -1;}return pax_svc->file_write(buf,1,count,s_files[fd]);}
off_t _lseek(int fd,off_t off,int whence){if(fd<3||fd>=MAX_FDS||!s_files[fd]){errno=EBADF;return -1;}if(pax_svc->file_seek(s_files[fd],off,whence)!=0)return -1;return pax_svc->file_tell(s_files[fd]);}
int _fstat(int fd,struct stat *st){memset(st,0,sizeof(*st));if(fd<3){st->st_mode=S_IFCHR;return 0;}if(fd>=MAX_FDS||!s_files[fd]){errno=EBADF;return -1;}long at=pax_svc->file_tell(s_files[fd]);pax_svc->file_seek(s_files[fd],0,SEEK_END);st->st_size=pax_svc->file_tell(s_files[fd]);pax_svc->file_seek(s_files[fd],at,SEEK_SET);st->st_mode=S_IFREG|0644;return 0;}
int _stat(const char *p,struct stat *st){pax_file_stat_t info;memset(st,0,sizeof(*st));if(!pax_svc->file_stat||pax_svc->file_stat(p,&info)!=0){errno=ENOENT;return -1;}st->st_mode=(info.is_dir?S_IFDIR:S_IFREG)|0644;st->st_size=info.size;st->st_mtime=info.mtime;return 0;}
int mkdir(const char *p,mode_t mode){(void)mode;return pax_svc->file_mkdir?pax_svc->file_mkdir(p):-1;}
int _unlink(const char *p){return pax_svc->file_remove?pax_svc->file_remove(p):-1;}
int _link(const char *a,const char *b){(void)a;(void)b;errno=ENOSYS;return -1;}
int _isatty(int fd){return fd>=0&&fd<=2;}
int _kill(int pid,int sig){(void)pid;(void)sig;errno=EINVAL;return -1;}
int _getpid(void){return 1;}
int _gettimeofday(struct timeval *tv,void *tz){(void)tz;long long us=pax_time_us();tv->tv_sec=us/1000000;tv->tv_usec=us%1000000;return 0;}
int clock_gettime(clockid_t id,struct timespec *t){(void)id;long long us=pax_time_us();t->tv_sec=us/1000000;t->tv_nsec=(us%1000000)*1000;return 0;}
int nanosleep(const struct timespec *req,struct timespec *rem){pax_svc->delay_ms(req->tv_sec*1000+(req->tv_nsec+999999)/1000000);if(rem)memset(rem,0,sizeof(*rem));return 0;}
int usleep(useconds_t us){pax_svc->delay_ms((us+999)/1000);return 0;}
void _exit(int code){pax_quit(code);}
void abort(void){pax_svc->log_printf("Scratch: runtime aborted (invalid project or out of memory)\n");pax_quit(1);}
void __cxa_pure_virtual(void){abort();}
void *__dso_handle=&__dso_handle;
int __cxa_atexit(void (*fn)(void*),void *arg,void *dso){(void)fn;(void)arg;(void)dso;return 0;}
int atexit(void (*fn)(void)){(void)fn;return 0;}

int pthread_mutex_init(void *mutex, const void *attr) { (void)mutex; (void)attr; return 0; }
int pthread_mutex_destroy(void *mutex) { (void)mutex; return 0; }
int pthread_mutex_lock(void *mutex) { (void)mutex; return 0; }
int pthread_mutex_trylock(void *mutex) { (void)mutex; return 0; }
int pthread_mutex_unlock(void *mutex) { (void)mutex; return 0; }

#define MAX_KEYS 16
static const void *s_key_values[MAX_KEYS];
static unsigned long s_keys_used;

int pthread_key_create(unsigned long *key, void (*destructor)(void *))
{
    (void)destructor;
    if (s_keys_used >= MAX_KEYS) {
        return EAGAIN;
    }
    *key = s_keys_used++;
    return 0;
}
int pthread_key_delete(unsigned long key) { (void)key; return 0; }
void *pthread_getspecific(unsigned long key) { return key < MAX_KEYS ? (void *)s_key_values[key] : NULL; }
int pthread_setspecific(unsigned long key, const void *value)
{
    if (key >= MAX_KEYS) {
        return EINVAL;
    }
    s_key_values[key] = value;
    return 0;
}


int pthread_mutexattr_init(void *a){(void)a;return 0;}
int pthread_mutexattr_destroy(void *a){(void)a;return 0;}
int pthread_mutexattr_settype(void *a,int type){(void)a;(void)type;return 0;}
int pthread_cond_init(void *c,const void *a){(void)c;(void)a;return 0;}
int pthread_cond_broadcast(void *c){(void)c;return 0;}
int pthread_cond_wait(void *c,void *m){(void)c;(void)m;errno=ENOSYS;return ENOSYS;}
int pthread_once(pthread_once_t *once,void (*fn)(void)){
    // ESP newlib uses {is_initialized, init_executed}, initialized to {1,0}.
    // The C++ library calls this to initialize locale and guard state.
    if(!once->init_executed){once->init_executed=1;fn();}return 0;
}

// newlib's rename() is pulled in by archive export; use the current PAX
// storage service so a temporary archive is renamed on the device filesystem.
int _rename(const char *from,const char *to){
    if(!pax_svc->file_rename){errno=ENOSYS;return -1;}
    return pax_svc->file_rename(from,to);
}
