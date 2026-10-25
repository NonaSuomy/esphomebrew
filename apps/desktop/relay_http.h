#ifndef DESKTOP_RELAY_HTTP_H
#define DESKTOP_RELAY_HTTP_H
#include "psram_app.h"
/* Reads URL + token from /sd/roms/desktop/relay.conf. No credential logging. */
const char *relay_start_vnc(const app_services_t *, const char *, unsigned,
                            const char *, int (*pump)(void));
#endif
