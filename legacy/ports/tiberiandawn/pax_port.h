// Tiberian Dawn (Vanilla Conquer) on the ESP32-P4 PAX loader: shared declarations
// for the port's glue code. Nothing here is part of Vanilla Conquer itself.
#pragma once

#ifndef PAX_APP_SIDE
#define PAX_APP_SIDE 1
#endif
// ESP-IDF builds get this C++ spelling from newlib's <sys/cdefs.h>; the loader
// header uses the C11 keyword.
#if defined(__cplusplus) && !defined(_Static_assert)
#define _Static_assert static_assert
#endif

// The loader's own ABI header: the SDK one plus the USB mouse/keyboard services.
#include "esphome/components/pax_loader/psram_app.h"

#ifdef __cplusplus
extern "C" {
#endif

// The loader's service table, set by app_entry before anything else runs.
extern const app_services_t *pax_svc;

// Where the game data lives on the card (CONQUER.MIX, GENERAL.MIX, ...).
#define PAX_TD_DATA_DIR "/sd/roms/tiberiandawn"

// Leave the game and return to the loader (exit(), abort(), a close request).
void pax_quit(int code) __attribute__((noreturn));

// True once the loader asked the app to close (on-screen X: MENU + X held).
int pax_close_requested(void);

// Release every heap block / file the game still holds (pax_syscalls.c).
void pax_free_all_memory(void);
void pax_close_all_files(void);

// Check every heap block's guards and log damaged ones (game task only).
void pax_heap_check(void);

// Stop the sound mixer task (pax_sound.cpp); safe when it never started.
void pax_sound_shutdown(void);

// Stop the presenter task (pax_video.cpp); safe when it never started.
void pax_video_shutdown(void);

// Sockets (pax_net.cpp) use descriptors PAX_SOCKET_FD_BASE.. so close()
// can tell them from files; they must stay below newlib's FD_SETSIZE (64).
#define PAX_SOCKET_FD_BASE 40
#define PAX_SOCKET_FD_COUNT 16 // a TCP game: listener, up to 7 peers, discovery socket
int pax_socket_close(int fd);

// Microseconds since boot.
long long pax_time_us(void);

// Poll the loader's inputs into the game's keyboard/mouse queue; called from
// the game's keyboard check so the game loop also yields to other tasks.
void pax_input_poll(void);

#ifdef __cplusplus
}
#endif
