#define PAX_APP_SIDE 1

#include "psram_app.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/reent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

extern const app_services_t *_pax_svc;
extern volatile int pax_exit_requested;

static void *s_fd[16];

// C allocations must use the same launcher heap as C++/Lua/SDL. Newlib's
// private _sbrk heap is unavailable in a freestanding PAX image.
void *__wrap_malloc(size_t size)
{
	if (!_pax_svc) return NULL;
	void *p = _pax_svc->mem_caps_alloc ?
		_pax_svc->mem_caps_alloc(size ? size : 1, PAX_MEM_CAP_SPIRAM) : NULL;
	return p ? p : (_pax_svc->mem_alloc ? _pax_svc->mem_alloc(size ? size : 1) : NULL);
}
void *__wrap_calloc(size_t count, size_t size)
{
	if (count && size > (size_t)-1 / count) { errno = ENOMEM; return NULL; }
	const size_t bytes = count * size;
	void *p = __wrap_malloc(bytes);
	if (p) __builtin_memset(p, 0, bytes);
	return p;
}
void __wrap_free(void *p)
{
	if (p && _pax_svc && _pax_svc->mem_free) _pax_svc->mem_free(p);
}
void *__wrap_realloc(void *p, size_t size)
{
	if (!p) return __wrap_malloc(size);
	if (!size) { __wrap_free(p); return NULL; }
	return _pax_svc && _pax_svc->mem_realloc ? _pax_svc->mem_realloc(p, size) : NULL;
}

static void *fd_file(int fd)
{
	return (fd >= 3 && fd < (int)(sizeof(s_fd) / sizeof(s_fd[0]))) ? s_fd[fd] : NULL;
}

// Desktop campaign setup paths contain /./ components. The launcher rejects
// those before FatFs sees them; remove only redundant dots/slashes, leaving
// parent components intact so the launcher's containment checks still apply.
int pax_file_service_path(const char *path, char *out, size_t capacity)
{
	if (!path || !out || capacity < 2) { errno = EINVAL; return -1; }
	size_t used = 0;
	if (*path == '/') { out[used++] = '/'; ++path; }
	while (*path) {
		while (*path == '/') ++path;
		const char *component = path;
		while (*path && *path != '/') ++path;
		size_t length = (size_t)(path - component);
		if (!length || (length == 1 && component[0] == '.')) continue;
		if (used && out[used - 1] != '/') {
			if (used + 1 >= capacity) { errno = ENAMETOOLONG; return -1; }
			out[used++] = '/';
		}
		if (length >= capacity - used) { errno = ENAMETOOLONG; return -1; }
		__builtin_memcpy(out + used, component, length); used += length;
	}
	if (!used) out[used++] = '.';
	out[used] = 0;
	return 0;
}

int _open(const char *path, int flags, int mode)
{
	(void)mode;
	if (!path) { errno = EINVAL; return -1; }
	if (!_pax_svc || !_pax_svc->file_open) { errno = ENOSYS; return -1; }
	char normalized[1024];
	if (pax_file_service_path(path, normalized, sizeof(normalized))) return -1;
	const char *m = (flags & 3) == 1 ? "wb" : ((flags & 3) == 2 ? "rb+" : "rb");
	void *f = _pax_svc->file_open(normalized, m);
	// Launcher/newlib errno lives in a different runtime from the PAX.
	// Do not report an unrelated stale allocation error for a missing read.
	if (!f) { errno = (flags & 3) == 0 ? ENOENT : EIO; return -1; }
	for (int i = 3; i < (int)(sizeof(s_fd) / sizeof(s_fd[0])); ++i) {
		if (!s_fd[i]) { s_fd[i] = f; return i; }
	}
	_pax_svc->file_close(f);
	errno = EMFILE;
	return -1;
}

int _close(int fd)
{
	void *f = fd_file(fd);
	if (!f || !_pax_svc || !_pax_svc->file_close) return -1;
	s_fd[fd] = NULL;
	return _pax_svc->file_close(f);
}

ssize_t _read(int fd, void *buf, size_t count)
{
	void *f = fd_file(fd);
	if (!f || !_pax_svc || !_pax_svc->file_read) return -1;
	return (ssize_t)_pax_svc->file_read(buf, 1, count, f);
}

ssize_t _write(int fd, const void *buf, size_t count)
{
	if (fd == 1 || fd == 2) {
		if (_pax_svc && _pax_svc->log_printf && buf) {
			static char line[160];
			static size_t used;
			static int logging;
			if (logging) return (ssize_t)count;
			const char *text = (const char *)buf;
			for (size_t pos = 0; pos < count; ++pos) {
				if (text[pos] != '\r' && text[pos] != '\n') line[used++] = text[pos];
				if (text[pos] == '\n' || used == sizeof(line) - 1) {
					line[used] = 0;
					logging = 1;
					if (used) _pax_svc->log_printf("Stargus console: %s", line);
					logging = 0;
					used = 0;
				}
			}
		}
		return (ssize_t)count;
	}
	void *f = fd_file(fd);
	if (!f || !_pax_svc || !_pax_svc->file_write) return -1;
	return (ssize_t)_pax_svc->file_write(buf, 1, count, f);
}

