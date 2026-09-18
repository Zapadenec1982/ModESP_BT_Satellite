/*
 * Framing: 0x00 | COBS( type | seq | payload | crc16 ) | 0x00
 *
 * The encoder emits the trailing delimiter only. Frames sent back to back then
 * share one, which is what the spec allows, and a receiver treats anything
 * between two delimiters as a frame — an empty one is simply ignored.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "satlink/proto.h"

typedef enum {
    SATLINK_FRAME_OK = 0,
    SATLINK_FRAME_EMPTY,       /* nothing between two delimiters; not an error */
    SATLINK_FRAME_BAD_COBS,
    SATLINK_FRAME_RUNT,        /* decoded to less than type + seq + crc */
    SATLINK_FRAME_BAD_CRC,
    SATLINK_FRAME_TOO_LONG,
} satlink_frame_result_t;

/* Encodes one frame, trailing delimiter included. `out` needs SATLINK_WIRE_MAX
 * for the largest payload. */
bool satlink_frame_encode(uint8_t type, uint8_t seq,
                          const uint8_t *payload, size_t payload_len,
                          uint8_t *out, size_t out_cap, size_t *out_len);

/* Decodes the bytes between two delimiters.
 *
 * `scratch` must be at least SATLINK_FRAME_MAX and is where the decoded frame
 * lands; `*payload` points into it, so it is valid exactly as long as the scratch
 * is untouched. The buffer is the caller's because a frame is a kilobyte, and a
 * kilobyte on an embedded stack is how this firmware crashed once already. */
satlink_frame_result_t satlink_frame_decode(const uint8_t *cobs, size_t cobs_len,
                                            uint8_t *scratch, size_t scratch_cap,
                                            uint8_t *type, uint8_t *seq,
                                            const uint8_t **payload, size_t *payload_len);

/*
 * Byte-stream reassembly.
 *
 * Feed it whatever arrives from the UART, in whatever sized pieces. It calls back
 * once per good frame. A frame longer than the buffer is dropped and the receiver
 * keeps its place: that is the property COBS is here for, and it is what makes a
 * power-on in the middle of a transmission a non-event.
 */

typedef void (*satlink_rx_frame_cb)(void *ctx, uint8_t type, uint8_t seq,
                                    const uint8_t *payload, size_t payload_len);

typedef struct {
    uint8_t  buf[SATLINK_WIRE_MAX];      /* bytes as they arrive, still encoded */
    uint8_t  plain[SATLINK_FRAME_MAX];   /* where a frame is decoded, off the stack */
    size_t   len;
    bool     overrun;          /* current frame too long: discard to the next delimiter */

    bool     have_seq;
    uint8_t  last_seq;

    /* Counters, for the LOG line that explains a bad cable. */
    uint32_t frames_ok;
    uint32_t err_cobs;
    uint32_t err_crc;
    uint32_t err_runt;
    uint32_t err_too_long;
    uint32_t seq_gaps;         /* frames the other side sent that never arrived */
} satlink_rx_t;

void satlink_rx_init(satlink_rx_t *rx);

void satlink_rx_feed(satlink_rx_t *rx, const uint8_t *data, size_t len,
                     satlink_rx_frame_cb cb, void *ctx);
