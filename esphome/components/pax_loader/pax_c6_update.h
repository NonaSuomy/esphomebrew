#pragma once
#include <stdint.h>
/* ABI-1 virtual file: no service table changes. Requests commit on close. */
#define PAX_C6_UPDATE_FILE "/dev/pax-c6-update"
#define PAX_C6_UPDATE_MAGIC 0x50433631U
#define PAX_C6_PREPARE 1U
#define PAX_C6_INSTALL 2U
// Explicit recovery opt-in: permit a missing version RPC, never a different image.
#define PAX_C6_PREPARE_UNKNOWN 3U
#define PAX_C6_IDLE 0U
#define PAX_C6_BUSY 1U
#define PAX_C6_READY 2U
#define PAX_C6_REBOOTING 3U
#define PAX_C6_ERROR 4U
#define PAX_C6_CURRENT 5U
#define PAX_C6_RECOMMENDED "board-recommended"
typedef struct { uint32_t magic, command; char source[512]; } pax_c6_request_t;
typedef struct { uint32_t magic, state, done, total; char message[160]; } pax_c6_status_t;
#ifdef __cplusplus
#include <cstdio>
namespace pax_c6 {
inline uint32_t prepare_state(uint32_t command, bool version_known, bool newer) {
  if (!version_known) return command == PAX_C6_PREPARE_UNKNOWN ? PAX_C6_READY : PAX_C6_ERROR;
  return newer ? PAX_C6_READY : PAX_C6_CURRENT;
}
struct Info { pax_c6_status_t status; char profile[48], installed_version[32], recommended_version[32]; };
FILE *open(const char *mode); void loop(); bool busy(); Info snapshot();
}
#endif
