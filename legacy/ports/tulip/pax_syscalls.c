// newlib glue for the Tulip PAX: heap, files, clocks, locks and exit all go
// through the loader's service table (pax_svc). Same approach as the Red
// Alert and OpenLara ports, with locking added: Tulip runs MicroPython, the
// display compositor and AMY on three tasks that all allocate.
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

// ── Locks ──────────────────────────────────────────────────────────────────
// The loader offers tasks but no mutex, so the app's tasks share spinlocks.
// The lock word lives in internal RAM (atomics on PSRAM are not something to
// rely on); a waiter that spins for a while sleeps one tick, so a lower
// priority holder on the same core gets to finish.

struct pax_lock {
    volatile uint32_t word;
};

static pax_lock_t s_static_locks[4];  // before the loader's heap is usable
static int s_static_used = 0;

pax_lock_t *pax_lock_new(void)
{
    pax_lock_t *lock = NULL;
    if (pax_svc != NULL && pax_svc->mem_caps_alloc != NULL) {
        lock = (pax_lock_t *)pax_svc->mem_caps_alloc(sizeof(pax_lock_t), PAX_MEM_CAP_INTERNAL);
    }
    if (lock == NULL && s_static_used < (int)(sizeof(s_static_locks) / sizeof(s_static_locks[0]))) {
        lock = &s_static_locks[s_static_used++];
    }
    if (lock != NULL) {
        lock->word = 0;
    }
    return lock;
}

void pax_lock_take(pax_lock_t *lock)
{
    unsigned spins = 0;
    while (__atomic_exchange_n(&lock->word, 1u, __ATOMIC_ACQUIRE) != 0) {
        if (++spins >= 2000) {
            spins = 0;
            pax_svc->delay_ms(1);
        }
    }
}

void pax_lock_give(pax_lock_t *lock)
{
    __atomic_store_n(&lock->word, 0u, __ATOMIC_RELEASE);
}

static pax_lock_t *s_heap_lock;
static pax_lock_t *s_atomic_lock;
static pax_lock_t *s_amy_lock;
static pax_lock_t *s_file_lock;

static void locks_init(void)
{
    if (s_heap_lock == NULL) {
        s_heap_lock = pax_lock_new();
        s_atomic_lock = pax_lock_new();
        s_amy_lock = pax_lock_new();
        s_file_lock = pax_lock_new();
    }
}

unsigned pax_atomic_begin(void)
{
    pax_lock_take(s_atomic_lock);
    return 1;
}

void pax_atomic_end(unsigned state)
{
    (void)state;
    pax_lock_give(s_atomic_lock);
}

void pax_amy_lock(int grab)
{
    if (grab) {
        pax_lock_take(s_amy_lock);
    } else {
        pax_lock_give(s_amy_lock);
    }
}

// ── Heap ─────────────────────────────────────────────────────────────────────
// Every block carries a small header that links it into a list, so all
// memory Tulip still holds goes back to the loader when it quits (the loader
// does not reclaim an app's heap). Everything is PSRAM: MicroPython, LVGL and
// AMY make many small allocations, and plain malloc() would put those under
// 16 KiB into the loader's scarce internal RAM.

typedef struct block {
    struct block *prev;
    struct block *next;
    size_t size;
    uint32_t magic;   // HEAD_MAGIC while the block is live
    uint32_t pad[4];  // keeps the payload 16-byte aligned like the loader's blocks
} block_t;

#define HEAD_MAGIC 0x54554c50u  // "TULP"

static block_t *s_blocks = NULL;
static int s_bad_frees = 0;

void *pax_alloc_raw(size_t size, int internal)
{
    if (pax_svc->mem_caps_alloc != NULL) {
        void *p = pax_svc->mem_caps_alloc(size, internal ? (PAX_MEM_CAP_INTERNAL | PAX_MEM_CAP_DMA)
                                                          : PAX_MEM_CAP_SPIRAM);
        if (p != NULL || internal) {
            return p;
        }
    }
    return internal ? NULL : pax_svc->mem_alloc(size);
}

