/* Loader services used by the embedded video decoder, with NetSurf's heap. */
#include "../pax_port.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

typedef struct pax_lock { volatile uint32_t word; } pax_lock_t;
pax_lock_t *pax_lock_new(void)
{
    pax_lock_t *p = pax_svc->mem_caps_alloc ? pax_svc->mem_caps_alloc(sizeof(*p), PAX_MEM_CAP_INTERNAL) : NULL;
    if (p) p->word = 0;
    return p;
}
void pax_lock_free(pax_lock_t *p) { if (p) pax_svc->mem_free(p); }
void pax_lock_take(pax_lock_t *p)
{
    unsigned spins = 0;
    while (__atomic_exchange_n(&p->word, 1u, __ATOMIC_ACQUIRE)) {
        if (++spins >= 2000) { spins = 0; pax_svc->delay_ms(1); }
    }
}
void pax_lock_give(pax_lock_t *p) { __atomic_store_n(&p->word, 0u, __ATOMIC_RELEASE); }
void pax_log(const char *fmt, ...)
{
    char text[256]; va_list ap; va_start(ap,fmt); vsnprintf(text,sizeof(text),fmt,ap); va_end(ap);
    pax_svc->log_printf("NETSURF MEDIA: %s\n",text);
}

#include <errno.h>
#include <sys/times.h>
#include <time.h>
int _fcntl(int fd, int command, int arg)
{ (void)fd; (void)command; (void)arg; errno = EBADF; return -1; }
clock_t _times(struct tms *t)
{
    clock_t ticks = (clock_t)(pax_time_us() / (1000000 / CLOCKS_PER_SEC));
    if (t) { t->tms_utime=ticks; t->tms_stime=0; t->tms_cutime=0; t->tms_cstime=0; }
    return ticks;
}
