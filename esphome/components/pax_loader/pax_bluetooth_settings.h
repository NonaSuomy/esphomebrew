// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include "pax_bt_discovery.h"
#define PAX_BLUETOOTH_FILE "/dev/pax-bluetooth"
#define PAX_BLUETOOTH_MAGIC 0x50425431U
#define PAX_BLUETOOTH_MAX_DEVICES 16
typedef struct {uint32_t magic,available,busy,generation,count,capabilities;char message[128];pax_bt_discovery_event_t devices[PAX_BLUETOOTH_MAX_DEVICES];} pax_bluetooth_status_t;
typedef struct {uint32_t magic;pax_bt_discovery_command_t command;} pax_bluetooth_request_t;