void pax_free_raw(void *ptr)
{
    if (ptr != NULL) {
        pax_svc->mem_free(ptr);
    }
}

static void *block_alloc(size_t size)
{
    if (size > SIZE_MAX - sizeof(block_t)) {
        return NULL;
    }
    block_t *b = (block_t *)pax_alloc_raw(sizeof(block_t) + size, 0);
    if (b == NULL) {
        return NULL;
    }
    b->size = size;
    b->magic = HEAD_MAGIC;
    b->prev = NULL;
    pax_lock_take(s_heap_lock);
    b->next = s_blocks;
    if (s_blocks) {
        s_blocks->prev = b;
    }
    s_blocks = b;
    pax_lock_give(s_heap_lock);
    return b + 1;
}

static void block_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    block_t *b = (block_t *)ptr - 1;
    pax_lock_take(s_heap_lock);
    if (b->magic != HEAD_MAGIC) {
        pax_lock_give(s_heap_lock);
        // Not ours, freed twice, or its header was overwritten: leave it alone.
        if (s_bad_frees++ < 8) {
            pax_svc->log_printf("TULIP: free of a bad block %p (header %08x)\n", ptr, (unsigned)b->magic);
        }
        return;
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
    pax_lock_give(s_heap_lock);
    pax_svc->mem_free(b);
}

static void *block_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return block_alloc(size);
    }
    if (size == 0) {
        block_free(ptr);
        return NULL;
    }
    block_t *old = (block_t *)ptr - 1;
    if (size <= old->size) {
        return ptr;
    }
    void *fresh = block_alloc(size);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, old->size);
    block_free(ptr);
    return fresh;
}

void pax_free_all_memory(void)
{
    pax_lock_take(s_heap_lock);
    while (s_blocks) {
        block_t *b = s_blocks;
        s_blocks = b->next;
        b->magic = 0;
        pax_svc->mem_free(b);
    }
    pax_lock_give(s_heap_lock);
}

void *__wrap_malloc(size_t size) { return block_alloc(size); }
void __wrap_free(void *ptr) { block_free(ptr); }
void *__wrap_realloc(void *ptr, size_t size) { return block_realloc(ptr, size); }
void *__wrap_calloc(size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = block_alloc(n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}
void *__wrap__malloc_r(struct _reent *r, size_t size) { (void)r; return __wrap_malloc(size); }
void __wrap__free_r(struct _reent *r, void *ptr) { (void)r; __wrap_free(ptr); }
void *__wrap__realloc_r(struct _reent *r, void *ptr, size_t size) { (void)r; return __wrap_realloc(ptr, size); }
void *__wrap__calloc_r(struct _reent *r, size_t n, size_t size) { (void)r; return __wrap_calloc(n, size); }

// newlib's own locks (stdio, environment) guard nothing here: each task
// writes whole log lines, and only the MicroPython task opens files.
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

// ── Loader file handles ─────────────────────────────────────────────────────
// Every file the app opens (newlib fds, the /sd VFS, the filesystem image) is
// remembered, so it can be closed when Tulip quits.

#define MAX_FILES 48
static void *s_open_files[MAX_FILES];

void *pax_file_open(const char *path, const char *mode)
{
    void *fp = pax_svc->file_open(path, mode);
    if (fp == NULL) {
        return NULL;
    }
    pax_lock_take(s_file_lock);
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_open_files[i] == NULL) {
            s_open_files[i] = fp;
            pax_lock_give(s_file_lock);
            return fp;
        }
    }
    pax_lock_give(s_file_lock);
    pax_svc->file_close(fp);  // table full
    return NULL;
}

