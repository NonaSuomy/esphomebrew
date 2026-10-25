/* Stratagus PAX entry point.
 *
 * The engine remains an external build input.  This wrapper supplies the
 * launcher ABI, selects the SD-card data root, and keeps the normal Stratagus
 * command-line entry point unchanged so upstream Lua/game logic remains
 * usable.
 */
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "pax_platform.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "pax_number_format.h"
#include "pax_soft_round.h"
#ifdef PAX_STRATAGUS_WARCRAFT
#include <typeinfo>
#endif

extern "C" double floor(double value) noexcept { return pax_round_integral(value,0); }
extern "C" double ceil(double value) noexcept { return pax_round_integral(value,1); }
extern "C" double modf(double value, double *integer) noexcept { return pax_modf(value,integer); }

extern const app_services_t *_pax_svc;

static size_t pax_puts(char *dst, size_t cap, size_t pos, const char *s)
{
	while (*s) {
		if (pos + 1 < cap) dst[pos] = *s;
		++pos; ++s;
	}
	return pos;
}

static size_t pax_putu(char *dst, size_t cap, size_t pos, unsigned long value, unsigned base)
{
	static const char digits[] = "0123456789abcdef";
	char tmp[32]; size_t n = 0;
	do { tmp[n++] = digits[value % base]; value /= base; } while (value && n < sizeof(tmp));
	while (n) { if (pos + 1 < cap) dst[pos] = tmp[--n]; else --n; ++pos; }
	return pos;
}

static int pax_vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap)
{
	size_t pos = 0;
	for (const char *p = fmt; *p; ++p) {
		if (*p != '%') { if (pos + 1 < cap) dst[pos] = *p; ++pos; continue; }
		++p;
		if (*p == '%') { if (pos + 1 < cap) dst[pos] = '%'; ++pos; continue; }
		bool long_arg = false;
		int precision = -1;
		while (*p == 'l' || *p == 'z' || *p == '+' || *p == '-' || (*p >= '0' && *p <= '9')) {
			if (*p == 'l' || *p == 'z') long_arg = true;
			++p;
		}
		if (*p == '.') {
			++p; precision=0;
			if (*p == '*') { precision=va_arg(ap,int); ++p; }
			else while (*p >= '0' && *p <= '9') precision=precision*10+(*p++-'0');
		}
		switch (*p) {
			case 's': pos = pax_puts(dst, cap, pos, va_arg(ap, const char *) ?: "(null)"); break;
			case 'c': { char c = (char)va_arg(ap, int); if (pos + 1 < cap) dst[pos] = c; ++pos; break; }
			case 'd': case 'i': {
				long value = long_arg ? va_arg(ap, long) : va_arg(ap, int);
				if (value < 0) { if (pos + 1 < cap) dst[pos] = '-'; ++pos; value = -value; }
				pos = pax_putu(dst, cap, pos, (unsigned long)value, 10); break;
			}
			case 'u': pos = pax_putu(dst, cap, pos, long_arg ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned), 10); break;
			case 'x': case 'X': pos = pax_putu(dst, cap, pos, long_arg ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned), 16); break;
			case 'p': pos = pax_puts(dst, cap, pos, "0x"); pos = pax_putu(dst, cap, pos, (uintptr_t)va_arg(ap, void *), 16); break;
			case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
				char number[64];
				pax_format_number(number,sizeof(number),va_arg(ap,double),*p,precision);
				pos=pax_puts(dst,cap,pos,number); break;
			}
			default: if (pos + 1 < cap) dst[pos] = '?'; ++pos; break;
		}
	}
	if (cap) dst[pos < cap ? pos : cap - 1] = '\0';
	return (int)pos;
}

extern "C" int vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap)
{
	return pax_vsnprintf(dst, cap, fmt, ap);
}
extern "C" int snprintf(char *dst, size_t cap, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); int n = pax_vsnprintf(dst, cap, fmt, ap); va_end(ap); return n;
}
extern "C" int sprintf(char *dst, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); int n = pax_vsnprintf(dst, (size_t)-1, fmt, ap); va_end(ap); return n;
}

