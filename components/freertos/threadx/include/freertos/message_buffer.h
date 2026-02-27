/*
 * message_buffer.h — Stub for ThreadX compat layer
 * Message buffers are stream buffers with framing. Redirect to stream_buffer.h.
 */
#ifndef FREERTOS_MESSAGE_BUFFER_H
#define FREERTOS_MESSAGE_BUFFER_H

#include "FreeRTOS.h"
#include "stream_buffer.h"

#define xMessageBufferCreate(xBufferSizeBytes) \
    xStreamBufferGenericCreateWithCaps(xBufferSizeBytes, 0, pdTRUE, 0)

#define xMessageBufferCreateWithCaps(xBufferSizeBytes, uxMemoryCaps) \
    xStreamBufferGenericCreateWithCaps(xBufferSizeBytes, 0, pdTRUE, uxMemoryCaps)

#define vMessageBufferDelete(xMessageBuffer) \
    vStreamBufferGenericDeleteWithCaps(xMessageBuffer, pdTRUE)

#endif /* FREERTOS_MESSAGE_BUFFER_H */
