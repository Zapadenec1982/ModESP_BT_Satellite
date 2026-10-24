/*
 * Framing tests: COBS, CRC-16 and the byte-stream reassembler.
 *
 * The cases that matter are the nasty ones - a receiver that joins mid-frame, a
 * flipped bit, a run of 254 non-zero bytes, a frame at the exact maximum. Those
 * are the ones that turn into a two-day bug on real hardware.
 */

#include <string.h>

#include "harness.h"

#include "satlink/codec.h"
#include "satlink/cobs.h"
#include "satlink/crc16.h"
#include "satlink/frame.h"
#include "satlink/proto.h"

/* ------------------------------------------------------------------ CRC-16 */

static void test_crc16(void)
{
    /* The check value every CRC catalogue lists for CCITT-FALSE. If this passes,
     * the other end's library and ours agree about which "CCITT" this is. */
    const char *s = "123456789";
    CHECK_EQ(satlink_crc16((const uint8_t *)s, 9), 0x29B1);

    CHECK_EQ(satlink_crc16(NULL, 0), SATLINK_CRC16_INIT);

    /* Split it and the running form must land in the same place. */
    uint16_t crc = satlink_crc16_update(SATLINK_CRC16_INIT, (const uint8_t *)s, 4);
    crc = satlink_crc16_update(crc, (const uint8_t *)s + 4, 5);
    CHECK_EQ(crc, 0x29B1);
}

/* -------------------------------------------------------------------- COBS */

static void cobs_roundtrip(const uint8_t *data, size_t len)
{
    uint8_t enc[SATLINK_WIRE_MAX];
    uint8_t dec[SATLINK_FRAME_MAX];
    size_t enc_len = 0, dec_len = 0;

    CHECK(satlink_cobs_encode(data, len, enc, sizeof(enc), &enc_len));
    CHECK(enc_len <= SATLINK_COBS_MAX(len));

    /* The whole point: nothing inside may be a delimiter. */
    for (size_t i = 0; i < enc_len; i++) {
        CHECK(enc[i] != 0);
    }

    CHECK(satlink_cobs_decode(enc, enc_len, dec, sizeof(dec), &dec_len));
    CHECK_EQ(dec_len, len);
    CHECK_EQ(memcmp(dec, data, len), 0);
}

