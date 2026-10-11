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
