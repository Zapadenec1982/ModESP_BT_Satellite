#include "satlink/frame.h"

#include <string.h>

#include "satlink/cobs.h"
#include "satlink/crc16.h"

#define HDR_LEN   2   /* type + seq */
#define CRC_LEN   2

bool satlink_frame_encode(uint8_t type, uint8_t seq,
                          const uint8_t *payload, size_t payload_len,
                          uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out == NULL || out_len == NULL) {
        return false;
    }
    if (payload_len > SATLINK_PAYLOAD_MAX) {
        return false;
    }
    if (payload == NULL && payload_len != 0) {
        return false;
    }

    uint8_t plain[SATLINK_FRAME_MAX];
    plain[0] = type;
    plain[1] = seq;
    if (payload_len != 0) {
        memcpy(plain + HDR_LEN, payload, payload_len);
    }

    /* The CRC covers type | seq | payload, before encoding. */
    uint16_t crc = satlink_crc16(plain, HDR_LEN + payload_len);
    plain[HDR_LEN + payload_len]     = (uint8_t)(crc & 0xFF);         /* little-endian */
    plain[HDR_LEN + payload_len + 1] = (uint8_t)(crc >> 8);

    size_t plain_len = HDR_LEN + payload_len + CRC_LEN;

    size_t encoded = 0;
    if (!satlink_cobs_encode(plain, plain_len, out, out_cap, &encoded)) {
        return false;
    }
    if (encoded + 1 > out_cap) {
        return false;
    }
    out[encoded] = SATLINK_DELIMITER;
    *out_len = encoded + 1;
    return true;
}

satlink_frame_result_t satlink_frame_decode(const uint8_t *cobs, size_t cobs_len,
                                            uint8_t *type, uint8_t *seq,
                                            uint8_t *payload, size_t payload_cap,
                                            size_t *payload_len)
{
    if (cobs_len == 0) {
        return SATLINK_FRAME_EMPTY;
    }
    if (cobs_len > SATLINK_COBS_MAX(SATLINK_FRAME_MAX)) {
        return SATLINK_FRAME_TOO_LONG;
    }

    uint8_t plain[SATLINK_FRAME_MAX];
    size_t plain_len = 0;
    if (!satlink_cobs_decode(cobs, cobs_len, plain, sizeof(plain), &plain_len)) {
        return SATLINK_FRAME_BAD_COBS;
    }
    if (plain_len < HDR_LEN + CRC_LEN) {
        return SATLINK_FRAME_RUNT;
    }

    size_t body_len = plain_len - CRC_LEN;
    uint16_t want = (uint16_t)plain[body_len] | (uint16_t)((uint16_t)plain[body_len + 1] << 8);
    if (satlink_crc16(plain, body_len) != want) {
        return SATLINK_FRAME_BAD_CRC;
    }

    size_t len = body_len - HDR_LEN;
    if (len > payload_cap) {
        return SATLINK_FRAME_TOO_LONG;
    }

    if (type != NULL) { *type = plain[0]; }
    if (seq  != NULL) { *seq  = plain[1]; }
    if (len != 0 && payload != NULL) {
        memcpy(payload, plain + HDR_LEN, len);
    }
    if (payload_len != NULL) { *payload_len = len; }
    return SATLINK_FRAME_OK;
}

void satlink_rx_init(satlink_rx_t *rx)
{
    if (rx != NULL) {
        memset(rx, 0, sizeof(*rx));
    }
}

static void rx_deliver(satlink_rx_t *rx, satlink_rx_frame_cb cb, void *ctx)
{
    uint8_t type = 0, seq = 0;
    uint8_t payload[SATLINK_PAYLOAD_MAX];
    size_t payload_len = 0;

    switch (satlink_frame_decode(rx->buf, rx->len, &type, &seq,
                                 payload, sizeof(payload), &payload_len)) {
    case SATLINK_FRAME_OK:
        rx->frames_ok++;
        if (rx->have_seq) {
            /* Wraps at 256 by construction, so the subtraction is the gap. */
            uint8_t gap = (uint8_t)(seq - rx->last_seq);
            if (gap > 1) {
                rx->seq_gaps += (uint32_t)(gap - 1);
            }
        }
        rx->have_seq = true;
        rx->last_seq = seq;
        if (cb != NULL) {
            cb(ctx, type, seq, payload, payload_len);
        }
        break;

    case SATLINK_FRAME_EMPTY:   break;   /* two delimiters in a row; normal */
    case SATLINK_FRAME_BAD_COBS: rx->err_cobs++;     break;
    case SATLINK_FRAME_BAD_CRC:  rx->err_crc++;      break;
    case SATLINK_FRAME_RUNT:     rx->err_runt++;     break;
    case SATLINK_FRAME_TOO_LONG: rx->err_too_long++; break;
    }
}

void satlink_rx_feed(satlink_rx_t *rx, const uint8_t *data, size_t len,
                     satlink_rx_frame_cb cb, void *ctx)
{
    if (rx == NULL || (data == NULL && len != 0)) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        if (byte != SATLINK_DELIMITER) {
            if (rx->len < sizeof(rx->buf)) {
                rx->buf[rx->len++] = byte;
            } else {
                /* Too long for anything this protocol defines. Drop the rest of
                 * it; the next delimiter puts us back in step. */
                rx->overrun = true;
            }
            continue;
        }

        if (rx->overrun) {
            rx->err_too_long++;
            rx->overrun = false;
        } else {
            rx_deliver(rx, cb, ctx);
        }
        rx->len = 0;
    }
}
