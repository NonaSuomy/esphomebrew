// Tiberian Sun PAX port: loader ABI declarations shared by staged probes.
#pragma once

#ifndef PAX_APP_SIDE
#define PAX_APP_SIDE 1
#endif

#if defined(__cplusplus) && !defined(_Static_assert)
#define _Static_assert static_assert
#endif

#include "esphome/components/pax_loader/psram_app.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const app_services_t *pax_svc;

#define PAX_TS_DATA_DIR "/sd/roms/tiberiansun"

#ifdef __cplusplus
}
#endif
