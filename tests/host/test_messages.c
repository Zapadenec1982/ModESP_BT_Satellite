/*
 * Every message in the protocol, encoded and decoded again.
 *
 * Each case does three things: a round trip, a check that the bytes on the wire
 * are the length the specification says, and a check that a truncated or padded
 * payload is rejected rather than half-read. The second matters because the other
 * end is a separate implementation working from the same document, and a layout
 * that only agrees with itself is worse than one that fails loudly.
 */

#include <string.h>

#include "harness.h"

#include "satlink/frame.h"
#include "satlink/messages.h"
#include "satlink/proto.h"

static uint8_t g_buf[SATLINK_PAYLOAD_MAX];

/* A decoder must reject a payload one byte short and one byte long. Feeding the
 * generic case through here keeps every test honest without repeating it. */
#define CHECK_STRICT_LEN(dec_fn, out, len)                                      \
    do {                                                                        \
        if ((len) > 0) {                                                        \
            CHECK(!dec_fn(g_buf, (len) - 1, &(out)));                           \
        }                                                                       \
        uint8_t padded[SATLINK_PAYLOAD_MAX];                                    \
        memcpy(padded, g_buf, (len));                                           \
        padded[(len)] = 0x00;                                                   \
        CHECK(!dec_fn(padded, (len) + 1, &(out)));                              \
    } while (0)

static void test_msg_hello(void)
{
    satlink_hello_t in = {
        .major = SATLINK_PROTO_MAJOR,
        .minor = SATLINK_PROTO_MINOR,
        .caps  = SATLINK_CAP_A2DP_SINK | SATLINK_CAP_AVRCP_META |
                 SATLINK_CAP_ABS_VOLUME | SATLINK_CAP_COVER_ART | SATLINK_CAP_EXPANDER,
        .build = satlink_str("sat 0.1.0 deadbee"),
    };

    size_t len = satlink_enc_hello(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 1 + 1 + 4 + 2 + 17);

    satlink_hello_t out = {0};
    CHECK(satlink_dec_hello(g_buf, len, &out));
    CHECK_EQ(out.major, in.major);
    CHECK_EQ(out.minor, in.minor);
    CHECK_EQ(out.caps, in.caps);
    CHECK_EQ(out.build.len, in.build.len);
    CHECK_EQ(memcmp(out.build.ptr, in.build.ptr, out.build.len), 0);

    CHECK_STRICT_LEN(satlink_dec_hello, out, len);

    /* An empty build string is legal and must not be confused with a failure. */
    satlink_hello_t bare = { .major = 1, .minor = 0, .caps = 0, .build = satlink_str(NULL) };
    len = satlink_enc_hello(g_buf, sizeof(g_buf), &bare);
    CHECK_EQ(len, 8);
    CHECK(satlink_dec_hello(g_buf, len, &out));
    CHECK_EQ(out.build.len, 0);
}

static void test_msg_ping_pong(void)
{
    satlink_ping_t ping = { .tick_ms = 0x01020304 };
    size_t len = satlink_enc_ping(g_buf, sizeof(g_buf), &ping);
    CHECK_EQ(len, 4);
    CHECK_EQ(g_buf[0], 0x04);   /* little-endian */
    CHECK_EQ(g_buf[3], 0x01);

    satlink_ping_t ping_out = {0};
    CHECK(satlink_dec_ping(g_buf, len, &ping_out));
    CHECK_EQ(ping_out.tick_ms, ping.tick_ms);
    CHECK_STRICT_LEN(satlink_dec_ping, ping_out, len);

    satlink_pong_t pong = { .echo_ms = 0xDEADBEEF, .own_ms = 0x11223344 };
    len = satlink_enc_pong(g_buf, sizeof(g_buf), &pong);
    CHECK_EQ(len, 8);

    satlink_pong_t pong_out = {0};
    CHECK(satlink_dec_pong(g_buf, len, &pong_out));
    CHECK_EQ(pong_out.echo_ms, pong.echo_ms);
    CHECK_EQ(pong_out.own_ms, pong.own_ms);
    CHECK_STRICT_LEN(satlink_dec_pong, pong_out, len);
}