// Route diagnostics through the launcher instead of buffered newlib stderr.
// A fatal startup exits before that buffer is flushed, hiding the real error.
#ifdef PAX_STRATAGUS_WARCRAFT
extern "C" [[noreturn]] void __real___cxa_throw(void *, std::type_info *, void (*)(void *));
extern "C" [[noreturn]] void __wrap___cxa_throw(void *object, std::type_info *type, void (*destroy)(void *)) {
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("Warcraft: throw type=%s caller=%p\n",type?type->name():"?",__builtin_return_address(0));
	__real___cxa_throw(object,type,destroy);
}
extern "C" int fprintf(FILE *file, const char *fmt, ...)
{
	char message[1024];
	va_list ap; va_start(ap,fmt);
	int count=pax_vsnprintf(message,sizeof(message),fmt,ap); va_end(ap);
	if (file==stderr || file==stdout) {
		if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("Stratagus console: %s",message);
		return count;
	}
	// Preserve file output; use two passes when a save line exceeds the buffer.
	if (count < int(sizeof(message))) return fwrite(message,1,count,file)==size_t(count)?count:-1;
	char *full=(char *)malloc(size_t(count)+1); if (!full) return -1;
	va_start(ap,fmt); pax_vsnprintf(full,size_t(count)+1,fmt,ap); va_end(ap);
	const bool ok=fwrite(full,1,count,file)==size_t(count); free(full); return ok?count:-1;
}
// GCC rewrites integer-only fprintf calls to newlib's fiprintf.
extern "C" int fiprintf(FILE *, const char *, ...) __attribute__((alias("fprintf")));
extern "C" [[noreturn]] void _exit(int);
extern "C" [[noreturn]] void exit(int status) noexcept {
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("Warcraft: exit status=%d caller=%p\n",status,__builtin_return_address(0));
	// The loader owns teardown; do not enter newlib's desktop exit handlers
	// from an active Lua callback before child workers can be stopped safely.
	_exit(status);
}
extern "C" [[noreturn]] void abort() noexcept {
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("Warcraft: abort caller=%p\n",__builtin_return_address(0));
	_exit(134);
}
extern "C" [[noreturn]] void __assert_func(const char *file,int line,const char *function,const char *condition) {
	if (_pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("Warcraft: assertion %s:%d %s: %s\n",file,line,function?function:"?",condition);
	_exit(134);
}
#endif

static double pax_parse_decimal(const char *s, char **end)
{
	const char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
	int sign = 1;
	if (*p == '-' || *p == '+') { if (*p++ == '-') sign = -1; }
	double value = 0.0;
	while (*p >= '0' && *p <= '9') value = value * 10.0 + (*p++ - '0');
	if (*p == '.') {
		++p;
		double place = 0.1;
		while (*p >= '0' && *p <= '9') { value += (*p++ - '0') * place; place *= 0.1; }
	}
	if (*p == 'e' || *p == 'E') {
		++p; int esign = 1; if (*p == '-' || *p == '+') { if (*p++ == '-') esign = -1; }
		int exp = 0; while (*p >= '0' && *p <= '9') exp = exp * 10 + (*p++ - '0');
		if (esign < 0) while (exp--) value *= 0.1; else while (exp--) value *= 10.0;
	}
	if (end) *end = const_cast<char *>(p);
	return sign < 0 ? -value : value;
}

extern "C" double strtod(const char *s, char **end) { return pax_parse_decimal(s, end); }
extern "C" double _strtod_l(const char *s, char **end, void *) { return pax_parse_decimal(s, end); }

static unsigned long pax_parse_uint(const char *s, char **end, int base)
{
	const char *p = s; while (*p == ' ' || *p == '\t') ++p;
	if (base == 0) { base = 10; if (*p == '0') { base = 8; ++p; if (*p == 'x' || *p == 'X') { base = 16; ++p; } } }
	unsigned long value = 0;
	for (;;) { int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1; if (d < 0 || d >= base) break; value = value * (unsigned)base + (unsigned)d; ++p; }
	if (end) *end = const_cast<char *>(p);
	return value;
}
extern "C" unsigned long strtoul(const char *s, char **e, int b) { return pax_parse_uint(s, e, b); }
extern "C" unsigned long _strtoul_l(const char *s, char **e, int b, void *) { return pax_parse_uint(s, e, b); }
extern "C" long strtol(const char *s, char **e, int b) { return (long)pax_parse_uint(s, e, b); }
extern "C" long _strtol_l(const char *s, char **e, int b, void *) { return (long)pax_parse_uint(s, e, b); }

static void *pax_cpp_alloc(size_t size)
{
	if (!_pax_svc) return nullptr;
	if (_pax_svc->mem_caps_alloc) {
		if (void *p = _pax_svc->mem_caps_alloc(size, PAX_MEM_CAP_SPIRAM)) return p;
	}
	void *result=_pax_svc->mem_alloc ? _pax_svc->mem_alloc(size) : nullptr;
	static unsigned failures;
	if (!result && _pax_svc->log_printf && ++failures<=4)
		_pax_svc->log_printf("Stargus: C++ allocation exhausted bytes=%u caller=%p\n",
			(unsigned)size,__builtin_return_address(0));
	return result;
}

void *operator new(size_t size) { return pax_cpp_alloc(size); }
void *operator new[](size_t size) { return pax_cpp_alloc(size); }
void operator delete(void *ptr) noexcept
{
	if (ptr && _pax_svc && _pax_svc->mem_free) _pax_svc->mem_free(ptr);
}
void operator delete[](void *ptr) noexcept
{
	if (ptr && _pax_svc && _pax_svc->mem_free) _pax_svc->mem_free(ptr);
}
void operator delete(void *ptr, size_t) noexcept { operator delete(ptr); }
void operator delete[](void *ptr, size_t) noexcept { operator delete[](ptr); }

int stratagusMain(int argc, char **argv);

#ifndef PAX_STRATAGUS_DATA_ROOT
#define PAX_STRATAGUS_DATA_ROOT "/sd/roms/stargus"
#endif

#ifndef PAX_STRATAGUS_BANNER
#define PAX_STRATAGUS_BANNER "Stratagus"
#endif

const app_services_t *_pax_svc = nullptr;
volatile int pax_exit_requested = 0;

/* PAX images are linked without the normal C++ startup files.  Preserve and
 * run the compiler-generated constructor table explicitly so global
 * std::string/vector objects have the same invariants as a normal executable.
 */
using pax_init_fn_t = void (*)();
extern "C" pax_init_fn_t __preinit_array_start[];
extern "C" pax_init_fn_t __preinit_array_end[];
extern "C" pax_init_fn_t __init_array_start[];
extern "C" pax_init_fn_t __init_array_end[];
extern "C" pax_init_fn_t __fini_array_start[];
extern "C" pax_init_fn_t __fini_array_end[];
// Explicit atexit callbacks and compiler-generated .fini_array destructors
// are separate. The launcher supplies neither exit() nor CRT shutdown.
extern "C" void __call_exitprocs(int status, void *dso);
#ifdef PAX_STRATAGUS_WARCRAFT
extern "C" const char __pax_eh_frame_start[];
extern "C" void __register_frame(const void *);
extern "C" void *__deregister_frame(const void *);
#endif

static void pax_run_global_destructors(int status)
{
	static bool destroyed;
	if (destroyed) return;
	destroyed=true;
	__call_exitprocs(status,nullptr);
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: running %u global finalizers\n",
		(unsigned)(__fini_array_end-__fini_array_start));
	for (pax_init_fn_t *fn=__fini_array_end; fn!=__fini_array_start;) (*--fn)();
#ifdef PAX_STRATAGUS_WARCRAFT
	__deregister_frame(__pax_eh_frame_start);
#endif
}

