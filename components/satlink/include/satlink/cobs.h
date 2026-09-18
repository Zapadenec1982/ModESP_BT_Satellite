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

/* Encodes `in_len` bytes. Writes no delimiter: the caller adds it.
 * Returns false and leaves *out_len untouched if `out_cap` is too small. */
bool satlink_cobs_encode(const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t out_cap, size_t *out_len);

/* Decodes one frame's worth of encoded bytes, delimiters already stripped.
 * Returns false on a malformed frame: a zero byte inside, or a code byte that
 * runs off the end. */
bool satlink_cobs_decode(const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t out_cap, size_t *out_len);