static void test_msg_bt_state(void)
{
    satlink_bt_state_t_msg in = {
        .state = SATLINK_BT_STREAMING,
        .codec = SATLINK_CODEC_SBC,
        .addr  = { 0xDC, 0xE5, 0x5B, 0xEA, 0xC9, 0x3B },
        .name  = satlink_str("Pixel"),
    };

    size_t len = satlink_enc_bt_state(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 1 + 1 + 6 + 2 + 5);

    satlink_bt_state_t_msg out = {0};
    CHECK(satlink_dec_bt_state(g_buf, len, &out));
    CHECK_EQ(out.state, SATLINK_BT_STREAMING);
    CHECK_EQ(out.codec, SATLINK_CODEC_SBC);
    CHECK_EQ(memcmp(out.addr, in.addr, 6), 0);
    CHECK_EQ(out.name.len, 5);
    CHECK_EQ(memcmp(out.name.ptr, "Pixel", 5), 0);
    CHECK_STRICT_LEN(satlink_dec_bt_state, out, len);

    /* Disconnected: zero address, no name. */
    satlink_bt_state_t_msg idle = { .state = SATLINK_BT_IDLE, .codec = SATLINK_CODEC_NONE,
                                    .name = satlink_str(NULL) };
    len = satlink_enc_bt_state(g_buf, sizeof(g_buf), &idle);
    CHECK(satlink_dec_bt_state(g_buf, len, &out));
    CHECK_EQ(out.name.len, 0);
    for (int i = 0; i < 6; i++) { CHECK_EQ(out.addr[i], 0); }
}

static void test_msg_track_meta(void)
{
    /* Real metadata off the bench, accents and all: the protocol says UTF-8 and
     * the codec must not care what is in the bytes. */
    satlink_track_meta_t in = {
        .title  = satlink_str("Life Is Simple (Move Your Body) [Feat Salomé Das]"),
        .artist = satlink_str("Maesic, Marshall Jefferson, Salomé Das"),
        .album  = satlink_str(""),
    };

    size_t len = satlink_enc_track_meta(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 2 + in.title.len + 2 + in.artist.len + 2 + 0);

    satlink_track_meta_t out = {0};
    CHECK(satlink_dec_track_meta(g_buf, len, &out));
    CHECK_EQ(out.title.len, in.title.len);
    CHECK_EQ(memcmp(out.title.ptr, in.title.ptr, out.title.len), 0);
    CHECK_EQ(out.artist.len, in.artist.len);
    CHECK_EQ(out.album.len, 0);
    CHECK_STRICT_LEN(satlink_dec_track_meta, out, len);

    /* The first one after connecting is all empty, and that is normal. */
    satlink_track_meta_t empty = { satlink_str(NULL), satlink_str(NULL), satlink_str(NULL) };
    len = satlink_enc_track_meta(g_buf, sizeof(g_buf), &empty);
    CHECK_EQ(len, 6);
    CHECK(satlink_dec_track_meta(g_buf, len, &out));
    CHECK_EQ(out.title.len, 0);
}

static void test_msg_track_pos(void)
{
    satlink_track_pos_t in = {
        .state = SATLINK_PLAY_PLAYING,
        .elapsed_ms = 54000,
        .total_ms = 286000,
    };

    size_t len = satlink_enc_track_pos(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 9);

    satlink_track_pos_t out = {0};
    CHECK(satlink_dec_track_pos(g_buf, len, &out));
    CHECK_EQ(out.state, in.state);
    CHECK_EQ(out.elapsed_ms, in.elapsed_ms);
    CHECK_EQ(out.total_ms, in.total_ms);
    CHECK_STRICT_LEN(satlink_dec_track_pos, out, len);
}

static void test_sanitize_total_ms(void)
{
    /* The phone on the bench really did answer -1000. */
    CHECK_EQ(satlink_sanitize_total_ms(-1000), 0);
    CHECK_EQ(satlink_sanitize_total_ms(0), 0);
    CHECK_EQ(satlink_sanitize_total_ms(286000), 286000);
    CHECK_EQ(satlink_sanitize_total_ms(24LL * 3600 * 1000), 86400000);
    CHECK_EQ(satlink_sanitize_total_ms(24LL * 3600 * 1000 + 1), 0);
    CHECK_EQ(satlink_sanitize_total_ms(0x7FFFFFFFFFFFLL), 0);
}