static void pax_run_global_constructors()
{
#ifdef PAX_STRATAGUS_WARCRAFT
	__register_frame(__pax_eh_frame_start);
#endif
	for (pax_init_fn_t *fn = __preinit_array_start;
		 fn != __preinit_array_end; ++fn) {
		(*fn)();
	}
	for (pax_init_fn_t *fn = __init_array_start;
		 fn != __init_array_end; ++fn) {
		(*fn)();
	}
}

extern "C" void pax_request_exit(void)
{
	pax_exit_requested = 1;
}

static int s_engine_done, s_engine_result;
extern "C" void *__real_memset(void *,int,size_t);
extern "C" void *__wrap_memset(void *target,int value,size_t bytes)
{
	// Preserve the fault, but identify the caller before the launcher's
	// reboot-only LAN report reduces the traceback to "memset at null".
	if (!target && bytes && _pax_svc && _pax_svc->log_printf)
		_pax_svc->log_printf("Stargus: invalid memset bytes=%u caller=%p\n",(unsigned)bytes,__builtin_return_address(0));
	return __real_memset(target,value,bytes);
}
static void engine_worker(void *)
{
#ifdef PAX_STRATAGUS_WARCRAFT
	// Exercise the exact throw/begin/end-catch runtime that crashed in game,
	// including a nested catch and rethrow, before starting child workers.
	bool exceptionStorageOk=false;
	try {
		try {
			throw 17;
		} catch (int value) {
			try { throw 23; } catch (int nested) {
				if (value!=17 || nested!=23) __builtin_trap();
			}
			throw;
		}
	} catch (int value) { exceptionStorageOk=value==17; }
	if (!exceptionStorageOk) __builtin_trap();
	_pax_svc->log_printf("Warcraft: exception storage regression PASS (nested catch/rethrow)\n");
	if (_pax_svc && _pax_svc->file_mkdir) {
		_pax_svc->file_mkdir(PAX_STRATAGUS_DATA_ROOT "/user");
		_pax_svc->file_mkdir(PAX_STRATAGUS_DATA_ROOT "/user/" PAX_STRATAGUS_USER_GAME);
		_pax_svc->file_mkdir(PAX_STRATAGUS_DATA_ROOT "/user/" PAX_STRATAGUS_USER_GAME "/save");
	}
#endif
	// Prepare short unit voices before the music/speaker starts. Opening an
	// Ogg/codebook on each tap otherwise stalls the music for hundreds of ms.
	pax_stratagus_audio_precache();
	/* Stratagus resolves scripts and assets relative to -d.  Keep saves on
	 * the SD card as well; the loader owns the underlying file handles through
	 * the libc wrappers used by the external engine build. */
	static char arg0[] = "stratagus";
	static char arg_d[] = "-d";
	static char data_root[] = PAX_STRATAGUS_DATA_ROOT;
	static char arg_u[] = "-u";
	static char user_root[] = PAX_STRATAGUS_DATA_ROOT "/user";
	static char arg_c[] = "-c";
#ifdef PAX_STRATAGUS_TEST_STARTUP
	static char start_script[] = PAX_STRATAGUS_TEST_STARTUP;
#else
	static char start_script[] = "scripts/stratagus.lua";
#endif
#ifdef PAX_STRATAGUS_TEST_MAP
	static char test_map[] = PAX_STRATAGUS_TEST_MAP;
	static char *argv[] = {arg0,arg_d,data_root,arg_u,user_root,arg_c,start_script,test_map,nullptr};
	_pax_svc->log_printf("Warcraft: diagnostic CLI map=%s (not a menu-navigation test)\n",test_map);
	s_engine_result = stratagusMain(8, argv);
#else
	static char *argv[] = {
		arg0, arg_d, data_root, arg_u, user_root, arg_c, start_script, nullptr
	};

	s_engine_result = stratagusMain(7, argv);
#endif
	// Stop child workers before destroying the globals they may reference.
	// Run shutdown on this 64 KiB stack, not the loader's 8 KiB entry stack.
	pax_stratagus_platform_shutdown();
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: releasing C++ global resources\n");
	pax_run_global_destructors(s_engine_result);
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: C++ global resources released\n");
	__atomic_store_n(&s_engine_done, 1, __ATOMIC_RELEASE);
	// The entry task owns deletion. It must not unmap this image while the
	// engine worker is still returning through code in it.
	for (;;) _pax_svc->delay_ms(100);
}

