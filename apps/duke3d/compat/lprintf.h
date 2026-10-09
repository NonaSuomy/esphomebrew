/* The shared MIDI parser was written for PrBoom's logging layer.  Duke's
 * PAX already has its own launcher logger, and parser warnings are not
 * allowed to call into an unavailable Doom subsystem. */
#ifndef DUKE_PAX_LPRINTF_H
#define DUKE_PAX_LPRINTF_H

#define LO_WARN 0
#define lprintf(...) ((void)0)

#endif
