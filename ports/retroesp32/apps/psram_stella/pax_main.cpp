/* Atari 2600 / Stella PAX entry point. */
#define PAX_APP_SIDE 1

#include "runtime.h"
#include "stella_run.h"

#include <stddef.h>
#include <setjmp.h>

const app_services_t *_pax_svc;
static jmp_buf s_exit_env;

extern "C" void app_return_to_launcher(void)
{
    longjmp(s_exit_env, 1);
}

void *operator new(size_t size) noexcept
{
    return _pax_svc ? _pax_svc->mem_alloc(size) : NULL;
}
void *operator new[](size_t size) noexcept
{
    return _pax_svc ? _pax_svc->mem_alloc(size) : NULL;
}
void operator delete(void *ptr) noexcept
{
    if (ptr && _pax_svc) _pax_svc->mem_free(ptr);
}
void operator delete[](void *ptr) noexcept
{
    if (ptr && _pax_svc) _pax_svc->mem_free(ptr);
}
void operator delete(void *ptr, size_t) noexcept
{
    if (ptr && _pax_svc) _pax_svc->mem_free(ptr);
}
void operator delete[](void *ptr, size_t) noexcept
{
    if (ptr && _pax_svc) _pax_svc->mem_free(ptr);
}

/* The PAX header uses entry_off=0. Keep the C++ entry point at the first
 * byte of the flat image, just like the C PAX entry points. */
extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAX_ABI_VERSION) return -1;
    _pax_svc = svc;

    char rom_path[512];
    if (emu_rom_path("stella", ".a26|.bin|", rom_path, sizeof(rom_path))) {
        svc->log_printf("Stella: set /sd/roms/pax/stella.rom to an .a26 or .bin ROM\n");
        return -1;
    }

    if (setjmp(s_exit_env)) {
        pax_close_streams();
        pax_cleanup_fds();
        pax_cleanup_heap();
        return -1;
    }

    stella_run(rom_path);
    return 0;
}