static void test_msg_stream_fmt(void)
{
    satlink_stream_fmt_t in = { .rate_hz = 44100, .bits = 16, .channels = 2 };
    size_t len = satlink_enc_stream_fmt(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 6);

    satlink_stream_fmt_t out = {0};
    CHECK(satlink_dec_stream_fmt(g_buf, len, &out));
    CHECK_EQ(out.rate_hz, 44100);
    CHECK_EQ(out.bits, 16);
    CHECK_EQ(out.channels, 2);
    CHECK_STRICT_LEN(satlink_dec_stream_fmt, out, len);
}

static void test_msg_cover(void)
{
    uint8_t art[SATLINK_COVER_CHUNK_MAX];
    for (size_t i = 0; i < sizeof(art); i++) { art[i] = (uint8_t)(i ^ 0x5A); }

    satlink_cover_t in = {
        .chunk = 3, .total = 64, .mime = SATLINK_MIME_JPEG,
        .total_len = 64 * 1024, .len = sizeof(art), .data = art,
    };

    size_t len = satlink_enc_cover(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 2 + 2 + 1 + 4 + 2 + sizeof(art));
    CHECK(len <= SATLINK_PAYLOAD_MAX);

    satlink_cover_t out = {0};
    CHECK(satlink_dec_cover(g_buf, len, &out));
    CHECK_EQ(out.chunk, 3);
    CHECK_EQ(out.total, 64);
    CHECK_EQ(out.mime, SATLINK_MIME_JPEG);
    CHECK_EQ(out.total_len, 64u * 1024u);
    CHECK_EQ(out.len, sizeof(art));
    CHECK_EQ(memcmp(out.data, art, sizeof(art)), 0);

    /* A last chunk is usually short. */
    satlink_cover_t tail = { .chunk = 63, .total = 64, .mime = SATLINK_MIME_PNG,
                             .total_len = 64 * 1024, .len = 7, .data = art };
    len = satlink_enc_cover(g_buf, sizeof(g_buf), &tail);
    CHECK_EQ(len, 11 + 7);
    CHECK(satlink_dec_cover(g_buf, len, &out));
    CHECK_EQ(out.len, 7);

    /* A length field that disagrees with the payload is a disagreement about the
     * layout, not something to read past. */
    g_buf[9] = 0xFF;
    g_buf[10] = 0x00;
    CHECK(!satlink_dec_cover(g_buf, len, &out));

    /* Over a chunk's worth is refused at the encoder. */
    satlink_cover_t huge = { .len = SATLINK_COVER_CHUNK_MAX + 1, .data = art };
    CHECK_EQ(satlink_enc_cover(g_buf, sizeof(g_buf), &huge), 0);
}

static void test_msg_volume_state(void)
{
    satlink_volume_state_t in = { .volume = 96, .source = 1 };
    size_t len = satlink_enc_volume_state(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 2);

    satlink_volume_state_t out = {0};
    CHECK(satlink_dec_volume_state(g_buf, len, &out));
    CHECK_EQ(out.volume, 96);
    CHECK_EQ(out.source, 1);
    CHECK_STRICT_LEN(satlink_dec_volume_state, out, len);
}

static void test_msg_commands(void)
{
    satlink_cmd_transport_t xport = { .cmd = SATLINK_XPORT_NEXT };
    size_t len = satlink_enc_cmd_transport(g_buf, sizeof(g_buf), &xport);
    CHECK_EQ(len, 1);
    satlink_cmd_transport_t xport_out = {0};
    CHECK(satlink_dec_cmd_transport(g_buf, len, &xport_out));
    CHECK_EQ(xport_out.cmd, SATLINK_XPORT_NEXT);
    CHECK_STRICT_LEN(satlink_dec_cmd_transport, xport_out, len);

    satlink_cmd_volume_t vol = { .volume = 127 };
    len = satlink_enc_cmd_volume(g_buf, sizeof(g_buf), &vol);
    CHECK_EQ(len, 1);
    satlink_cmd_volume_t vol_out = {0};
    CHECK(satlink_dec_cmd_volume(g_buf, len, &vol_out));
    CHECK_EQ(vol_out.volume, 127);

    satlink_cmd_radio_t radio = { .radio = 1, .discoverable = 1, .disconnect = 0 };
    len = satlink_enc_cmd_radio(g_buf, sizeof(g_buf), &radio);
    CHECK_EQ(len, 3);
    satlink_cmd_radio_t radio_out = {0};
    CHECK(satlink_dec_cmd_radio(g_buf, len, &radio_out));
    CHECK_EQ(radio_out.radio, 1);
    CHECK_EQ(radio_out.discoverable, 1);
    CHECK_EQ(radio_out.disconnect, 0);
    CHECK_STRICT_LEN(satlink_dec_cmd_radio, radio_out, len);
}

