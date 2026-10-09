// NetSurf's resources compiled into the app (.pax-gen/pax_resources.c,
// written by gen_netsurf.py): what resource: URLs serve and the Messages file.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct pax_resource {
    const char *name;     // resource: path, e.g. "default.css"
    const uint8_t *data;  // NUL-terminated
    size_t len;           // without the NUL
};

// Ends with a {NULL, NULL, 0} entry.
extern const struct pax_resource pax_resources[];

// English Messages for the framebuffer frontend, "key:value" lines.
extern const uint8_t *const pax_messages;
extern const size_t pax_messages_len;
