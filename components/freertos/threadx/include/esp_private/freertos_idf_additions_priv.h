// SPDX-License-Identifier: Apache-2.0
/*
 * freertos_idf_additions_priv.h — Stub for ESP-IDF private FreeRTOS additions
 *
 * The real header provides kernel-level macros (prvENTER_CRITICAL_OR_SUSPEND_ALL,
 * etc.) for thread-safe flash operations. On ThreadX, we provide no-op stubs
 * since cache_utils.c includes this but doesn't use the macros on single-core.
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
