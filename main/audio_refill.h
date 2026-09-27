/* One bounded refill attempt after an empty nonblocking receive in steady
 * playback. This does not estimate DMA reserve or claim to avoid an underrun.
 * A tick is the caller's RTOS tick, not one millisecond. Scheduling can extend
 * the observed wait beyond the receive timeout; elapsed_us reports that too. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    /* Compare the captured gate AND epoch, and require open gate, streaming,
     * and running. The caller must serialize format changes while using the
     * returned item (the writer's existing s_lock provides that ownership). */
    bool (*current)(void *ctx);
    uint32_t (*now_us)(void *ctx);
    /* Borrow up to max_bytes of contiguous data; partial frames are allowed.
     * NULL means timeout. A non-NULL item has a positive *len <= max_bytes. */
    void *(*receive)(void *ctx, size_t *len, unsigned wait_ticks, size_t max_bytes);
    void (*return_item)(void *ctx, void *item);
} audio_refill_ops_t;

typedef enum {
    AUDIO_REFILL_RECOVERED,
    AUDIO_REFILL_TIMEOUT,
    AUDIO_REFILL_CANCELLED
} audio_refill_status_t;

typedef struct {
    audio_refill_status_t status;
    /* RECOVERED transfers the borrowed item to the caller. CANCELLED returns
     * any borrowed item exactly once here, and exposes no stale pointer/length. */
    void *item;
    size_t len;
    uint32_t elapsed_us;
} audio_refill_result_t;

static inline audio_refill_result_t audio_refill_once(
    const audio_refill_ops_t *ops, size_t max_bytes)
{
    const uint32_t start = ops->now_us(ops->ctx);
    audio_refill_result_t result = { AUDIO_REFILL_CANCELLED, NULL, 0, 0 };
    if (ops->current(ops->ctx)) {
        size_t len = 0;
        void *item = ops->receive(ops->ctx, &len, 1u, max_bytes);
        if (!ops->current(ops->ctx)) {
            if (item) ops->return_item(ops->ctx, item);
        } else if (item) {
            result.status = AUDIO_REFILL_RECOVERED;
            result.item = item;
            result.len = len;
        } else {
            result.status = AUDIO_REFILL_TIMEOUT;
        }
    }
    /* Unsigned subtraction remains valid across one uint32_t timer wrap. */
    result.elapsed_us = (uint32_t)(ops->now_us(ops->ctx) - start);
    return result;
}
