/*
 * SPDX-FileCopyrightText: 2026 Zapadenec1982
 * SPDX-License-Identifier: Apache-2.0
 *
 * One checked RTP media header stripper, shared by every decoder.
 *
 * Why this file exists. In stock Bluedroid, avdt_scb_hdl_pkt_no_frag() parses the
 * RTP header and hands the payload up. That cannot stay: aptX carries no RTP header
 * at all, aptX HD carries one, AAC carries one and then LATM framing, so only the
 * decoder knows what to strip. The multi-codec framework therefore moves the parse
 * into the decoders.
 *
 * What must not be lost in the move is the bounds checking. Between v5.5.2 and
 * v5.5.5 Espressif hardened that AVDTP routine: every field that advances the
 * offset is now checked against the packet length first. The donor this port draws
 * from was cut against 5.5.2 and its per-decoder copies check nothing — one of them
 * also indexes a struct pointer where it means a byte pointer, and another reads the
 * padding byte one past the end. A media packet arrives from the air, from whatever
 * the peer chose to send, so `p_buf->len -= header_len` on an unchecked header_len
 * underflows a UINT16 into about 65000 and the decoder then reads far off the end of
 * the buffer.
 *
 * So the parse moves, and the checks move with it, once.
 */

#include "common/bt_target.h"

#if (A2D_INCLUDED == TRUE)

#include "common/bt_trace.h"
#include "stack/a2dp_decoder.h"

#define RTP_FIXED_HEADER_LEN    (sizeof(struct media_packet_header))   /* 12 */
#define RTP_EXT_HEADER_LEN      (4)   /* profile id (2) + length in words (2) */

ssize_t a2dp_rtp_strip_header(BT_HDR *p_buf)
{
    if (p_buf == NULL) {
        return -1;
    }

    const uint8_t *base = (const uint8_t *)(p_buf + 1) + p_buf->offset;
    const uint16_t len  = p_buf->len;
    const struct media_packet_header *hdr = (const struct media_packet_header *)base;

    if (len < RTP_FIXED_HEADER_LEN) {
        APPL_TRACE_WARNING("%s: packet of %u bytes is shorter than an RTP header", __func__, len);
        return -1;
    }

    size_t header_len = RTP_FIXED_HEADER_LEN + (size_t)hdr->cc * sizeof(uint32_t);
    if (len < header_len) {
        APPL_TRACE_WARNING("%s: %u CSRC entries do not fit in %u bytes", __func__, hdr->cc, len);
        return -1;
    }

    if (hdr->x) {
        if (len < header_len + RTP_EXT_HEADER_LEN) {
            APPL_TRACE_WARNING("%s: extension header does not fit in %u bytes", __func__, len);
            return -1;
        }
        /* Byte arithmetic: base is a byte pointer on purpose. The length is the
         * second halfword of the extension header, big endian, counted in words. */
        const uint8_t *p_ext = base + header_len;
        uint32_t ext_len = (((uint32_t)p_ext[2] << 8) | p_ext[3]) * 4;
        header_len += RTP_EXT_HEADER_LEN;
        if (ext_len > (uint32_t)(len - header_len)) {
            APPL_TRACE_WARNING("%s: extension of %u bytes does not fit in %u", __func__,
                               (unsigned)ext_len, len);
            return -1;
        }
        header_len += ext_len;
    }

    if (len <= header_len) {
        APPL_TRACE_WARNING("%s: nothing left after a %u byte header in %u bytes", __func__,
                           (unsigned)header_len, len);
        return -1;
    }

    uint8_t pad_len = 0;
    if (hdr->p) {
        /* Padding length lives in the last byte of the packet, which is len - 1. */
        pad_len = base[len - 1];
        if (pad_len >= len - header_len) {
            APPL_TRACE_WARNING("%s: %u bytes of padding in a %u byte payload", __func__,
                               pad_len, (unsigned)(len - header_len));
            return -1;
        }
    }

    /* The sequence number is what the sink uses to notice dropped packets. */
    p_buf->layer_specific = ((uint16_t)base[2] << 8) | base[3];

    p_buf->offset += header_len;
    p_buf->len     = len - header_len - pad_len;

    return (ssize_t)header_len;
}

#endif /* A2D_INCLUDED == TRUE */
