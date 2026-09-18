#include "satlink/cobs.h"

bool satlink_cobs_encode(const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out == NULL || out_len == NULL || (in == NULL && in_len != 0)) {
        return false;
    }
    if (out_cap < SATLINK_COBS_MAX(in_len)) {
        return false;
    }

    size_t code_pos = 0;    /* where the current run length goes */
    size_t pos      = 1;    /* first free byte after it */
    uint8_t code    = 1;    /* run length so far, including the code byte itself */

    for (size_t i = 0; i < in_len; i++) {
        if (in[i] != 0) {
            out[pos++] = in[i];
            code++;
            if (code != 0xFF) {
                continue;
            }
            /* A full run of 254 non-zero bytes: close it and start another. */
        }
        out[code_pos] = code;
        code_pos = pos++;
        code = 1;
    }

    out[code_pos] = code;
    *out_len = pos;
    return true;
}

bool satlink_cobs_decode(const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out == NULL || out_len == NULL || (in == NULL && in_len != 0)) {
        return false;
    }

    size_t src = 0;
    size_t dst = 0;

    while (src < in_len) {
        uint8_t code = in[src++];
        if (code == 0) {
            return false;   /* a delimiter inside the frame: not our encoding */
        }

        /* code - 1 literal bytes follow, and none of them may be a zero: an
         * encoder never emits one, so seeing one means these bytes did not come
         * from one. The stream reassembler splits on zeros before we ever get
         * here, but this is a public entry point and the contract is the
         * contract. */
        for (uint8_t i = 1; i < code; i++) {
            if (src >= in_len || dst >= out_cap || in[src] == 0) {
                return false;
            }
            out[dst++] = in[src++];
        }

        /* A short run stood for a zero, unless the frame ends right here. */
        if (code != 0xFF && src < in_len) {
            if (dst >= out_cap) {
                return false;
            }
            out[dst++] = 0;
        }
    }

    *out_len = dst;
    return true;
}