int pax_file_close(void *fp)
{
    if (fp == NULL) {
        return -1;
    }
    pax_lock_take(s_file_lock);
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_open_files[i] == fp) {
            s_open_files[i] = NULL;
        }
    }
    pax_lock_give(s_file_lock);
    return pax_svc->file_close(fp);
}

long pax_file_size(void *fp)
{
    long here = pax_svc->file_tell(fp);
    pax_svc->file_seek(fp, 0, SEEK_END);
    long size = pax_svc->file_tell(fp);
    pax_svc->file_seek(fp, here, SEEK_SET);
    return size;
}

void pax_close_all_files(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_open_files[i] != NULL) {
            pax_svc->file_close(s_open_files[i]);
            s_open_files[i] = NULL;
        }
    }
}

// ── newlib file descriptors: fds 3.. are loader files ──────────────────────
// Used by C code that calls fopen() (lodepng, AMY sample files). MicroPython
// itself goes through its VFS (pax_vfs.c).

#define MAX_FDS 16
static void *s_fds[MAX_FDS];

static void *fd_file(int fd)
{
    return (fd >= 3 && fd < MAX_FDS) ? s_fds[fd] : NULL;
}

static const char *open_mode(int flags)
{
    const int access = flags & O_ACCMODE;
    if (flags & O_APPEND) {
        return access == O_RDWR ? "a+b" : "ab";
    }
    if (access == O_RDONLY) {
        return "rb";
    }
    if (flags & O_TRUNC || (flags & O_CREAT && access == O_WRONLY)) {
        return access == O_RDWR ? "w+b" : "wb";
    }
    return "r+b";
}

int _open(const char *path, int flags, int mode)
{
    (void)mode;
    int fd = 3;
    while (fd < MAX_FDS && s_fds[fd]) {
        fd++;
    }
    if (fd == MAX_FDS) {
        errno = EMFILE;
        return -1;
    }
    void *fp = pax_file_open(path, open_mode(flags));
    if (fp == NULL && (flags & O_CREAT) && (flags & O_ACCMODE) == O_RDWR) {
        fp = pax_file_open(path, "w+b");  // "r+b" needs the file to exist
    }
    if (fp == NULL) {
        errno = ENOENT;
        return -1;
    }
    s_fds[fd] = fp;
    return fd;
}

int _close(int fd)
{
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    s_fds[fd] = NULL;
    return pax_file_close(fp);
}

ssize_t _read(int fd, void *buf, size_t count)
{
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    return (ssize_t)pax_svc->file_read(buf, 1, count, fp);
}

// stdout/stderr (printf from Tulip, LVGL and AMY): the loader's log, one line
// per call so lines from different tasks do not interleave mid-line.
ssize_t _write(int fd, const void *buf, size_t count)
{
    if (fd == 1 || fd == 2) {
        static char line[256];
        static size_t used = 0;
        const char *s = (const char *)buf;
        pax_lock_take(s_file_lock);
        for (size_t i = 0; i < count; i++) {
            if (s[i] == '\n' || used == sizeof(line) - 1) {
                line[used] = '\0';
                pax_svc->log_printf("%s\n", line);
                used = 0;
                if (s[i] == '\n') {
                    continue;
                }
            }
            line[used++] = s[i];
        }
        pax_lock_give(s_file_lock);
        return (ssize_t)count;
    }
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    return (ssize_t)pax_svc->file_write(buf, 1, count, fp);
}

off_t _lseek(int fd, off_t offset, int whence)
{
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    if (pax_svc->file_seek(fp, (long)offset, whence) != 0) {
        errno = EINVAL;
        return -1;
    }
    return (off_t)pax_svc->file_tell(fp);
}

int _fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (fd >= 0 && fd <= 2) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    st->st_mode = S_IFREG | 0644;
    st->st_size = pax_file_size(fp);
    return 0;
}

