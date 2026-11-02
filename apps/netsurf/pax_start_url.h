#pragma once
#include <stddef.h>
#include <string.h>

/* The PAX launcher passes the selected executable as its argument. A browser
 * hand-off instead passes a page, which must still take precedence. */
static int pax_netsurf_is_launcher_path(const char *arg)
{
    const char *end = arg + strcspn(arg, "?#");
    const char *base = end;
    while (base > arg && base[-1] != '/') --base;
    size_t n = (size_t)(end - base);
    return (n == sizeof("netsurf.pax") - 1 && memcmp(base, "netsurf.pax", n) == 0) ||
           (n == sizeof("netsurf-modern.pax") - 1 && memcmp(base, "netsurf-modern.pax", n) == 0) ||
           (n == sizeof("psram_netsurf.pax") - 1 && memcmp(base, "psram_netsurf.pax", n) == 0);
}
