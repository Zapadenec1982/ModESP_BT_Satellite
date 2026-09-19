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
