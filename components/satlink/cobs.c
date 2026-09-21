#include "satlink/cobs.h"

void satlink_cobs_enc_begin(satlink_cobs_enc_t *e, uint8_t *out, size_t cap)
{
    e->out      = out;
    e->cap      = cap;
    e->len      = 1;     /* out[0] is reserved for the first run length */
    e->code_pos = 0;
    e->code     = 1;
    e->open     = true;
    e->ok       = (out != NULL && cap >= 1);
}

void satlink_cobs_enc_push(satlink_cobs_enc_t *e, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    if (!e->ok || data == NULL) {
        e->ok = false;
        return;
    }

    for (size_t i = 0; i < len; i++) {
        if (!e->open) {
            /* The previous run was a full one and ended on a push boundary.
             * Only now is it known that more input follows, so only now does
             * the next run get its code byte. */
            if (e->len >= e->cap) { e->ok = false; return; }
            e->code_pos = e->len++;
            e->code = 1;
            e->open = true;
        }
        if (data[i] != 0) {
            if (e->len >= e->cap) { e->ok = false; return; }
            e->out[e->len++] = data[i];
            if (++e->code != 0xFF) {
                continue;
            }
            /* A full run of 254 non-zero bytes: close it, but do not open the
             * next one yet. Whether anything follows is not known until the
             * next byte arrives, and a run opened here for input that never
             * comes is a trailing 0x01 -- one byte over the ceil(n/254)
             * overhead the contract states, at every multiple of 254 bytes.
             * Both decoders accept that form, but the player's encoder stopped
             * emitting it (sat_link.cpp) and the two sides should agree byte
             * for byte. */
            e->out[e->code_pos] = e->code;
            e->open = false;
            continue;
        }
        /* A zero: the run so far stands for it. Close it and open the next. */
        e->out[e->code_pos] = e->code;
        if (e->len >= e->cap) { e->ok = false; return; }
        e->code_pos = e->len++;
        e->code = 1;
    }
}

bool satlink_cobs_enc_finish(satlink_cobs_enc_t *e, size_t *out_len)
{
    if (!e->ok || out_len == NULL) {
        return false;
    }
    if (e->open) {
        e->out[e->code_pos] = e->code;
    }
    *out_len = e->len;
    return true;
}

bool satlink_cobs_encode(const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_len == NULL || (in == NULL && in_len != 0)) {
        return false;
    }
    if (out_cap < SATLINK_COBS_MAX(in_len)) {
        return false;
    }

    satlink_cobs_enc_t e;
    satlink_cobs_enc_begin(&e, out, out_cap);
    satlink_cobs_enc_push(&e, in, in_len);
    return satlink_cobs_enc_finish(&e, out_len);
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