/*
 * Protocol 1.7. The bytes are checked against the same frozen vector the player's
 * own tests carry, so a change made on one side alone fails on that side rather
 * than on the bench. The document says "exactly 8 bytes, both nonzero", and both
 * halves of that sentence are load bearing: a zero pair is the shape a truncated
 * or padded frame takes, and acting on it would cost the phone an OBEX round trip
 * for an image the host is not assembling.
 */
static void test_msg_cover_retry(void)
{
    satlink_cover_retry_t retry = { .boot_nonce = 0x11223344u, .track_generation = 5u };
    size_t len = satlink_enc_cover_retry(g_buf, sizeof(g_buf), &retry);
    CHECK_EQ(len, 8);

    static const uint8_t wire[8] = { 0x44, 0x33, 0x22, 0x11, 0x05, 0x00, 0x00, 0x00 };
    CHECK(memcmp(g_buf, wire, sizeof(wire)) == 0);

    satlink_cover_retry_t out = {0};
    CHECK(satlink_dec_cover_retry(g_buf, len, &out));
    CHECK_EQ(out.boot_nonce, 0x11223344u);
    CHECK_EQ(out.track_generation, 5u);
    CHECK_STRICT_LEN(satlink_dec_cover_retry, out, len);

    /* Neither field may be zero, and the check is per field rather than per pair. */
    satlink_cover_retry_t no_nonce = { .boot_nonce = 0, .track_generation = 5u };
    len = satlink_enc_cover_retry(g_buf, sizeof(g_buf), &no_nonce);
    CHECK_EQ(len, 8);
    CHECK(!satlink_dec_cover_retry(g_buf, len, &out));

    satlink_cover_retry_t no_gen = { .boot_nonce = 7u, .track_generation = 0 };
    len = satlink_enc_cover_retry(g_buf, sizeof(g_buf), &no_gen);
    CHECK_EQ(len, 8);
    CHECK(!satlink_dec_cover_retry(g_buf, len, &out));
}

/*
 * The version and the two capability bits the 1.7 negotiation turns on.
 *
 * This is not decoration: bit 6 already means "takes streaming covers" in the HOST
 * word, and 1.7 gives bit 6 of the DEVICE word a different meaning. A refactor that
 * merged the two words, or moved either bit, would break a satellite talking to a
 * panel flashed at a different time -- which is the normal case here, not an edge.
 */
static void test_proto_1_7_constants(void)
{
    CHECK_EQ(SATLINK_PROTO_MAJOR, 1);
    CHECK_EQ(SATLINK_PROTO_MINOR, 7);
    CHECK_EQ(SATLINK_MSG_COVER_RETRY, 0x24);
    CHECK_EQ(SATLINK_CAP_COVER_RETRY, 1u << 6);   /* device word */
    CHECK_EQ(SATLINK_CAP_COVER_ART, 1u << 3);
    CHECK_EQ(COVER_STREAM_CAP, 1u << 6);          /* host word, and a different word */
    CHECK_EQ(SATLINK_HOST_CAP_COVER, 1u << 1);
    CHECK_EQ(COVER_RETRY_MAX, 2u);
}