off_t _lseek(int fd, off_t offset, int whence)
{
	void *f = fd_file(fd);
	if (!f || !_pax_svc || _pax_svc->file_seek(f, (long)offset, whence) != 0) return -1;
	return (off_t)_pax_svc->file_tell(f);
}

int _fstat(int fd, struct stat *st)
{
	if (!st) return -1;
	__builtin_memset(st, 0, sizeof(*st));
	st->st_mode = fd_file(fd) ? S_IFREG : S_IFCHR;
	return 0;
}

int _isatty(int fd) { return fd >= 0 && fd <= 2; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
int _fcntl(int fd, int cmd, int arg) { (void)fd; (void)cmd; (void)arg; return 0; }
int _stat(const char *path, struct stat *st)
{
	if (!path || !st) { errno = EINVAL; return -1; }
	if (!_pax_svc || !_pax_svc->file_stat) { errno = ENOSYS; return -1; }
	char normalized[1024];
	if (pax_file_service_path(path, normalized, sizeof(normalized))) return -1;
	pax_file_stat_t info;
	if (_pax_svc->file_stat(normalized, &info) != 0) { errno = ENOENT; return -1; }
	__builtin_memset(st, 0, sizeof(*st));
	st->st_mode = info.is_dir ? S_IFDIR : S_IFREG;
	st->st_size = (off_t)info.size;
	return 0;
}
clock_t _times(struct tms *buf) { if (buf) __builtin_memset(buf, 0, sizeof(*buf)); return 0; }
void *_sbrk(ptrdiff_t incr) { (void)incr; return (void *)-1; }

int _gettimeofday(struct timeval *tv, void *tz)
{
	(void)tz;
	if (tv && _pax_svc && _pax_svc->get_time_us) {
		int64_t us = _pax_svc->get_time_us();
		tv->tv_sec = us / 1000000;
		tv->tv_usec = us % 1000000;
	}
	return 0;
}

struct _reent *__getreent(void) { return _impure_ptr; }
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
int chmod(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
char *getcwd(char *buf, size_t size) { if (buf && size) { buf[0] = '/'; if (size > 1) buf[1] = 0; return buf; } return NULL; }
int execvp(const char *file, char *const argv[]) { (void)file; (void)argv; errno = ENOSYS; return -1; }
long pathconf(const char *path, int name) { (void)path; (void)name; return -1; }
void (*signal(int sig, void (*handler)(int)))(int) { (void)sig; return handler; }
int _rename(const char *old_path, const char *new_path) { (void)old_path; (void)new_path; errno = ENOSYS; return -1; }
int _unlink(const char *path) { (void)path; errno = ENOSYS; return -1; }
void _exit(int status) {
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("Stargus fatal exit: %d\n", status);
	pax_exit_requested = 1;
	for (;;) {
		if (_pax_svc && _pax_svc->delay_ms) _pax_svc->delay_ms(100);
	}
}

int pthread_once(unsigned long *control, void (*init_routine)(void))
{
	if (control && *control == 0) { *control = 1; if (init_routine) init_routine(); }
	return 0;
}
int pthread_key_create(unsigned long *key, void (*destructor)(void *))
{
	// This service ABI has no task-local storage. Do not advertise working
	// pthread keys: libsupc++ otherwise saves exception state through the
	// no-op setspecific(), then dereferences NULL in __cxa_end_catch().
	// Returning an error selects its built-in non-pthread EH storage. All
	// engine/Guichan exceptions belong to the single engine worker; audio
	// workers must not use C++ exceptions until real per-task TLS is added.
	(void)key;
	(void)destructor;
	return ENOSYS;
}
int pthread_key_delete(unsigned long key) { (void)key; return EINVAL; }
int pthread_mutex_lock(void *mutex) { (void)mutex; return 0; }
int pthread_mutex_unlock(void *mutex) { (void)mutex; return 0; }
int pthread_cond_init(void *cond, const void *attr) { (void)cond; (void)attr; return 0; }
int pthread_cond_wait(void *cond, void *mutex) { (void)cond; (void)mutex; return 0; }
int pthread_cond_broadcast(void *cond) { (void)cond; return 0; }
int pthread_mutex_init(void *mutex, const void *attr) { (void)mutex; (void)attr; return 0; }
int pthread_mutex_destroy(void *mutex) { (void)mutex; return 0; }
int pthread_mutexattr_init(void *attr) { (void)attr; return 0; }
int pthread_mutexattr_destroy(void *attr) { (void)attr; return 0; }
int pthread_mutexattr_settype(void *attr, int type) { (void)attr; (void)type; return 0; }
void *pthread_getspecific(unsigned long key) { (void)key; return NULL; }
int pthread_setspecific(unsigned long key, const void *value) { (void)key; (void)value; return ENOSYS; }

int clock_gettime(clockid_t clock_id, struct timespec *ts)
{
	(void)clock_id;
	if (!ts) { errno = EINVAL; return -1; }
	int64_t us = _pax_svc && _pax_svc->get_time_us ?
		_pax_svc->get_time_us() : 0;
	ts->tv_sec = us / 1000000;
	ts->tv_nsec = (long)(us % 1000000) * 1000L;
	return 0;
}

int _getentropy(void *buffer, size_t length)
{
	if (!buffer) { errno = EINVAL; return -1; }
	__builtin_memset(buffer, 0, length);
	return 0;
}