// The loader has no stat(): a path is a file when it opens.
int _stat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    void *fp = pax_svc->file_open(path, "rb");
    if (fp == NULL) {
        errno = ENOENT;
        return -1;
    }
    st->st_mode = S_IFREG | 0644;
    st->st_size = pax_file_size(fp);
    pax_svc->file_close(fp);
    return 0;
}

int _unlink(const char *path) { (void)path; errno = EACCES; return -1; }
int _link(const char *a, const char *b) { (void)a; (void)b; errno = EMLINK; return -1; }
int _rename(const char *a, const char *b) { (void)a; (void)b; errno = EACCES; return -1; }
int _isatty(int fd) { return fd >= 0 && fd <= 2; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }

int _open_r(struct _reent *r, const char *path, int flags, int mode) { (void)r; return _open(path, flags, mode); }
int _close_r(struct _reent *r, int fd) { (void)r; return _close(fd); }
ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t n) { (void)r; return _read(fd, buf, n); }
ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t n) { (void)r; return _write(fd, buf, n); }
off_t _lseek_r(struct _reent *r, int fd, off_t off, int whence) { (void)r; return _lseek(fd, off, whence); }
int _fstat_r(struct _reent *r, int fd, struct stat *st) { (void)r; return _fstat(fd, st); }
int _stat_r(struct _reent *r, const char *path, struct stat *st) { (void)r; return _stat(path, st); }
int _unlink_r(struct _reent *r, const char *path) { (void)r; return _unlink(path); }
int _link_r(struct _reent *r, const char *a, const char *b) { (void)r; return _link(a, b); }
int _rename_r(struct _reent *r, const char *a, const char *b) { (void)r; return _rename(a, b); }
int _isatty_r(struct _reent *r, int fd) { (void)r; return _isatty(fd); }
int _kill_r(struct _reent *r, int pid, int sig) { (void)r; return _kill(pid, sig); }
int _getpid_r(struct _reent *r) { (void)r; return 1; }

// ── Time ─────────────────────────────────────────────────────────────────────

int64_t pax_time_us(void)
{
    return pax_svc->get_time_us();
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        int64_t us = pax_time_us();
        tv->tv_sec = (time_t)(us / 1000000);
        tv->tv_usec = (suseconds_t)(us % 1000000);
    }
    return 0;
}
int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz) { (void)r; return _gettimeofday(tv, tz); }

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    (void)clock_id;  // REALTIME and MONOTONIC: both time since boot
    int64_t us = pax_time_us();
    tp->tv_sec = (time_t)(us / 1000000);
    tp->tv_nsec = (long)(us % 1000000) * 1000;
    return 0;
}

int usleep(useconds_t us)
{
    pax_svc->delay_ms(us >= 1000 ? (int)(us / 1000) : 1);
    return 0;
}

unsigned sleep(unsigned seconds)
{
    pax_svc->delay_ms((int)seconds * 1000);
    return 0;
}

// ── Exit ─────────────────────────────────────────────────────────────────────

void __wrap_exit(int code) { pax_quit(code); }
void _exit(int code) { pax_quit(code); }
void abort(void)
{
    pax_svc->log_printf("TULIP: abort()\n");
    pax_quit(-1);
}

void *__dso_handle = &__dso_handle;
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
int atexit(void (*fn)(void)) { (void)fn; return 0; }

// Called first by app_entry.
void pax_syscalls_init(void)
{
    locks_init();
}

// Called last by app_entry, once no other task of the app runs.
void pax_syscalls_deinit(void)
{
    pax_lock_t **locks[] = {&s_heap_lock, &s_atomic_lock, &s_amy_lock, &s_file_lock};
    for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); i++) {
        pax_lock_t *lock = *locks[i];
        const int is_static = lock >= &s_static_locks[0] &&
                              lock < &s_static_locks[sizeof(s_static_locks) / sizeof(s_static_locks[0])];
        if (lock != NULL && !is_static) {
            pax_svc->mem_free(lock);
        }
        *locks[i] = NULL;
    }
}
