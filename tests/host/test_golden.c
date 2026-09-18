/*
 * Golden vectors from the host implementation, cross-checked there by a second
 * independent one.
 *
 * These are the only test in this suite that can fail because of somebody else's
 * code, and that is exactly their value: everything else here proves this
 * implementation agrees with itself. Each vector is checked twice — decoded into
 * its fields, and encoded back from those fields into the same bytes. A decoder
 * that is merely permissive passes the first and fails the second.
 *
 * HELLO and TRACK_META are in the set deliberately. The first carries three zero
 * bytes in a row inside caps, the second an empty string; both are where COBS
 * does its work and where a naive implementation quietly breaks.
 */

#include <string.h>

#include "harness.h"

#include "satlink/frame.h"
#include "satlink/messages.h"
#include "satlink/proto.h"

typedef struct {
    int            calls;
    uint8_t        type;
    uint8_t        seq;
    uint8_t        payload[SATLINK_PAYLOAD_MAX];
    size_t         payload_len;
} golden_capture_t;

static void on_golden(void *ctx, uint8_t type, uint8_t seq,
                      const uint8_t *payload, size_t len)
{
    golden_capture_t *c = (golden_capture_t *)ctx;
    c->calls++;
    c->type = type;
    c->seq = seq;
    c->payload_len = len;
    if (len != 0) { memcpy(c->payload, payload, len); }
}

/* Feeds one vector through the byte-stream receiver, the way it would arrive. */
static bool decode_vector(const uint8_t *wire, size_t len, golden_capture_t *cap)
{
    satlink_rx_t rx;
    satlink_rx_init(&rx);
    memset(cap, 0, sizeof(*cap));
    satlink_rx_feed(&rx, wire, len, on_golden, cap);
    CHECK_EQ(rx.err_cobs + rx.err_crc + rx.err_runt + rx.err_too_long, 0);
    CHECK_EQ(cap->calls, 1);
    return cap->calls == 1;
}

static void check_encodes_to(uint8_t type, uint8_t seq,
                             const uint8_t *payload, size_t payload_len,
                             const uint8_t *expect, size_t expect_len)
{
    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;
    CHECK(satlink_frame_encode(type, seq, payload, payload_len,
                               wire, sizeof(wire), &wire_len));
    CHECK_EQ(wire_len, expect_len);
    if (wire_len == expect_len) {
        int diff = memcmp(wire, expect, expect_len);
        CHECK_EQ(diff, 0);
        if (diff != 0) {
            printf("    ours  :");
            for (size_t i = 0; i < wire_len; i++) { printf(" %02X", wire[i]); }
            printf("\n    theirs:");
            for (size_t i = 0; i < expect_len; i++) { printf(" %02X", expect[i]); }
            printf("\n");
        }
    }
}

/* --------------------------------------------------------------------- HELLO */

static const uint8_t g_hello[] = {
    0x00, 0x02, 0x01, 0x02, 0x01, 0x02, 0x01, 0x01, 0x01, 0x02, 0x0C, 0x0F, 0x70, 0x6C, 0x61, 0x79,
    0x65, 0x72, 0x20, 0x30, 0x2E, 0x34, 0x2E, 0x30, 0x0A, 0x72, 0x00,
};

static void test_golden_hello(void)
{
    golden_capture_t cap;
    if (!decode_vector(g_hello, sizeof(g_hello), &cap)) { return; }

    CHECK_EQ(cap.type, SATLINK_MSG_HELLO);
    CHECK_EQ(cap.seq, 0);

    satlink_hello_t m = {0};
    CHECK(satlink_dec_hello(cap.payload, cap.payload_len, &m));
    CHECK_EQ(m.major, 1);
    CHECK_EQ(m.minor, 0);
    CHECK_EQ(m.caps, 0x00000001u);          /* only "can relock I2S on the fly" */
    CHECK_EQ(m.build.len, 12);
    CHECK_EQ(memcmp(m.build.ptr, "player 0.4.0", 12), 0);

    /* The host declares neither cover art nor the expander. */
    CHECK(!(m.caps & SATLINK_HOST_CAP_COVER));
    CHECK(!(m.caps & SATLINK_HOST_CAP_EXPANDER));
    CHECK(m.caps & SATLINK_HOST_CAP_RELOCK);

    uint8_t payload[SATLINK_PAYLOAD_MAX];
    size_t len = satlink_enc_hello(payload, sizeof(payload), &m);
    check_encodes_to(SATLINK_MSG_HELLO, 0, payload, len, g_hello, sizeof(g_hello));
}