static void test_cobs(void)
{
    cobs_roundtrip((const uint8_t *)"", 0);
    cobs_roundtrip((const uint8_t *)"\x00", 1);
    cobs_roundtrip((const uint8_t *)"\x00\x00\x00", 3);
    cobs_roundtrip((const uint8_t *)"hello", 5);
    cobs_roundtrip((const uint8_t *)"a\x00\x62\x00\x63", 5);

    /* Exactly 254 non-zero bytes is where the run length rolls over, and 255 is
     * the byte after. Both have been getting COBS implementations wrong for
     * decades. */
    uint8_t run[600];
    for (size_t i = 0; i < sizeof(run); i++) {
        run[i] = (uint8_t)(1 + (i % 255));
    }
    cobs_roundtrip(run, 253);
    cobs_roundtrip(run, 254);
    cobs_roundtrip(run, 255);
    cobs_roundtrip(run, 256);
    cobs_roundtrip(run, 600);

    /* The overhead the contract states is exactly ceil(n/254) code bytes for a
     * zero-free body. At every multiple of 254 the encoder used to add a
     * trailing 0x01 -- a run opened for input that never came -- and encode
     * one byte longer than the player does for the same body. Both decoders
     * accept both forms; the encoders should nevertheless agree. */
    uint8_t enc_exact[SATLINK_WIRE_MAX];
    uint8_t body[1016];
    for (size_t i = 0; i < sizeof(body); i++) { body[i] = (uint8_t)(1 + (i % 255)); }
    static const size_t boundaries[] = { 1, 253, 254, 255, 508, 509, 762, 1016 };
    for (size_t b = 0; b < sizeof(boundaries) / sizeof(boundaries[0]); b++) {
        size_t n = boundaries[b];
        size_t got = 0;
        CHECK(satlink_cobs_encode(body, n, enc_exact, sizeof(enc_exact), &got));
        CHECK_EQ(got, n + (n + 253) / 254);
    }

    /* The streaming encoder cannot see the end of its input, so a full run
     * that ends exactly on a push boundary must still not open a run for data
     * that never arrives -- and must open one when data does arrive. Each
     * split must produce the same bytes as the one-shot encoder. */
    static const size_t splits[] = { 1, 2, 253, 254, 255, 507, 508, 509 };
    for (size_t s = 0; s < sizeof(splits) / sizeof(splits[0]); s++) {
        for (size_t n = 508; n <= 510; n++) {
            uint8_t one_shot[SATLINK_WIRE_MAX], pieces[SATLINK_WIRE_MAX];
            size_t one_len = 0, piece_len = 0;
            CHECK(satlink_cobs_encode(body, n, one_shot, sizeof(one_shot), &one_len));
            satlink_cobs_enc_t e;
            satlink_cobs_enc_begin(&e, pieces, sizeof(pieces));
            size_t first = (splits[s] < n) ? splits[s] : n;
            satlink_cobs_enc_push(&e, body, first);
            satlink_cobs_enc_push(&e, body + first, n - first);
            CHECK(satlink_cobs_enc_finish(&e, &piece_len));
            CHECK_EQ(piece_len, one_len);
            CHECK_EQ(memcmp(pieces, one_shot, one_len), 0);
        }
    }

    /* A zero right after a full run, pushed on its own: the run that stands
     * for the zero must be opened, closed, and followed by the final run. */
    {
        uint8_t with_zero[256];
        memcpy(with_zero, body, 254);
        with_zero[254] = 0;
        with_zero[255] = 0x7B;
        uint8_t one_shot[SATLINK_WIRE_MAX], pieces[SATLINK_WIRE_MAX];
        size_t one_len = 0, piece_len = 0;
        CHECK(satlink_cobs_encode(with_zero, sizeof(with_zero), one_shot, sizeof(one_shot), &one_len));
        satlink_cobs_enc_t e;
        satlink_cobs_enc_begin(&e, pieces, sizeof(pieces));
        satlink_cobs_enc_push(&e, with_zero, 254);
        satlink_cobs_enc_push(&e, with_zero + 254, 1);
        satlink_cobs_enc_push(&e, with_zero + 255, 1);
        CHECK(satlink_cobs_enc_finish(&e, &piece_len));
        CHECK_EQ(piece_len, one_len);
        CHECK_EQ(memcmp(pieces, one_shot, one_len), 0);
        CHECK_EQ(one_len, 254 + 4);           /* FF, 254 bytes, 01, 02 7B */
        cobs_roundtrip(with_zero, sizeof(with_zero));
    }

    /* The long form a pre-fix peer still sends -- a full run followed by an
     * empty trailing run -- must keep decoding to the same body. */
    {
        uint8_t legacy[SATLINK_WIRE_MAX];
        size_t legacy_len = 0;
        CHECK(satlink_cobs_encode(body, 254, legacy, sizeof(legacy), &legacy_len));
        CHECK_EQ(legacy_len, 255);
        legacy[legacy_len++] = 0x01;
        uint8_t dec[SATLINK_FRAME_MAX];
        size_t dec_len = 0;
        CHECK(satlink_cobs_decode(legacy, legacy_len, dec, sizeof(dec), &dec_len));
        CHECK_EQ(dec_len, 254);
        CHECK_EQ(memcmp(dec, body, 254), 0);
    }

    /* A zero followed by a long run, and a long run followed by a zero. */
    uint8_t mix[300];
    memset(mix, 0x41, sizeof(mix));
    mix[0] = 0;
    cobs_roundtrip(mix, sizeof(mix));
    mix[0] = 0x41;
    mix[sizeof(mix) - 1] = 0;
    cobs_roundtrip(mix, sizeof(mix));

    /* Every byte value, so no code path is special-cased by accident. */
    uint8_t all[256];
    for (int i = 0; i < 256; i++) { all[i] = (uint8_t)i; }
    cobs_roundtrip(all, sizeof(all));

    /* Worst case for the frame size the protocol allows must still fit the buffer
     * the protocol tells both sides to allocate. */
    uint8_t big[SATLINK_FRAME_MAX];
    memset(big, 0x5A, sizeof(big));
    uint8_t enc[SATLINK_WIRE_MAX];
    size_t enc_len = 0;
    CHECK(satlink_cobs_encode(big, sizeof(big), enc, sizeof(enc), &enc_len));
    CHECK(enc_len + 1 <= SATLINK_WIRE_MAX);

    /* Refusals. */
    uint8_t tiny[2];
    size_t out_len = 0;
    CHECK(!satlink_cobs_encode(big, sizeof(big), tiny, sizeof(tiny), &out_len));

    /* A zero inside an encoded frame is not our encoding. */
    uint8_t bad[] = { 0x03, 0x41, 0x00 };
    uint8_t dec[16];
    CHECK(!satlink_cobs_decode(bad, sizeof(bad), dec, sizeof(dec), &out_len));

    /* A code byte that promises more than is there. */
    uint8_t truncated[] = { 0x05, 0x41 };
    CHECK(!satlink_cobs_decode(truncated, sizeof(truncated), dec, sizeof(dec), &out_len));
}

