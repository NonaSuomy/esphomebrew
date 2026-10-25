#include "psram_app.h"
#include "qcommon/qcommon.h"
#include "client/keys.h"
#include "quakegeneric.h"
#include "other/q_arena.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern const app_services_t *_pax_svc;
extern void app_return_to_launcher(void);
extern void pax_input_poll(void);
extern void R_FreeUnusedImages(void);
extern void Mod_FreeAll(void);
extern void S_FlushCaches(void);
extern int registration_sequence;

static struct { int key, down; } keyq[64];
static int head, tail;
unsigned sys_frame_time;
qboolean stdin_active = false;
cvar_t *nostdout;
extern cvar_t *vid_fullscreen;

void Sys_ConsoleOutput(char *s) { if (_pax_svc) _pax_svc->log_printf("%s", s); }
void Sys_Printf(char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (_pax_svc) _pax_svc->log_printf("%s", b);
}
void Sys_Quit(void) { app_return_to_launcher(); }
void Sys_Init(void) { head = tail = 0; }
void Sys_Error(char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (_pax_svc) _pax_svc->log_printf("QUAKE2 ERROR: %s\n", b);
    app_return_to_launcher();
}
void Sys_Warn(char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (_pax_svc) _pax_svc->log_printf("QUAKE2 WARN: %s", b);
}
char *Sys_ConsoleInput(void) { return NULL; }
void Sys_UnloadGame(void) {}
void *Sys_GetGameAPI(void *p) { extern void *GetGameAPI(void *); return GetGameAPI(p); }
void Sys_AppActivate(void) {}
void Sys_SendKeyEvents(void) {
    while (tail != head) { Key_Event(keyq[tail].key, keyq[tail].down, Sys_Milliseconds()); tail = (tail + 1) & 63; }
    sys_frame_time = Sys_Milliseconds();
}
char *Sys_GetClipboardData(void) { return NULL; }
void Sys_CopyProtect(void) {}
int curtime;
int Sys_Milliseconds(void) { curtime = (int)(_pax_svc->get_time_us() / 1000); return curtime; }
void ProcessKeyEvent(int key, qboolean down) { Key_Event(key, down, Sys_Milliseconds()); }

void Quake2_Init(int argc, char **argv) {
    head = tail = 0;
    /* The PAX itself occupies executable PSRAM, so the full desktop
     * 16 MiB Quake arena does not fit in the loader's remaining heap. */
    if (Q_Arena_Init(QUAKE_MEMSIZE) < QUAKE_MEMSIZE) {
        Sys_Error("not enough PSRAM for Quake arena (%d bytes)", QUAKE_MEMSIZE);
        return;
    }
    Hunk_Init();
    Qcommon_Init(argc, argv);
    nostdout = Cvar_Get("nostdout", "0", 0);
}
void Quake2_Frame(int msec) { Qcommon_Frame(msec); }
int Quake2_Milliseconds(void) { return Sys_Milliseconds(); }
void Quake2_SendKey(int key, int down) {
    int n = (head + 1) & 63;
    if (n == tail) return;
    keyq[head].key = key; keyq[head].down = down; head = n;
}
void ri_esp_log_model(const char *name, int filelen, int datasize) {
    if (strstr(name, "models/monsters/") && _pax_svc->log_printf)
        _pax_svc->log_printf("QUAKE2 MONSTER loaded %s file=%d cache=%d\n", name, filelen, datasize);
}

/* Release media from the previous registration sequence before the new BSP
 * and its models are loaded.  The renderer's hunk pool is separate, but its
 * cache-backed allocations and sound cache still compete with the Quake
 * arena's transient map allocations. */
void Q_MapLoading(void) {
    /* Evict this registration before the next BSP is read. */
    registration_sequence++;
    R_FreeUnusedImages();
    Mod_FreeAll();
    S_FlushCaches();
}
