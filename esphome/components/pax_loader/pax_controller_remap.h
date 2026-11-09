// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <string.h>
#define PAX_CONTROLLER_FILE "/dev/pax-controller-map"
#define PAX_CONTROLLER_MAGIC 0x50434d31U
#define PAX_CONTROLLER_BUTTONS 12
#define PAX_CONTROLLER_UNBOUND 255
/* A shared normalized button/D-pad profile. MENU and VOLUME bypass remapping.
 * source[logical] names the physical normalized input, or UNBOUND. */
typedef struct { uint32_t magic; uint8_t source[PAX_CONTROLLER_BUTTONS]; } pax_controller_map_t;
typedef struct { pax_controller_map_t map; uint32_t raw, pending, save_failed; } pax_controller_status_t;
static inline void pax_controller_defaults(pax_controller_map_t *m) {
 m->magic=PAX_CONTROLLER_MAGIC;for(unsigned i=0;i<PAX_CONTROLLER_BUTTONS;i++)m->source[i]=i;
}
static inline int pax_controller_valid(const pax_controller_map_t *m) {
 if(m->magic!=PAX_CONTROLLER_MAGIC)return 0;
 uint32_t used=0;
 for(unsigned i=0;i<PAX_CONTROLLER_BUTTONS;i++){
  unsigned n=m->source[i];if(n==PAX_CONTROLLER_UNBOUND)continue;
  if(n>=PAX_CONTROLLER_BUTTONS || (used&(1U<<n)))return 0;
  used|=1U<<n;
 }return 1;
}
static inline uint32_t pax_controller_apply(const pax_controller_map_t *m,uint32_t raw) {
 uint32_t out=raw&~((1U<<PAX_CONTROLLER_BUTTONS)-1);
 for(unsigned i=0;i<PAX_CONTROLLER_BUTTONS;i++)if(m->source[i]<PAX_CONTROLLER_BUTTONS && (raw&(1U<<m->source[i])))out|=1U<<i;
 return out;
}
