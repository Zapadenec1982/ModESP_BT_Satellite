/* Small event FIFO. Callers serialize access; no allocation, logging or RTOS calls.
 * Timestamps and cumulative counters are low32 and wrap modulo 2^32 (us: 71.6 min).
 * Overflow drops the newest event and is always reported, never silently overwritten. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { AUDIO_EVENT_REBUFFER_BEGIN = 1, AUDIO_EVENT_REBUFFER_END, AUDIO_EVENT_TRIM };
enum { AUDIO_REBUFFER_EMPTY = 1, AUDIO_REBUFFER_STARTUP,
       AUDIO_REBUFFER_READY, AUDIO_REBUFFER_CANCELLED };
typedef struct {
    uint32_t kind, begin_us, end_us, gate, epoch;
    int32_t a, b, c;
    int32_t dma_proxy_pct; /* rebuffer begin only; -1 unavailable, not PCM headroom */
} audio_event_t;
typedef struct { unsigned read, count; uint32_t overflow; } audio_event_fifo_t;
static inline void audio_event_push(audio_event_fifo_t *q, audio_event_t *storage,
                                    unsigned capacity, audio_event_t event)
{
    if (q->count == capacity) { q->overflow++; return; }
    storage[(q->read + q->count) % capacity] = event;
    q->count++;
}
static inline bool audio_event_pop(audio_event_fifo_t *q, const audio_event_t *storage,
                                   unsigned capacity, audio_event_t *event)
{
    if (!q->count) return false;
    *event = storage[q->read];
    q->read = (q->read + 1u) % capacity;
    q->count--;
    return true;
}

typedef struct {
    uint32_t zero_frames, zero_writes, zero_last_us, pcm_last_us;
    uint32_t zero_gate, zero_epoch, zero_cancel_frames;
    uint32_t rebuffer_begin_us, rebuffer_end_us;
    uint32_t begin_us, gate, epoch, zero_base;
    bool active;
} audio_event_state_t;
static inline void audio_event_zeros(audio_event_state_t *s, uint32_t now,
                                      uint32_t gate, uint32_t epoch, unsigned frame_bytes,
                                      unsigned accepted, bool streaming_open, bool cancelled)
{
    if (!streaming_open || !frame_bytes || accepted < frame_bytes) return;
    s->zero_frames += accepted / frame_bytes;
    if (cancelled) s->zero_cancel_frames += accepted / frame_bytes;
    s->zero_writes++;
    s->zero_last_us = now;
    s->zero_gate = gate; s->zero_epoch = epoch;
}
static inline bool audio_event_rebuffer(audio_event_state_t *s, audio_event_t *event,
                                        uint32_t now, uint32_t gate, uint32_t epoch,
                                        uint32_t fill, unsigned reason)
{
    const bool begin = reason == AUDIO_REBUFFER_EMPTY || reason == AUDIO_REBUFFER_STARTUP;
    if (begin) {
        if (s->active) return false;
        s->active = true;
        s->begin_us = s->rebuffer_begin_us = now;
        s->gate = gate; s->epoch = epoch; s->zero_base = s->zero_frames;
        *event = (audio_event_t){AUDIO_EVENT_REBUFFER_BEGIN, now, now, gate, epoch,
                                (int32_t)fill, (int32_t)s->pcm_last_us, (int32_t)reason, -1};
    } else {
        if (!s->active) return false;
        s->active = false;
        s->rebuffer_end_us = now;
        *event = (audio_event_t){AUDIO_EVENT_REBUFFER_END, s->begin_us, now, s->gate, s->epoch,
                                (int32_t)(s->zero_frames - s->zero_base), (int32_t)fill,
                                (int32_t)reason, -1};
    }
    return true;
}
