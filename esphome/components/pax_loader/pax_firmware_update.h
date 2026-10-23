#pragma once
#include <stdint.h>
/* ABI-1 virtual file. Opening Settings never starts an installation. */
#define PAX_FIRMWARE_FILE "/dev/pax-firmware-update"
#define PAX_FIRMWARE_MAGIC 0x50465731U
#define PAX_FIRMWARE_CHECK 1U
#define PAX_FIRMWARE_INSTALL 2U
#define PAX_FIRMWARE_IDLE 0U
#define PAX_FIRMWARE_CHECKING 1U
#define PAX_FIRMWARE_CURRENT 2U
#define PAX_FIRMWARE_AVAILABLE 3U
#define PAX_FIRMWARE_WAITING 4U
#define PAX_FIRMWARE_INSTALLING 5U
#define PAX_FIRMWARE_REBOOTING 6U
#define PAX_FIRMWARE_ERROR 7U
typedef struct {
  uint32_t magic, command;
  uint64_t expected_build;
} pax_firmware_request_t;
typedef struct {
  uint32_t magic, state, done, total;
  uint64_t installed_build, latest_build;
  char profile[48], message[192];
} pax_firmware_status_t;
#ifdef __cplusplus
#include <cstdio>
namespace pax_firmware {
FILE *open(const char *mode);
bool pending();
bool busy();
void loop(bool app_closed);
pax_firmware_status_t snapshot();
} // namespace pax_firmware
#endif