/* ------------------------------------------------------------ CMD_TRANSPORT */

static const uint8_t g_transport[] = { 0x00, 0x06, 0x20, 0x01, 0x03, 0x08, 0x49, 0x00 };

static void test_golden_transport(void)
{
    golden_capture_t cap;
    if (!decode_vector(g_transport, sizeof(g_transport), &cap)) { return; }

    CHECK_EQ(cap.type, SATLINK_MSG_CMD_TRANSPORT);
    CHECK_EQ(cap.seq, 1);

    satlink_cmd_transport_t m = {0};
    CHECK(satlink_dec_cmd_transport(cap.payload, cap.payload_len, &m));
    CHECK_EQ(m.cmd, SATLINK_XPORT_NEXT);

    uint8_t payload[8];
    size_t len = satlink_enc_cmd_transport(payload, sizeof(payload), &m);
    check_encodes_to(SATLINK_MSG_CMD_TRANSPORT, 1, payload, len,
                     g_transport, sizeof(g_transport));
}

/* --------------------------------------------------------------- CMD_VOLUME */

static const uint8_t g_volume[] = { 0x00, 0x06, 0x21, 0x02, 0x40, 0xCC, 0x53, 0x00 };

static void test_golden_volume(void)
{
    golden_capture_t cap;
    if (!decode_vector(g_volume, sizeof(g_volume), &cap)) { return; }

    CHECK_EQ(cap.type, SATLINK_MSG_CMD_VOLUME);
    CHECK_EQ(cap.seq, 2);

    satlink_cmd_volume_t m = {0};
    CHECK(satlink_dec_cmd_volume(cap.payload, cap.payload_len, &m));
    CHECK_EQ(m.volume, 64);

    uint8_t payload[8];
    size_t len = satlink_enc_cmd_volume(payload, sizeof(payload), &m);
    check_encodes_to(SATLINK_MSG_CMD_VOLUME, 2, payload, len, g_volume, sizeof(g_volume));
}

/* --------------------------------------------------------------- TRACK_META */

static const uint8_t g_track_meta[] = {
    0x00, 0x02, 0x11, 0x02, 0x0A, 0x0C, 0xD0, 0x92, 0xD0, 0xB5, 0xD1, 0x81, 0xD0, 0xBD, 0xD0, 0xB0,
    0x15, 0x16, 0xD0, 0x9E, 0xD0, 0xBA, 0xD0, 0xB5, 0xD0, 0xB0, 0xD0, 0xBD, 0x20, 0xD0, 0x95, 0xD0,
    0xBB, 0xD1, 0x8C, 0xD0, 0xB7, 0xD0, 0xB8, 0x01, 0x03, 0x62, 0x9F, 0x00,
};

static void test_golden_track_meta(void)
{
    golden_capture_t cap;
    if (!decode_vector(g_track_meta, sizeof(g_track_meta), &cap)) { return; }

    CHECK_EQ(cap.type, SATLINK_MSG_TRACK_META);
    CHECK_EQ(cap.seq, 0);

    satlink_track_meta_t m = {0};
    CHECK(satlink_dec_track_meta(cap.payload, cap.payload_len, &m));

    /* Ten bytes for five Cyrillic characters, twenty-one for eleven: the length
     * on the wire is bytes, never characters. */
    CHECK_EQ(m.title.len, 10);
    CHECK_EQ(memcmp(m.title.ptr, "Весна", 10), 0);
    CHECK_EQ(m.artist.len, 21);
    CHECK_EQ(memcmp(m.artist.ptr, "Океан Ельзи", 21), 0);
    CHECK_EQ(m.album.len, 0);

    uint8_t payload[SATLINK_PAYLOAD_MAX];
    size_t len = satlink_enc_track_meta(payload, sizeof(payload), &m);
    check_encodes_to(SATLINK_MSG_TRACK_META, 0, payload, len,
                     g_track_meta, sizeof(g_track_meta));
}