extern "C" __attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
	if (!svc || svc->abi_version != PAX_ABI_VERSION || !svc->task_create || !svc->task_delete) return -1;
	_pax_svc = svc;
    pax_exit_requested = 0;
#ifndef PAX_STRATAGUS_WARCRAFT
    const char *files[] = {PAX_STRATAGUS_DATA_ROOT "/scripts/stratagus.lua",
        PAX_STRATAGUS_DATA_ROOT "/media/runtime.paxassets", PAX_STRATAGUS_DATA_ROOT "/media/menu.paxmenu",
        PAX_STRATAGUS_DATA_ROOT "/media/terrain.paxtile", PAX_STRATAGUS_DATA_ROOT "/media/stargus_intro.ogg"};
    const char *magic[] = {nullptr, "PAPASST1", "PAPMENU1", "PAPTILE1", "OggS"};
    for (unsigned i=0; i<sizeof(files)/sizeof(files[0]); ++i) {
        void *file=svc->file_open ? svc->file_open(files[i],"rb") : nullptr;
        unsigned char header[8];
        const bool ok=file && svc->file_read(header,1,sizeof(header),file)==sizeof(header) &&
            (!magic[i] || !memcmp(header,magic[i],i==4 ? 4 : 8));
        if(file) svc->file_close(file);
        if(!ok) {
            svc->log_printf("Stargus: missing or invalid SD asset %s; run prepare_stargus_sd.py on the converted game tree\n", files[i]);
            return -1;
        }
    }
#endif
	pax_run_global_constructors();
	pax_stratagus_audio_prepare();
	svc->log_printf("=== %s PAX starting ===\n", PAX_STRATAGUS_BANNER);
	svc->log_printf("Stargus: entering stratagusMain, data root=%s\n", PAX_STRATAGUS_DATA_ROOT);
	// The loader deliberately has an 8 KiB internal bootstrap stack for MMU
	// map/unmap. Lua/map progress and native action calls exceed that budget.
	// Large app-created stacks are PSRAM-backed by the existing service API.
	void *worker=nullptr;
	__atomic_store_n(&s_engine_done, 0, __ATOMIC_RELEASE);
	if (svc->task_create(engine_worker,"stargus",64*1024,nullptr,5,&worker,0)) {
		pax_stratagus_platform_shutdown();
		pax_run_global_destructors(-1);
		svc->log_printf("Stargus: engine worker unavailable\n");
		return -1;
	}
	while (!__atomic_load_n(&s_engine_done, __ATOMIC_ACQUIRE)) svc->delay_ms(10);
	svc->task_delete(worker);
	const int rc=s_engine_result;
	pax_stratagus_platform_shutdown();
	svc->log_printf("=== %s PAX exited (%d) ===\n", PAX_STRATAGUS_BANNER, rc);
	return rc;
}
