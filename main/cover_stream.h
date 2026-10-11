#pragma once
#include <stdbool.h>
#include <string.h>
#include "satlink/cover_wire.h"

/* Callback bytes are borrowed from Bluedroid. emit must copy, never wait.
 * Unknown-length DATA is committed only by END after successful OBEX final. */
typedef struct {
    uint32_t used, crc;
    uint8_t magic[3];
    bool failed, finished;
} cover_stream_t;
typedef bool (*cover_emit_t)(void *, uint32_t, const uint8_t *, size_t, uint32_t);

static inline bool cover_stream_feed(cover_stream_t *s, const uint8_t *p,
                                      size_t len, bool final, cover_emit_t emit, void *ctx) {
    if (s->failed || s->finished) return false;
    if ((!p && len) || len > COVER_BYTES_MAX-s->used) goto fail;
    while (len) {
        size_t n=len>COVER_PART_MAX?COVER_PART_MAX:len;
        for (size_t i=0; i<n && s->used+i<3; ++i) s->magic[s->used+i]=p[i];
        if (s->used+n>=3 && (s->magic[0]!=0xff || s->magic[1]!=0xd8 || s->magic[2]!=0xff)) goto fail;
        if (!emit(ctx,s->used,p,n,0)) goto fail;
        s->crc=cover_crc32(s->crc,p,n);
        s->used+=(uint32_t)n; p+=n; len-=n;
    }
    if (final) {
        if (s->used<3 || !emit(ctx,s->used,NULL,0,s->crc)) goto fail;
        s->finished=true;
    }
    return true;
fail:
    s->failed=true;
    return false;
}

/* The OBEX channel closed. The current track's image is fetched again only when the
 * transfer that died was the old track's, drained after a track change: no byte of
 * the current generation reached the host then, and its handle is already waiting.
 * drain_failed: such a drain ended in an OBEX error, which also closes the channel;
 * busy/stale: the transfer still in flight at the close, if any. A transfer of the
 * current generation that dies is left to the host's COVER_RETRY, which never
 * repeats a generation. */
static inline bool cover_refetch_on_close(bool drain_failed, bool busy, bool stale,
                                          bool has_handle) {
    if (busy && !stale) return false;
    return (drain_failed || busy) && has_handle;
}

/* The device's own bound on COVER_RETRY (PROTOCOL 10.5): at most COVER_RETRY_MAX
 * repeats per track, counted from the last AVRCP track change or the last successful
 * transfer. A transfer that ended in COVER_END is successful only until the host
 * names its generation in COVER_RETRY: the host saw it fail, typically a part lost
 * on the UART, which the device cannot see. Without that, every honoured retry is
 * followed by a "successful" transfer that refills the allowance, and a link that
 * loses a part of every image gets OBEX repeats for as long as the host keeps asking.
 * Not thread-safe: the caller serialises the three calls. Generation 0 is never valid. */
typedef struct {
    unsigned used;          // repeats honoured since the last refill
    unsigned refill_prev;   // what the last refill took away
    uint32_t refill_gen;    // the generation whose completion refilled
    uint32_t retried_gen;   // the last generation a retry was honoured for
} cover_retry_t;

static inline void cover_retry_track_change(cover_retry_t *r) {
    *r = (cover_retry_t){0};
}

/* The device sent COVER_END for gen. A generation the host has already named in a
 * retry -- it can ask before the device's OBEX final -- is not evidence either. */
static inline void cover_retry_completed(cover_retry_t *r, uint32_t gen) {
    if (gen == r->retried_gen) return;
    r->refill_prev = r->used;
    r->refill_gen = gen;
    r->used = 0;
}

/* The host asked for gen, the current generation, again. True: honour it. */
static inline bool cover_retry_take(cover_retry_t *r, uint32_t gen) {
    unsigned used = r->used;
    if (gen == r->refill_gen && r->refill_prev > used) used = r->refill_prev;
    if (used >= COVER_RETRY_MAX) return false;
    r->used = used + 1;
    r->retried_gen = gen;
    return true;
}