/* ------------------------------------------------------------------- frames */

typedef struct {
    int      calls;
    uint8_t  type;
    uint8_t  seq;
    uint8_t  payload[SATLINK_PAYLOAD_MAX];
    size_t   payload_len;
} capture_t;

static void on_frame(void *ctx, uint8_t type, uint8_t seq,
                     const uint8_t *payload, size_t len)
{
    capture_t *c = (capture_t *)ctx;
    c->calls++;
    c->type = type;
    c->seq = seq;
    c->payload_len = len;
    if (len != 0) { memcpy(c->payload, payload, len); }
}

static void test_frame_roundtrip(void)
{
    const uint8_t body[] = { 1, 0, 2, 0, 0, 3, 255 };
    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;

    CHECK(satlink_frame_encode(SATLINK_MSG_BT_STATE, 42, body, sizeof(body),
                               wire, sizeof(wire), &wire_len));
    CHECK_EQ(wire[wire_len - 1], SATLINK_DELIMITER);

    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);
    satlink_rx_feed(&rx, wire, wire_len, on_frame, &cap);

    CHECK_EQ(cap.calls, 1);
    CHECK_EQ(cap.type, SATLINK_MSG_BT_STATE);
    CHECK_EQ(cap.seq, 42);
    CHECK_EQ(cap.payload_len, sizeof(body));
    CHECK_EQ(memcmp(cap.payload, body, sizeof(body)), 0);
    CHECK_EQ(rx.frames_ok, 1);
}

static void test_frame_max_payload(void)
{
    uint8_t body[SATLINK_PAYLOAD_MAX];
    for (size_t i = 0; i < sizeof(body); i++) { body[i] = (uint8_t)(i * 7); }

    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;
    CHECK(satlink_frame_encode(SATLINK_MSG_COVER, 7, body, sizeof(body),
                               wire, sizeof(wire), &wire_len));
    CHECK(wire_len <= SATLINK_WIRE_MAX);

    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);
    satlink_rx_feed(&rx, wire, wire_len, on_frame, &cap);
    CHECK_EQ(cap.calls, 1);
    CHECK_EQ(cap.payload_len, sizeof(body));
    CHECK_EQ(memcmp(cap.payload, body, sizeof(body)), 0);

    /* One byte past the limit is refused rather than silently truncated. */
    uint8_t too_big[SATLINK_PAYLOAD_MAX + 1];
    memset(too_big, 0x11, sizeof(too_big));
    CHECK(!satlink_frame_encode(SATLINK_MSG_COVER, 8, too_big, sizeof(too_big),
                                wire, sizeof(wire), &wire_len));
}

static void test_frame_bad_crc(void)
{
    const uint8_t body[] = { 9, 9, 9, 9 };
    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;
    CHECK(satlink_frame_encode(SATLINK_MSG_PING, 1, body, sizeof(body),
                               wire, sizeof(wire), &wire_len));

    /* Flip the top bit of a payload byte. It stays non-zero, so the framing still
     * holds and the CRC is the only thing that can catch it. */
    uint8_t corrupted[SATLINK_WIRE_MAX];
    memcpy(corrupted, wire, wire_len);
    corrupted[4] ^= 0x80;

    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);
    satlink_rx_feed(&rx, corrupted, wire_len, on_frame, &cap);

    CHECK_EQ(cap.calls, 0);
    CHECK_EQ(rx.frames_ok, 0);
    CHECK_EQ(rx.err_crc, 1);

    /* A flip that turns a byte into a zero is a different story: the reassembler
     * takes it for a delimiter and the frame arrives as two malformed pieces. Two
     * errors, no callback, and — the part that matters — still in step afterwards. */
    memcpy(corrupted, wire, wire_len);
    corrupted[2] = 0x00;

    capture_t split_cap = {0};
    satlink_rx_t split_rx;
    satlink_rx_init(&split_rx);
    satlink_rx_feed(&split_rx, corrupted, wire_len, on_frame, &split_cap);

    CHECK_EQ(split_cap.calls, 0);
    CHECK_EQ(split_rx.frames_ok, 0);
    CHECK(split_rx.err_cobs + split_rx.err_crc + split_rx.err_runt >= 1);

    satlink_rx_feed(&split_rx, wire, wire_len, on_frame, &split_cap);
    CHECK_EQ(split_cap.calls, 1);
}

