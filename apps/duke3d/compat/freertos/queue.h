/* Stub for PSRAM app build */
#ifndef PAX_COMPAT_FREERTOS_QUEUE_H
#define PAX_COMPAT_FREERTOS_QUEUE_H

#include "FreeRTOS.h"

static inline QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t size) {
    (void)len; (void)size; return (QueueHandle_t)1;
}

#endif