static void test_msg_gpio_in_out(void)
{
    satlink_gpio_in_t in = { .values = 0x0000A5A5, .changed = 0x00000021 };
    size_t len = satlink_enc_gpio_in(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 8);
    satlink_gpio_in_t in_out = {0};
    CHECK(satlink_dec_gpio_in(g_buf, len, &in_out));
    CHECK_EQ(in_out.values, in.values);
    CHECK_EQ(in_out.changed, in.changed);
    CHECK_STRICT_LEN(satlink_dec_gpio_in, in_out, len);

    satlink_gpio_out_t out = { .values = 0xFFFFFFFF, .mask = 0x00000008 };
    len = satlink_enc_gpio_out(g_buf, sizeof(g_buf), &out);
    CHECK_EQ(len, 8);
    satlink_gpio_out_t out_out = {0};
    CHECK(satlink_dec_gpio_out(g_buf, len, &out_out));
    CHECK_EQ(out_out.values, out.values);
    CHECK_EQ(out_out.mask, out.mask);
}

static void test_msg_gpio_cfg(void)
{
    satlink_gpio_cfg_t in = { .count = 3 };
    in.pins[0] = (satlink_pin_cfg_t){ .index = 0, .mode = SATLINK_PIN_INPUT_PU, .initial = 0 };
    in.pins[1] = (satlink_pin_cfg_t){ .index = 1, .mode = SATLINK_PIN_OUTPUT,   .initial = 1 };
    in.pins[2] = (satlink_pin_cfg_t){ .index = 2, .mode = SATLINK_PIN_ANALOG,   .initial = 5 };

    size_t len = satlink_enc_gpio_cfg(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 1 + 3 * 3);

    satlink_gpio_cfg_t out = {0};
    CHECK(satlink_dec_gpio_cfg(g_buf, len, &out));
    CHECK_EQ(out.count, 3);
    CHECK_EQ(out.pins[1].mode, SATLINK_PIN_OUTPUT);
    CHECK_EQ(out.pins[1].initial, 1);
    CHECK_EQ(out.pins[2].mode, SATLINK_PIN_ANALOG);
    CHECK_EQ(out.pins[2].initial, 5);     /* half a second, in hundreds of ms */
    CHECK_STRICT_LEN(satlink_dec_gpio_cfg, out, len);

    /* Empty is legal. */
    satlink_gpio_cfg_t none = { .count = 0 };
    len = satlink_enc_gpio_cfg(g_buf, sizeof(g_buf), &none);
    CHECK_EQ(len, 1);
    CHECK(satlink_dec_gpio_cfg(g_buf, len, &out));
    CHECK_EQ(out.count, 0);

    /* A count past the 32-line ceiling the u32 masks impose is refused. */
    satlink_gpio_cfg_t over = { .count = SATLINK_GPIO_MAX + 1 };
    CHECK_EQ(satlink_enc_gpio_cfg(g_buf, sizeof(g_buf), &over), 0);
    uint8_t forged[] = { SATLINK_GPIO_MAX + 1 };
    CHECK(!satlink_dec_gpio_cfg(forged, sizeof(forged), &out));
}

static void test_msg_gpio_map(void)
{
    /* The starting configuration: the device understands the messages and has no
     * lines to offer yet. */
    satlink_gpio_map_t empty = { .count = 0 };
    size_t len = satlink_enc_gpio_map(g_buf, sizeof(g_buf), &empty);
    CHECK_EQ(len, 1);

    satlink_gpio_map_t out = {0};
    CHECK(satlink_dec_gpio_map(g_buf, len, &out));
    CHECK_EQ(out.count, 0);

    /* An input-only ESP32 pin: no output, no pull, but it can read an analogue
     * level. Exactly the case flags bit 3 exists for. */
    satlink_gpio_map_t in = { .count = 2 };
    in.pins[0] = (satlink_pin_map_t){ .index = 0, .gpio = 13,
                                      .flags = SATLINK_PIN_CAN_INPUT | SATLINK_PIN_CAN_OUTPUT |
                                               SATLINK_PIN_CAN_ADC | SATLINK_PIN_HAS_PULL };
    in.pins[1] = (satlink_pin_map_t){ .index = 1, .gpio = 34,
                                      .flags = SATLINK_PIN_CAN_INPUT | SATLINK_PIN_CAN_ADC };

    len = satlink_enc_gpio_map(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 1 + 2 * 3);
    CHECK(satlink_dec_gpio_map(g_buf, len, &out));
    CHECK_EQ(out.count, 2);
    CHECK_EQ(out.pins[1].gpio, 34);
    CHECK(!(out.pins[1].flags & SATLINK_PIN_HAS_PULL));
    CHECK(!(out.pins[1].flags & SATLINK_PIN_CAN_OUTPUT));
    CHECK(out.pins[0].flags & SATLINK_PIN_HAS_PULL);
    CHECK_STRICT_LEN(satlink_dec_gpio_map, out, len);
}

