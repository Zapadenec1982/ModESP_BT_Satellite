/*
 * Consistent Overhead Byte Stuffing.
 *
 * The point of it here is resynchronisation: an encoded frame contains no zero
 * bytes, so a receiver that joins mid-stream, or loses its place, is back in step
 * at the very next delimiter. Nothing else in the link has to handle that case.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Worst-case encoded size for `n` bytes of input: one overhead byte per 254, plus
 * the leading code byte. Does not include the trailing delimiter. */
#define SATLINK_COBS_MAX(n)   ((n) + ((n) / 254u) + 1u)

/*
 * Streaming encoder.
 *
 * It exists so that a frame can be built straight into the output buffer from its
 * pieces — header, payload, checksum — without first concatenating them somewhere.
 * On a microcontroller that somewhere is a kilobyte of stack, in whatever task
 * happened to call, and that is exactly how the first build of this firmware
 * overflowed the main task.
 */
typedef struct {
    uint8_t *out;
    size_t   cap;
    size_t   len;        /* bytes written, including the pending code byte */
    size_t   code_pos;   /* where the run length for the current run goes */
    uint8_t  code;       /* length of the current run, counting the code byte */
    bool     ok;         /* sticky: one overflow and the rest is a no-op */
} satlink_cobs_enc_t;

void satlink_cobs_enc_begin(satlink_cobs_enc_t *e, uint8_t *out, size_t cap);
void satlink_cobs_enc_push(satlink_cobs_enc_t *e, const uint8_t *data, size_t len);
bool satlink_cobs_enc_finish(satlink_cobs_enc_t *e, size_t *out_len);

/* One-shot form, for the tests and for callers that already have the bytes in one
 * place. Writes no delimiter: the caller adds it. */
bool satlink_cobs_encode(const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t out_cap, size_t *out_len);

/* Decodes one frame's worth of encoded bytes, delimiters already stripped.
 * Returns false on a malformed frame: a zero byte inside, or a code byte that
 * runs off the end. */
bool satlink_cobs_decode(const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t out_cap, size_t *out_len);
