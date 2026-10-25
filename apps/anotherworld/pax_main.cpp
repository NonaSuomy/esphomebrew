/* Another World / Out Of This World PAX wrapper.
 *
 * The game banks and resource table are read from SD. This
 * wrapper supplies the launcher's LCD, controller, USB keyboard, audio and
 * close services while keeping the game core independent of the VGA board.
 */
#define PAX_APP_SIDE 1

#include "runtime.h"
#include "engine.h"
#include "gbGlobals.h"
#include "pax_system.h"
#include "pax_data.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

static void free_emulator_memory(const app_services_t *svc)
{
    for (unsigned i = 0; i < 4; ++i) {
        if (gb_vram[i] && svc->mem_free) svc->mem_free(gb_vram[i]);
        gb_vram[i] = NULL;
    }
    if (gb_memory && svc->mem_free) svc->mem_free(gb_memory);
    gb_memory = NULL;
}

extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAX_ABI_VERSION) return -1;
    _pax_svc = svc;
    memset(gb_vram, 0, sizeof(gb_vram));
    gb_memory = NULL;

    svc->log_printf("Another World PAX: SD game data, LCD mode\n");

    if (!pax_aw_load_memlist()) {
        svc->log_printf("Another World: supply valid DOS memlist.bin and bank files in /sd/roms/anotherworld/\n");
        return -1;
    }

    gb_memory = (unsigned char *)svc->mem_caps_alloc(
        600u * 1024u, PAX_MEM_CAP_SPIRAM);
    if (!gb_memory) {
        svc->log_printf("Another World PAX: unable to allocate VM memory\n");
        return -1;
    }

    for (unsigned i = 0; i < 4; ++i) {
        gb_vram[i] = (unsigned char *)svc->mem_caps_alloc(
            Video::VID_PAGE_SIZE, PAX_MEM_CAP_SPIRAM);
        if (!gb_vram[i]) {
            svc->log_printf("Another World PAX: unable to allocate video page %u\n", i);
            free_emulator_memory(svc);
            return -1;
        }
        memset(gb_vram[i], 0, Video::VID_PAGE_SIZE);
    }

    gb_use_game_part = 2; /* start at the intro; protection is bypassed */
    gb_use_send_game_part = 0;

    PaxAnotherWorldSystem *system = new PaxAnotherWorldSystem(svc);
    Engine *engine = new Engine(system, "/sd/roms/anotherworld", "/sd/roms/anotherworld");
    if (!system || !engine) {
        svc->log_printf("Another World PAX: object allocation failed\n");
        if (engine) delete engine;
        if (system) delete system;
        free_emulator_memory(svc);
        return -1;
    }

    if (setjmp(s_exit_env) == 0) {
        engine->init();
        engine->run();
    } else {
        svc->log_printf("Another World PAX: close requested\n");
    }

    /* Engine's destructor finishes the audio/mixer core and destroys the
     * System. The System destructor is idempotent for the longjmp path. */
    delete engine;
    delete system;
    free_emulator_memory(svc);
    return 0;
}