static void test_msg_adc(void)
{
    satlink_adc_value_t in = { .index = 2, .raw = 2048, .mv = 1650 };
    size_t len = satlink_enc_adc_value(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 5);

    satlink_adc_value_t out = {0};
    CHECK(satlink_dec_adc_value(g_buf, len, &out));
    CHECK_EQ(out.index, 2);
    CHECK_EQ(out.raw, 2048);
    CHECK_EQ(out.mv, 1650);
    CHECK_STRICT_LEN(satlink_dec_adc_value, out, len);

    satlink_adc_read_t req = { .index = 7 };
    len = satlink_enc_adc_read(g_buf, sizeof(g_buf), &req);
    CHECK_EQ(len, 1);
    satlink_adc_read_t req_out = {0};
    CHECK(satlink_dec_adc_read(g_buf, len, &req_out));
    CHECK_EQ(req_out.index, 7);
}

static void test_msg_log(void)
{
    satlink_log_t in = { .level = SATLINK_LOG_WARN, .text = satlink_str("ring full") };
    size_t len = satlink_enc_log(g_buf, sizeof(g_buf), &in);
    CHECK_EQ(len, 1 + 2 + 9);

    satlink_log_t out = {0};
    CHECK(satlink_dec_log(g_buf, len, &out));
    CHECK_EQ(out.level, SATLINK_LOG_WARN);
    CHECK_EQ(out.text.len, 9);
    CHECK_EQ(memcmp(out.text.ptr, "ring full", 9), 0);
    CHECK_STRICT_LEN(satlink_dec_log, out, len);

    /* A log line longer than a frame is refused rather than truncated: half an
     * error message is its own kind of bug report. */
    static char long_text[SATLINK_PAYLOAD_MAX + 16];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    satlink_log_t big = { .level = SATLINK_LOG_INFO, .text = satlink_str(long_text) };
    CHECK_EQ(satlink_enc_log(g_buf, sizeof(g_buf), &big), 0);
}

static void test_encoders_refuse_small_buffers(void)
{
    /* Every encoder must say no rather than scribble past the end. One byte of
     * room is enough to catch the ones that check only at the start. */
    uint8_t tiny[1];
    satlink_hello_t hello = { .build = satlink_str("x") };
    CHECK_EQ(satlink_enc_hello(tiny, sizeof(tiny), &hello), 0);

    satlink_pong_t pong = {0};
    CHECK_EQ(satlink_enc_pong(tiny, sizeof(tiny), &pong), 0);

    satlink_bt_state_t_msg bt = { .name = satlink_str("x") };
    CHECK_EQ(satlink_enc_bt_state(tiny, sizeof(tiny), &bt), 0);

    satlink_track_meta_t meta = { satlink_str("a"), satlink_str("b"), satlink_str("c") };
    CHECK_EQ(satlink_enc_track_meta(tiny, sizeof(tiny), &meta), 0);

    satlink_track_pos_t pos = {0};
    CHECK_EQ(satlink_enc_track_pos(tiny, sizeof(tiny), &pos), 0);

    satlink_stream_fmt_t fmt = {0};
    CHECK_EQ(satlink_enc_stream_fmt(tiny, sizeof(tiny), &fmt), 0);

    uint8_t art[4] = {1, 2, 3, 4};
    satlink_cover_t cover = { .len = 4, .data = art };
    CHECK_EQ(satlink_enc_cover(tiny, sizeof(tiny), &cover), 0);

    satlink_gpio_in_t gin = {0};
    CHECK_EQ(satlink_enc_gpio_in(tiny, sizeof(tiny), &gin), 0);

    satlink_gpio_map_t map = { .count = 1 };
    CHECK_EQ(satlink_enc_gpio_map(tiny, sizeof(tiny), &map), 0);

    satlink_adc_value_t adc = {0};
    CHECK_EQ(satlink_enc_adc_value(tiny, sizeof(tiny), &adc), 0);

    satlink_log_t log = { .text = satlink_str("hello") };
    CHECK_EQ(satlink_enc_log(tiny, sizeof(tiny), &log), 0);
}

