/* Exercise the real MP3 decoder with a short 44.1 kHz stereo fixture.
 * The worker runs on pthreads; output is consumed at the device's 22050 Hz. */
#include "../pax_music.c"
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

const app_services_t *_pax_svc;
static const char *fixture;
static pthread_t thread;
static void (*thread_fn)(void *);
static unsigned allocations, opens, closes, deletes;
static cvar_t nocd_var, volume_var;

static int64_t host_time(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void host_delay(int ms) { usleep(ms * 1000); }
static void *host_calloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (p) ++allocations;
    return p;
}
static void host_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
static void *host_open(const char *path, const char *mode) {
    if (!strstr(path, "track02.mp3") && !strstr(path, "track03.mp3")) return NULL;
    FILE *f = fopen(fixture, mode);
    if (f) ++opens;
    return f;
}
static int host_close(void *f) { ++closes; return fclose(f); }
static size_t host_read(void *p, size_t n, size_t count, void *f) { return fread(p, n, count, f); }
static int host_seek(void *f, long offset, int mode) { return fseek(f, offset, mode); }
static int host_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); va_end(ap); return n;
}
static void *host_thread(void *arg) { thread_fn(arg); return NULL; }
static int host_create(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, int priority, void *out, int core) {
    assert(!strcmp(name, "q2_music") && stack > 32768 && priority < 6 && core == 1);
    thread_fn = fn;
    if (pthread_create(&thread, NULL, host_thread, arg)) return -1;
    *(void **)out = &thread;
    return 0;
}
static void host_delete(void *handle) {
    assert(handle == &thread && __atomic_load_n(&worker_done, __ATOMIC_ACQUIRE));
    pthread_cancel(thread);
    pthread_join(thread, NULL);
    ++deletes;
}
cvar_t *Cvar_Get(char *name, char *value, int flags) {
    (void)flags;
    cvar_t *v = !strcmp(name, "cd_nocd") ? &nocd_var : &volume_var;
    v->value = atof(value);
    v->modified = true;
    return v;
}
char *FS_Gamedir(void) { return "/sd/roms/quake2/baseq2"; }
void Cmd_AddCommand(char *name, xcommand_t fn) { assert(!strcmp(name, "cd") && fn); }
void Cmd_RemoveCommand(char *name) { assert(!strcmp(name, "cd")); }
char *Cmd_Argv(int arg) { (void)arg; return ""; }
void Com_Printf(char *fmt, ...) { (void)fmt; }

static void wait_ready(void) {
    int64_t until = host_time() + 2000000;
    while (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) && host_time() < until) host_delay(1);
    assert(__atomic_load_n(&ready, __ATOMIC_ACQUIRE));
}
static unsigned consume(unsigned blocks, int paced) {
    int16_t output[512 * 2];
    unsigned audible = 0;
    for (unsigned b = 0; b < blocks; ++b) {
        memset(output, 0, sizeof output);
        pax_music_mix(output, 512);
        for (unsigned i = 0; i < 1024; ++i) audible += output[i] != 0;
        if (paced) host_delay(24);
    }
    return audible;
}
static void wait_stopped(void) {
    int64_t until = host_time() + 2000000;
    while ((__atomic_load_n(&ready, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&ring_write, __ATOMIC_ACQUIRE)) && host_time() < until) host_delay(1);
    assert(!ready && !ring_write);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    fixture = argv[1];
    static const app_services_t svc = {
        .mem_calloc = host_calloc, .mem_free = host_free, .file_open = host_open,
        .file_read = host_read, .file_seek = host_seek, .file_close = host_close,
        .task_create = host_create, .task_delete = host_delete,
        .get_time_us = host_time, .delay_ms = host_delay, .log_printf = host_log
    };
    _pax_svc = &svc;
    assert(CDAudio_Init() == 0);
    CDAudio_Play(2, true);
    wait_ready();
    assert(consume(100, 1) > 10000); /* ring wrap and repeated short-file loops */
    assert(__atomic_load_n(&underrun_total, __ATOMIC_RELAXED) == 0);
    CDAudio_Pause();
    unsigned before = __atomic_load_n(&ring_read, __ATOMIC_RELAXED);
    assert(consume(3, 1) == 0 && ring_read == before);
    CDAudio_Resume();
    assert(consume(3, 1) > 0);
    nocd_var.value = 1; nocd_var.modified = true; CDAudio_Update();
    assert(consume(2, 1) == 0);
    nocd_var.value = 0; nocd_var.modified = true; CDAudio_Update();
    volume_var.value = 0; CDAudio_Update();
    assert(consume(2, 1) == 0);
    volume_var.value = 1; CDAudio_Update();
    assert(consume(2, 1) > 0);
    /* Saturating mix keeps loud music + SFX from wrapping to opposite sign. */
    int16_t loud[512 * 2];
    for (unsigned i = 0; i < 1024; ++i) loud[i] = 32760;
    pax_music_mix(loud, 512);
    int saturated = 0;
    for (unsigned i = 0; i < 1024; ++i) { assert(loud[i] >= 0); saturated |= loud[i] == 32767; }
    assert(saturated);
    CDAudio_Stop(); wait_stopped();
    assert(consume(2, 0) == 0);
    CDAudio_Play(3, false); wait_ready();
    assert(consume(80, 1) > 0);
    assert(__atomic_load_n(&finished, __ATOMIC_ACQUIRE) && !ready);
    CDAudio_Stop(); wait_stopped();
    CDAudio_Play(99, true); host_delay(50);
    assert(consume(2, 0) == 0);
    CDAudio_Stop(); wait_stopped();
    CDAudio_Play(2, true); wait_ready();
    assert(consume(2, 1) > 0);
    CDAudio_Shutdown(); pax_music_shutdown();
    assert(allocations == 0 && opens == closes && deletes == 1);
    assert(!task_handle && !ready);
    puts("PASS: real MP3 decode, looping, stereo resampling, pause/volume/clipping, EOF and clean shutdown");
    return 0;
}