static void test_rx_resync(void)
{
    /* A receiver powered on in the middle of a transmission: garbage, then a
     * delimiter, then a good frame. It must find its feet at the delimiter. */
    const uint8_t body[] = { 0xDE, 0xAD };
    uint8_t good[SATLINK_WIRE_MAX];
    size_t good_len = 0;
    CHECK(satlink_frame_encode(SATLINK_MSG_LOG, 200, body, sizeof(body),
                               good, sizeof(good), &good_len));

    uint8_t stream[SATLINK_WIRE_MAX + 64];
    size_t n = 0;
    const uint8_t junk[] = { 0x41, 0x7F, 0x03, 0xFE, 0x02 };   /* half a frame */
    memcpy(stream + n, junk, sizeof(junk)); n += sizeof(junk);
    stream[n++] = SATLINK_DELIMITER;
    memcpy(stream + n, good, good_len); n += good_len;

    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);
    satlink_rx_feed(&rx, stream, n, on_frame, &cap);

    CHECK_EQ(cap.calls, 1);
    CHECK_EQ(cap.type, SATLINK_MSG_LOG);
    CHECK_EQ(cap.seq, 200);
}

static void test_rx_byte_at_a_time(void)
{
    /* The UART hands over whatever it has. Feeding one byte per call must give
     * the same answer as feeding the lot. */
    const uint8_t body[] = { 1, 2, 3, 0, 5 };
    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;
    CHECK(satlink_frame_encode(SATLINK_MSG_TRACK_POS, 3, body, sizeof(body),
                               wire, sizeof(wire), &wire_len));

    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);
    for (size_t i = 0; i < wire_len; i++) {
        satlink_rx_feed(&rx, wire + i, 1, on_frame, &cap);
    }
    CHECK_EQ(cap.calls, 1);
    CHECK_EQ(cap.payload_len, sizeof(body));
    CHECK_EQ(memcmp(cap.payload, body, sizeof(body)), 0);
}

static void test_rx_shared_delimiter(void)
{
    /* Back-to-back frames share one delimiter, and a run of delimiters on an idle
     * line produces empty frames that must be ignored, not counted as errors. */
    uint8_t a[SATLINK_WIRE_MAX], b[SATLINK_WIRE_MAX];
    size_t a_len = 0, b_len = 0;
    const uint8_t p1[] = { 1 };
    const uint8_t p2[] = { 2 };
    CHECK(satlink_frame_encode(SATLINK_MSG_PING, 10, p1, 1, a, sizeof(a), &a_len));
    CHECK(satlink_frame_encode(SATLINK_MSG_PONG, 11, p2, 1, b, sizeof(b), &b_len));

    uint8_t stream[64];
    size_t n = 0;
    stream[n++] = SATLINK_DELIMITER;        /* leading one: optional, harmless */
    stream[n++] = SATLINK_DELIMITER;
    memcpy(stream + n, a, a_len); n += a_len;
    memcpy(stream + n, b, b_len); n += b_len;

    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);
    satlink_rx_feed(&rx, stream, n, on_frame, &cap);

    CHECK_EQ(cap.calls, 2);
    CHECK_EQ(cap.type, SATLINK_MSG_PONG);
    CHECK_EQ(rx.frames_ok, 2);
    CHECK_EQ(rx.err_cobs + rx.err_crc + rx.err_runt + rx.err_too_long, 0);
}