static void test_utf8_clip(void)
{
    /* Cutting a UTF-8 string at a byte boundary is how invalid UTF-8 gets onto a
     * wire that promises UTF-8. The real track title from the bench is the test
     * case: "Salomé" is seven bytes for six characters. */
    const char *s = "Salomé Das";               /* ten characters, eleven bytes */

    CHECK_EQ(satlink_str_clip(s, 100).len, 11);   /* under the limit: untouched */
    CHECK_EQ(satlink_str_clip(s, 11).len, 11);    /* exactly the limit */
    CHECK_EQ(satlink_str_clip(s, 10).len, 10);    /* cuts between two plain bytes */
    CHECK_EQ(satlink_str_clip(s, 7).len, 7);      /* right after the é */
    CHECK_EQ(satlink_str_clip(s, 6).len, 5);      /* mid-é: back off the whole character */
    CHECK_EQ(satlink_str_clip(s, 5).len, 5);
    CHECK_EQ(satlink_str_clip(s, 0).len, 0);
    CHECK_EQ(satlink_str_clip(NULL, 10).len, 0);

    /* A three-byte character, cut at each of its bytes. */
    const char *cyr = "aбв";                       /* 1 + 2 + 2 bytes */
    CHECK_EQ(satlink_str_clip(cyr, 2).len, 1);
    CHECK_EQ(satlink_str_clip(cyr, 3).len, 3);
    CHECK_EQ(satlink_str_clip(cyr, 4).len, 3);
    CHECK_EQ(satlink_str_clip(cyr, 5).len, 5);
}

static void test_every_message_through_a_frame(void)
{
    /* The largest payload any message can produce still has to fit a frame, and
     * the frame still has to fit the buffer the protocol mandates. COVER is the
     * one that decides this, so check it end to end rather than by arithmetic. */
    uint8_t art[SATLINK_COVER_CHUNK_MAX];
    memset(art, 0, sizeof(art));           /* all zeros: worst case for COBS */
    satlink_cover_t cover = { .chunk = 0, .total = 256, .mime = SATLINK_MIME_JPEG,
                              .total_len = SATLINK_COVER_TOTAL_MAX,
                              .len = sizeof(art), .data = art };

    size_t payload_len = satlink_enc_cover(g_buf, sizeof(g_buf), &cover);
    CHECK(payload_len > 0);

    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;
    CHECK(satlink_frame_encode(SATLINK_MSG_COVER, 0, g_buf, payload_len,
                               wire, sizeof(wire), &wire_len));
    CHECK(wire_len <= SATLINK_WIRE_MAX);
}

const test_case_t message_tests[] = {
    { "hello",                 test_msg_hello },
    { "ping and pong",         test_msg_ping_pong },
    { "bt state",              test_msg_bt_state },
    { "track meta",            test_msg_track_meta },
    { "track pos",             test_msg_track_pos },
    { "sanitize total_ms",     test_sanitize_total_ms },
    { "stream fmt",            test_msg_stream_fmt },
    { "cover",                 test_msg_cover },
    { "volume state",          test_msg_volume_state },
    { "host commands",         test_msg_commands },
    { "cover retry",           test_msg_cover_retry },
    { "protocol 1.7 constants", test_proto_1_7_constants },
    { "gpio in and out",       test_msg_gpio_in_out },
    { "gpio cfg",              test_msg_gpio_cfg },
    { "gpio map",              test_msg_gpio_map },
    { "adc",                   test_msg_adc },
    { "log",                   test_msg_log },
    { "encoders refuse small", test_encoders_refuse_small_buffers },
    { "worst case in a frame", test_every_message_through_a_frame },
    { "utf-8 clipping",        test_utf8_clip },
};

const size_t message_test_count = sizeof(message_tests) / sizeof(message_tests[0]);
