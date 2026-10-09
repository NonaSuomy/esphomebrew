/* The launcher uses the same freestanding C runtime as the LVGL reference
 * app.  Keeping one implementation avoids subtly different memcpy/memset
 * code in the two PAXs. */
#include "../lvgl/pax_libc.c"
