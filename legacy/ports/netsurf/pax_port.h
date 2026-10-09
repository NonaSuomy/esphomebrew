// NetSurf on the ESP32-P4 PAX loader: declarations shared by the port's glue
// code (ports/netsurf/*.c). Nothing here is part of NetSurf.
//
// Tasks (created through the loader's task_create service):
//   netsurf  NetSurf's framebuffer frontend: its scheduler, fetchers, layout
//            and drawing. Core 0, 512 KiB stack in PSRAM.
//   ns_dns   Host name lookups for the HTTP fetcher (net_resolve blocks).
#pragma once

#ifndef PAX_APP_SIDE
#define PAX_APP_SIDE 1
#endif

#include "esphome/components/pax_loader/psram_app.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// The loader's service table, set by app_entry before anything else runs.
extern const app_services_t *pax_svc;

// The canvas NetSurf draws on: the loader's offer (normally the whole panel,
// 1024x600) or 800x480 on older loaders. pax_main.c sets it before NetSurf
// starts.
extern int pax_canvas_w;
extern int pax_canvas_h;

// NetSurf's folder on the card: Choices, Cookies, Messages, and any resource
// (default.css, welcome.html, ...) that should replace the built-in copy.
#define PAX_NS_DIR "/sd/roms/netsurf"

// Leave NetSurf and return to the loader. pax_request_quit() asks the
// frontend to close (the surface turns it into NSFB_CONTROL_QUIT);
// pax_quit() is for exit()/abort() and never returns.
void pax_request_quit(int code);
int pax_quit_requested(void);
void pax_quit(int code) __attribute__((noreturn));

// Microseconds since boot.
int64_t pax_time_us(void);

// Sleep one scheduler tick if the calling task has not slept for a while, so
// the idle task (and its watchdog) on this core still runs. pax_sleep_ms()
// sleeps and records that it did.
void pax_yield_if_due(void);
void pax_sleep_ms(int ms);

// Wall clock: time() is the loader's uptime until the first HTTP response
// brings a Date header; then it is that date plus the time since.
void pax_set_wallclock(time_t now);

// PSRAM (or internal RAM) straight from the loader, not in the app heap's
// list; freed with pax_free_raw.
void *pax_alloc_raw(size_t size, int internal);
void pax_free_raw(void *ptr);

// Heap, files and locks the app still holds, released when it quits.
void pax_syscalls_init(void);
void pax_syscalls_deinit(void);
void pax_free_all_memory(void);
void pax_close_all_files(void);

// Loader files (FILE * behind the service table), remembered so they can be
// closed when NetSurf quits.
void *pax_file_open(const char *path, const char *mode);
int pax_file_close(void *fp);
long pax_file_size(void *fp);
int pax_file_exists(const char *path);

// HTTP fetcher (pax_http.c): stops the host-name lookup task.
void pax_http_shutdown(void);

#ifdef __cplusplus
}
#endif
