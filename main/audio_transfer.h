/* Bounded, cancellation-aware byte transfer shared by PCM enqueue and I2S TX.
 * The caller owns the source until this returns, including on partial writes. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    void *ctx;
    bool (*current)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    /* Must wait for at most one bounded slice on no progress. */
    bool (*send)(void *ctx, const uint8_t *data, size_t len, size_t *sent);
} audio_transfer_ops_t;
typedef enum { AUDIO_TRANSFER_OK, AUDIO_TRANSFER_CANCELLED,
               AUDIO_TRANSFER_TIMEOUT, AUDIO_TRANSFER_ERROR } audio_transfer_result_t;
static inline audio_transfer_result_t audio_transfer(
    const audio_transfer_ops_t *ops, const uint8_t *data, size_t len,
    uint32_t start_ms, uint32_t budget_ms, size_t *accepted)
{
    *accepted = 0;
    while (*accepted < len) {
        if (!ops->current(ops->ctx)) return AUDIO_TRANSFER_CANCELLED;
        if ((uint32_t)(ops->now_ms(ops->ctx) - start_ms) >= budget_ms)
            return AUDIO_TRANSFER_TIMEOUT;
        size_t sent = 0;
        bool retry = ops->send(ops->ctx, data + *accepted, len - *accepted, &sent);
        if (sent > len - *accepted) return AUDIO_TRANSFER_ERROR;
        *accepted += sent;
        if (!retry) return AUDIO_TRANSFER_ERROR;
    }
    return AUDIO_TRANSFER_OK;
}
/* Fixed memory budget: 12 ms at the current format, capped to half the ring.
 * DMA-paced zeros are sent while this reserve accumulates. */
static inline size_t audio_prefill_bytes(uint32_t rate, unsigned frame, size_t capacity)
{
    size_t bytes = (size_t)rate * frame * 12u / 1000u;
    if (bytes > capacity / 2u) bytes = capacity / 2u;
    return bytes - bytes % frame;
}
