/* Stub for PSRAM app build */
#ifndef PAX_COMPAT_FREERTOS_TASK_H
#define PAX_COMPAT_FREERTOS_TASK_H

#include "FreeRTOS.h"

static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }
static inline void taskYIELD(void) {}

#endif