/* --------------------------------------------------------------- STREAM_FMT */

static const uint8_t g_stream_fmt[] = {
    0x00, 0x02, 0x13, 0x03, 0x44, 0xAC, 0x01, 0x05, 0x20, 0x02, 0xA5, 0x2F, 0x00,
};

static void test_golden_stream_fmt(void)
{
    golden_capture_t cap;
    if (!decode_vector(g_stream_fmt, sizeof(g_stream_fmt), &cap)) { return; }

    CHECK_EQ(cap.type, SATLINK_MSG_STREAM_FMT);
    CHECK_EQ(cap.seq, 0);

    satlink_stream_fmt_t m = {0};
    CHECK(satlink_dec_stream_fmt(cap.payload, cap.payload_len, &m));
    CHECK_EQ(m.rate_hz, 44100);
    CHECK_EQ(m.bits, 32);           /* note: the host's example uses 32-bit slots */
    CHECK_EQ(m.channels, 2);

    uint8_t payload[16];
    size_t len = satlink_enc_stream_fmt(payload, sizeof(payload), &m);
    check_encodes_to(SATLINK_MSG_STREAM_FMT, 0, payload, len,
                     g_stream_fmt, sizeof(g_stream_fmt));
}

/* --------------------------------------------------------------- back to back */

static void test_golden_stream_of_frames(void)
{
    /* All five arriving as one run of bytes, the way they would off a UART. Each
     * frame here carries both delimiters, so between any two there is an empty
     * run — which must be ignored rather than counted as a malformed frame. */
    uint8_t stream[256];
    size_t n = 0;
    const uint8_t *vectors[] = { g_hello, g_transport, g_volume, g_stream_fmt };
    const size_t   sizes[]   = { sizeof(g_hello), sizeof(g_transport),
                                 sizeof(g_volume), sizeof(g_stream_fmt) };

    for (size_t i = 0; i < 4; i++) {
        memcpy(stream + n, vectors[i], sizes[i]);
        n += sizes[i];
    }

    golden_capture_t cap = {0};
    satlink_rx_t rx;
    satlink_rx_init(&rx);
    satlink_rx_feed(&rx, stream, n, on_golden, &cap);

    CHECK_EQ(cap.calls, 4);
    CHECK_EQ(rx.frames_ok, 4);
    CHECK_EQ(rx.err_cobs + rx.err_crc + rx.err_runt + rx.err_too_long, 0);

    /* Sequence numbers here are 0, 1, 2, 0 — the last is a different direction in
     * real life, but as a stream it exercises the wrap arithmetic: 2 to 0 is a
     * jump of 254, not a step back. */
    CHECK(rx.seq_gaps > 0);

    /* One byte at a time must give the same answer. */
    golden_capture_t slow = {0};
    satlink_rx_t slow_rx;
    satlink_rx_init(&slow_rx);
    for (size_t i = 0; i < n; i++) {
        satlink_rx_feed(&slow_rx, stream + i, 1, on_golden, &slow);
    }
    CHECK_EQ(slow.calls, 4);
    CHECK_EQ(slow_rx.err_cobs + slow_rx.err_crc + slow_rx.err_runt + slow_rx.err_too_long, 0);
}

const test_case_t golden_tests[] = {
    { "hello",           test_golden_hello },
    { "cmd_transport",   test_golden_transport },
    { "cmd_volume",      test_golden_volume },
    { "track_meta",      test_golden_track_meta },
    { "stream_fmt",      test_golden_stream_fmt },
    { "back to back",    test_golden_stream_of_frames },
};

const size_t golden_test_count = sizeof(golden_tests) / sizeof(golden_tests[0]);