static void test_rx_oversize_keeps_sync(void)
{
    /* A frame longer than the buffer is dropped, and the very next one still
     * arrives. This is the property the whole framing choice exists for. */
    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);

    uint8_t flood[SATLINK_WIRE_MAX * 2];
    memset(flood, 0x42, sizeof(flood));
    satlink_rx_feed(&rx, flood, sizeof(flood), on_frame, &cap);
    uint8_t delim = SATLINK_DELIMITER;
    satlink_rx_feed(&rx, &delim, 1, on_frame, &cap);

    CHECK_EQ(cap.calls, 0);
    CHECK_EQ(rx.err_too_long, 1);

    const uint8_t body[] = { 0x55 };
    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;
    CHECK(satlink_frame_encode(SATLINK_MSG_HELLO, 1, body, 1, wire, sizeof(wire), &wire_len));
    satlink_rx_feed(&rx, wire, wire_len, on_frame, &cap);

    CHECK_EQ(cap.calls, 1);
    CHECK_EQ(rx.frames_ok, 1);
}

static void test_rx_seq_gaps(void)
{
    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);

    const uint8_t seqs[] = { 250, 251, 254, 255, 0, 1 };   /* 252, 253 lost; wraps */
    for (size_t i = 0; i < sizeof(seqs); i++) {
        uint8_t wire[SATLINK_WIRE_MAX];
        size_t wire_len = 0;
        const uint8_t body[] = { 0 };
        CHECK(satlink_frame_encode(SATLINK_MSG_PING, seqs[i], body, 1,
                                   wire, sizeof(wire), &wire_len));
        satlink_rx_feed(&rx, wire, wire_len, on_frame, &cap);
    }

    CHECK_EQ(cap.calls, 6);
    CHECK_EQ(rx.frames_ok, 6);
    CHECK_EQ(rx.seq_gaps, 2);       /* and the wrap at 255 -> 0 is not a gap */
}

static void test_rx_discard_keeps_counters(void)
{
    /* What the link does after the UART driver threw bytes away: the half frame
     * goes, the totals stay. Resetting them made the error count run backwards. */
    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);

    const uint8_t garbage[] = { 0x41, 0x42, SATLINK_DELIMITER };
    satlink_rx_feed(&rx, garbage, sizeof(garbage), on_frame, &cap);
    const uint32_t errors = rx.err_cobs + rx.err_crc + rx.err_runt + rx.err_too_long;
    CHECK_EQ(errors, 1);

    const uint8_t body[] = { 7 };
    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;
    CHECK(satlink_frame_encode(SATLINK_MSG_PING, 10, body, 1, wire, sizeof(wire), &wire_len));
    satlink_rx_feed(&rx, wire, wire_len, on_frame, &cap);
    CHECK(satlink_frame_encode(SATLINK_MSG_PING, 12, body, 1, wire, sizeof(wire), &wire_len));
    satlink_rx_feed(&rx, wire, wire_len, on_frame, &cap);
    CHECK_EQ(rx.seq_gaps, 1);

    /* Half of the next frame arrives, then the overrun. */
    CHECK(satlink_frame_encode(SATLINK_MSG_PING, 20, body, 1, wire, sizeof(wire), &wire_len));
    satlink_rx_feed(&rx, wire, wire_len / 2, on_frame, &cap);
    satlink_rx_discard(&rx);

    CHECK_EQ(rx.len, 0);
    CHECK_EQ(rx.frames_ok, 2);
    CHECK_EQ(rx.err_cobs + rx.err_crc + rx.err_runt + rx.err_too_long, errors);
    CHECK_EQ(rx.seq_gaps, 1);

    /* The frames lost to the overrun are not counted as gaps on the wire. */
    CHECK(satlink_frame_encode(SATLINK_MSG_PING, 30, body, 1, wire, sizeof(wire), &wire_len));
    uint8_t delim = SATLINK_DELIMITER;
    satlink_rx_feed(&rx, &delim, 1, on_frame, &cap);
    satlink_rx_feed(&rx, wire, wire_len, on_frame, &cap);
    CHECK_EQ(cap.calls, 3);
    CHECK_EQ(cap.seq, 30);
    CHECK_EQ(rx.seq_gaps, 1);
}

/* -------------------------------------------------------------------- codec */

