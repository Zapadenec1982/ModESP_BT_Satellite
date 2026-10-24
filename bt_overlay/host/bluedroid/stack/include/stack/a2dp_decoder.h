/*
 * SPDX-FileContributor: 2026 Zapadenec1982
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef A2DP_DECODER_H
#define A2DP_DECODER_H

/*****************************************************************************
**  Type Definitions
*****************************************************************************/
struct __attribute__ ((packed)) media_packet_header {
    uint8_t cc:4;
    uint8_t x:1;
    uint8_t p:1;
    uint8_t v:2;

    uint8_t pt:7;
    uint8_t marker:1;

    uint16_t seq;
    uint32_t timestamp;

    uint32_t ssrc;
    uint32_t csrc[0];
};

struct __attribute__ ((packed)) media_payload_header {
    uint8_t frame_count:4;
    uint8_t rfa0:1;
    uint8_t is_last_fragment:1;
    uint8_t is_first_fragment:1;
    uint8_t is_fragmented:1;
};

/*****************************************************************************
**  Functions
*****************************************************************************/

/* Strips the RTP media header from a received packet, advancing p_buf->offset and
 * shrinking p_buf->len past it, and stores the sequence number in layer_specific.
 *
 * Returns the number of header bytes consumed, or -1 if the packet is malformed,
 * in which case p_buf is left untouched and the caller must drop it. Every field
 * that moves the offset is checked against the packet length first: the packet
 * comes off the air and the peer decides what is in it.
 *
 * Codecs that carry no RTP header, such as plain aptX, do not call this. */
ssize_t a2dp_rtp_strip_header(BT_HDR *p_buf);

#endif // A2DP_DECODER_H
