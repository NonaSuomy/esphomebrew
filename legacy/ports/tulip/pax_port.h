// Tulip Creative Computer on the ESP32-P4 PAX loader: declarations shared by
// the port's glue code (ports/tulip/*.c). Nothing here is part of Tulip.
//
// Tasks (all created through the loader's task_create service):
//   mp       MicroPython: _boot.py, boot.py, the REPL. Core 0, PSRAM stack.
//   display  Tulip's compositor (display_bounce_empty) into the loader's
//            1024x600 RGB565 canvas (scaled to 800x480 on older loaders),
//            ~30 fps, plus input polling. Core 1.
//   audio    AMY rendering (amy_simple_fill_buffer) into the loader's
//            speaker at 44.1 kHz. Core 1.
#pragma once

#ifndef PAX_APP_SIDE
#define PAX_APP_SIDE 1
#endif

#include "esphome/components/pax_loader/psram_app.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The loader's service table, set by app_entry before anything else runs.
extern const app_services_t *pax_svc;

// Tulip's files live here on the card: tulip.lfs (the Tulip filesystem image
// mounted at /), and anything the user copies for import from /sd.
#define PAX_TULIP_DIR "/sd/roms/tulip"

// Leave Tulip and return to the loader (exit(), abort(), fatal errors, the
// close control). Safe from any of the app's tasks.
void pax_quit(int code) __attribute__((noreturn));
void pax_request_quit(int code);  // same, but returns (display/input code)
volatile int *pax_quit_flag(void);

// Microseconds since boot.
int64_t pax_time_us(void);

// PSRAM (or internal RAM) straight from the loader, not tracked by the
// app heap's leak list; freed with pax_free_raw.
void *pax_alloc_raw(size_t size, int internal);
void pax_free_raw(void *ptr);

// Heap and files the app still holds, released when it quits.
void pax_syscalls_init(void);
void pax_syscalls_deinit(void);
void pax_free_all_memory(void);
void pax_close_all_files(void);

// Loader files (FILE * behind the service table), remembered so they can be
// closed when Tulip quits.
void *pax_file_open(const char *path, const char *mode);
int pax_file_close(void *fp);
long pax_file_size(void *fp);

// A spinlock usable across the app's tasks and cores (the loader has no mutex
// service). Lock words live in internal RAM.
typedef struct pax_lock pax_lock_t;
pax_lock_t *pax_lock_new(void);
void pax_lock_take(pax_lock_t *lock);
void pax_lock_give(pax_lock_t *lock);

// MicroPython's atomic sections (scheduler queue, keyboard interrupt) and
// AMY's event queue lock.
unsigned pax_atomic_begin(void);
void pax_atomic_end(unsigned state);
void pax_amy_lock(int grab);

// ── Display and input (pax_display.c) ──────────────────────────────────
int pax_display_start(void);
void pax_display_stop(void);

// ── Audio (pax_audio.c) ─────────────────────────────────────────────────
void run_amy(void);
void pax_audio_stop(void);

// ── MicroPython (pax_main.c) ────────────────────────────────────────────
// Set once the MicroPython task has its heap and scheduler (input waits for it).
extern volatile int pax_mp_ready;

#ifdef __cplusplus
}
#endif