static void test_codec(void)
{
    uint8_t buf[64];
    satlink_wr_t w;
    satlink_wr_init(&w, buf, sizeof(buf));

    satlink_wr_u8(&w, 0xA5);
    satlink_wr_u16(&w, 0x1234);
    satlink_wr_u32(&w, 0xDEADBEEF);
    satlink_wr_str(&w, "Salomé");         /* UTF-8, two bytes for the é */
    satlink_wr_str(&w, NULL);             /* unknown is an empty string */
    CHECK(w.ok);

    /* Little-endian on the wire, so the low byte comes first. */
    CHECK_EQ(buf[1], 0x34);
    CHECK_EQ(buf[2], 0x12);
    CHECK_EQ(buf[3], 0xEF);
    CHECK_EQ(buf[6], 0xDE);

    satlink_rd_t r;
    satlink_rd_init(&r, buf, w.len);
    CHECK_EQ(satlink_rd_u8(&r), 0xA5);
    CHECK_EQ(satlink_rd_u16(&r), 0x1234);
    CHECK_EQ(satlink_rd_u32(&r), 0xDEADBEEF);

    const char *s = NULL;
    uint16_t s_len = 0;
    CHECK(satlink_rd_str(&r, &s, &s_len));
    CHECK_EQ(s_len, 7);                   /* six characters, seven bytes */
    CHECK_EQ(memcmp(s, "Salomé", 7), 0);

    CHECK(satlink_rd_str(&r, &s, &s_len));
    CHECK_EQ(s_len, 0);

    CHECK(satlink_rd_done(&r));

    /* Reading past the end fails rather than inventing data. */
    CHECK_EQ(satlink_rd_u8(&r), 0);
    CHECK(!r.ok);

    /* Writing past the end is sticky: the flag stays false. */
    uint8_t small[3];
    satlink_wr_init(&w, small, sizeof(small));
    satlink_wr_u32(&w, 1);
    CHECK(!w.ok);
    satlink_wr_u8(&w, 1);
    CHECK(!w.ok);
    CHECK_EQ(w.len, 0);
}

static void test_hello_roundtrip(void)
{
    /* One real message end to end, the way the link will actually use it. */
    uint8_t payload[SATLINK_PAYLOAD_MAX];
    satlink_wr_t w;
    satlink_wr_init(&w, payload, sizeof(payload));
    satlink_wr_u8(&w, SATLINK_PROTO_MAJOR);
    satlink_wr_u8(&w, SATLINK_PROTO_MINOR);
    satlink_wr_u32(&w, SATLINK_CAP_A2DP_SINK | SATLINK_CAP_AVRCP_META | SATLINK_CAP_COVER_ART);
    satlink_wr_str(&w, "sat 0.1.0 a1b2c3d");
    CHECK(w.ok);

    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;
    CHECK(satlink_frame_encode(SATLINK_MSG_HELLO, 0, payload, w.len,
                               wire, sizeof(wire), &wire_len));

    capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);
    satlink_rx_feed(&rx, wire, wire_len, on_frame, &cap);
    CHECK_EQ(cap.calls, 1);

    satlink_rd_t r;
    satlink_rd_init(&r, cap.payload, cap.payload_len);
    CHECK_EQ(satlink_rd_u8(&r), SATLINK_PROTO_MAJOR);
    CHECK_EQ(satlink_rd_u8(&r), SATLINK_PROTO_MINOR);
    CHECK_EQ(satlink_rd_u32(&r),
             SATLINK_CAP_A2DP_SINK | SATLINK_CAP_AVRCP_META | SATLINK_CAP_COVER_ART);
    const char *build = NULL;
    uint16_t build_len = 0;
    CHECK(satlink_rd_str(&r, &build, &build_len));
    CHECK_EQ(build_len, 17);
    CHECK(satlink_rd_done(&r));
}

/* ------------------------------------------------------------------- exports */

const test_case_t frame_tests[] = {
    { "crc16",                  test_crc16 },
    { "cobs",                   test_cobs },
    { "frame roundtrip",        test_frame_roundtrip },
    { "frame max payload",      test_frame_max_payload },
    { "frame bad crc",          test_frame_bad_crc },
    { "rx resync",              test_rx_resync },
    { "rx byte at a time",      test_rx_byte_at_a_time },
    { "rx shared delimiter",    test_rx_shared_delimiter },
    { "rx oversize keeps sync", test_rx_oversize_keeps_sync },
    { "rx seq gaps",            test_rx_seq_gaps },
    { "rx discard keeps counters", test_rx_discard_keeps_counters },
    { "codec",                  test_codec },
    { "hello roundtrip",        test_hello_roundtrip },
};

const size_t frame_test_count = sizeof(frame_tests) / sizeof(frame_tests[0]);
