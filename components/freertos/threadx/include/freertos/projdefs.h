// SPDX-License-Identifier: Apache-2.0
/* projdefs.h — minimal FreeRTOS project definitions for ESP-IDF compat */
#ifndef PROJDEFS_H_WRAPPER
#define PROJDEFS_H_WRAPPER

#include "freertos/FreeRTOS.h"

/* These are already defined in the upstream compat FreeRTOS.h but some
 * ESP-IDF components include projdefs.h directly. */
#ifndef pdTRUE
#define pdTRUE      ((BaseType_t)1)
#endif
#ifndef pdFALSE
#define pdFALSE     ((BaseType_t)0)
#endif
#ifndef pdPASS
#define pdPASS      (pdTRUE)
#endif
#ifndef pdFAIL
#define pdFAIL      (pdFALSE)
#endif

#endif
