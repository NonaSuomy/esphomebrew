/* Emscripten libc replaces the ESP-IDF/newlib syscall layer. */
#include "pax_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static int64_t last_sleep;
int64_t pax_time_us(void) { return pax_svc->get_time_us(); }
void pax_set_wallclock(time_t now) {
  (void)now; /* Browser wall clock is already available. */
}
void pax_sleep_ms(int ms) {
  pax_svc->delay_ms(ms < 1 ? 1 : ms);
  last_sleep = pax_time_us();
}
void pax_yield_if_due(void) {
  if (pax_time_us() - last_sleep > 20000)
    pax_sleep_ms(1);
}
void *pax_alloc_raw(size_t n, int internal) {
  (void)internal;
  return malloc(n);
}
void pax_free_raw(void *p) { free(p); }
void pax_syscalls_init(void) {}
void pax_syscalls_deinit(void) {}
void pax_free_all_memory(void) {
} /* Each browser module owns its entire heap. */
void pax_close_all_files(void) {}
void *pax_file_open(const char *p, const char *m) {
  return pax_svc->file_open(p, m);
}
int pax_file_close(void *f) { return pax_svc->file_close(f); }
long pax_file_size(void *f) {
  long old = ftell(f);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, old, SEEK_SET);
  return size;
}
int pax_file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}
void __wrap_exit(int code) { pax_quit(code); }
