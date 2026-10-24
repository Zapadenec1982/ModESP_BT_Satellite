/*
 * Device-side rules of PROTOCOL.md that live in main/ as pure C, walked without a
 * board: LOG lines from the ESP log, what the link lets through for a host with
 * another proto_major, the slots that hold the messages which must not be lost,
 * the range of VOLUME_STATE, the stream suspend after a pause from the host, and
 * when a greeting or an I2S role change starts a new format transaction.
 */

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "avrc_volume.h"
#include "format_policy.h"
#include "link_policy.h"
#include "log_line.h"
#include "pause_policy.h"
#include "tx_slots.h"

int g_checks, g_failures;

/* ----------------------------------------------------------------------- LOG */

/* The bridge's two steps on a copy of `in`: level first, then the tidy text. */
static uint8_t tidy(const char *in, char *out, size_t cap)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", in);
    uint8_t level = log_line_level(buf);
    snprintf(out, cap, "%s", log_line_tidy(buf));
    return level;
}

static void test_log_level(void)
{
    char text[256];

    /* IDF log v1, colours off, which is this build. */
    CHECK_EQ(tidy("E (1234) sat.link: bad\n", text, sizeof text), SATLINK_LOG_ERROR);
    CHECK(strcmp(text, "E (1234) sat.link: bad") == 0);
    CHECK_EQ(tidy("W (1) t: w\n", text, sizeof text), SATLINK_LOG_WARN);
    CHECK_EQ(tidy("I (1) t: i\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK_EQ(tidy("D (1) t: d\n", text, sizeof text), SATLINK_LOG_DEBUG);
    CHECK_EQ(tidy("V (1) t: v\n", text, sizeof text), SATLINK_LOG_DEBUG);   /* no word for it */

    /* Colours on: the escape in front no longer hides the letter, and both
     * escapes are gone from the text. */
    CHECK_EQ(tidy("\033[0;31mE (5) BT_HCI: oops\033[0m\n", text, sizeof text), SATLINK_LOG_ERROR);
    CHECK(strcmp(text, "E (5) BT_HCI: oops") == 0);
    CHECK_EQ(tidy("\033[0;33mW (5) t: careful\033[0m\n", text, sizeof text), SATLINK_LOG_WARN);
    CHECK(strcmp(text, "W (5) t: careful") == 0);
    CHECK_EQ(tidy("\033[0;32mI (5) t: fine\033[0m\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK(strcmp(text, "I (5) t: fine") == 0);

    /* No letter: a bare esp_log_write. Info, as it always was. */
    CHECK_EQ(tidy("plain text\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK(strcmp(text, "plain text") == 0);
    CHECK_EQ(tidy("Error without the shape\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK_EQ(tidy("E", text, sizeof text), SATLINK_LOG_INFO);
    CHECK_EQ(tidy("", text, sizeof text), SATLINK_LOG_INFO);
    CHECK(strcmp(text, "") == 0);

    /* A trailing 'm' that is not an escape stays. */
    CHECK_EQ(tidy("I (1) t: 42 m\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK(strcmp(text, "I (1) t: 42 m") == 0);
    tidy("I (1) t: x[0m\n", text, sizeof text);
    CHECK(strcmp(text, "I (1) t: x[0m") == 0);

    /* The reset cut short by a full buffer is dropped with the rest of it. */
    tidy("\033[0;31mE (5) t: long\033[0", text, sizeof text);
    CHECK(strcmp(text, "E (5) t: long") == 0);
    tidy("\033[0;31mE (5) t: long\033[", text, sizeof text);
    CHECK(strcmp(text, "E (5) t: long") == 0);
    tidy("\033[0;31mE (5) t: long\033", text, sizeof text);
    CHECK(strcmp(text, "E (5) t: long") == 0);

    /* Only newlines and escapes: nothing to send. */
    tidy("\033[0m\r\n", text, sizeof text);
    CHECK(strcmp(text, "") == 0);
}

/* ---------------------------------------------------------------------- link */

static void test_incompatible_host(void)
{
    /* PROTOCOL.md 3 and 13: HELLO, PING, PONG and LOG, and nothing else. */
    unsigned allowed = 0;
    for (unsigned type = 0; type <= 0xFF; type++) {
        if (link_incompatible_allows((uint8_t)type)) {
            allowed++;
        }
    }
    CHECK_EQ(allowed, 4);
    CHECK(link_incompatible_allows(SATLINK_MSG_HELLO));
    CHECK(link_incompatible_allows(SATLINK_MSG_PING));
    CHECK(link_incompatible_allows(SATLINK_MSG_PONG));
    CHECK(link_incompatible_allows(SATLINK_MSG_LOG));

    /* The commands that used to get through. */
    CHECK(!link_incompatible_allows(SATLINK_MSG_CMD_TRANSPORT));
    CHECK(!link_incompatible_allows(SATLINK_MSG_CMD_VOLUME));
    CHECK(!link_incompatible_allows(SATLINK_MSG_CMD_RADIO));
    CHECK(!link_incompatible_allows(SATLINK_MSG_GPIO_OUT));
    CHECK(!link_incompatible_allows(SATLINK_MSG_GPIO_CFG));
    CHECK(!link_incompatible_allows(SATLINK_MSG_ADC_READ));
    /* And what the cover stream sends. */
    CHECK(!link_incompatible_allows(0x18));      /* COVER_DATA */
    CHECK(!link_incompatible_allows(0x19));      /* COVER_END */
}

/* -------------------------------------------------------------------- volume */

static void test_abs_volume(void)
{
    /* PROTOCOL.md 7: VOLUME_STATE carries 0..127. AVRCP's bit 7 is reserved, and
     * the controller path of the stack passes it through. */
    for (unsigned raw = 0; raw <= 0xFF; raw++) {
        CHECK(avrc_abs_volume((uint8_t)raw) <= 127);
    }
    CHECK_EQ(avrc_abs_volume(0x00), 0x00);
    CHECK_EQ(avrc_abs_volume(0x40), 0x40);
    CHECK_EQ(avrc_abs_volume(0x7F), 0x7F);
    /* Masked like the stack's own target parser, not clamped. */
    CHECK_EQ(avrc_abs_volume(0x80), 0x00);
    CHECK_EQ(avrc_abs_volume(0xC0), 0x40);
    CHECK_EQ(avrc_abs_volume(0xFF), 0x7F);
}

/* ---------------------------------------------------------------- tx slots */

static uint8_t b_state[16], b_meta[32], b_gpio[16], b_fmt[16], b_adc[2][16];

static void slots_init(tx_slot_t *t)
{
    const tx_slot_t init[] = {
        { .type = SATLINK_MSG_BT_STATE,     .cap = sizeof b_state, .buf = b_state },
        { .type = SATLINK_MSG_TRACK_META,   .cap = sizeof b_meta,  .buf = b_meta },
        { .type = SATLINK_MSG_GPIO_IN,      .cap = sizeof b_gpio,  .buf = b_gpio },
        { .type = SATLINK_MSG_STREAM_PREPARE, .cap = sizeof b_fmt, .buf = b_fmt },
        { .type = SATLINK_MSG_ADC_VALUE,    .cap = sizeof b_adc[0], .buf = b_adc[0] },
        { .type = SATLINK_MSG_ADC_VALUE,    .cap = sizeof b_adc[1], .buf = b_adc[1] },
    };
    memcpy(t, init, sizeof init);
}
#define NSLOTS 6

typedef struct { uint8_t type; uint16_t len; uint8_t buf[64]; uint32_t nonce, gen; } taken_t;

static bool take(tx_slot_t *t, taken_t *out)
{
    return tx_slot_take(t, NSLOTS, out->buf, sizeof out->buf, &out->type, &out->len,
                        &out->nonce, &out->gen);
}

static void test_slots_replace_in_order(void)
{
    tx_slot_t t[NSLOTS];
    slots_init(t);
    uint32_t stamp = 0;
    taken_t got;

    const uint8_t a[] = { 'A' }, m[] = { 'M' }, b[] = { 'B', 'B' };
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_BT_STATE, 0, a, 1, 0, 0, &stamp), TX_SLOT_QUEUED);
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_TRACK_META, 0, m, 1, 0, 0, &stamp), TX_SLOT_QUEUED);
    /* The unsent BT_STATE is replaced by the newer one, which now waits behind the
     * TRACK_META queued before it. */
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_BT_STATE, 0, b, 2, 0, 0, &stamp), TX_SLOT_REPLACED);

    CHECK(take(t, &got));
    CHECK_EQ(got.type, SATLINK_MSG_TRACK_META);
    CHECK(take(t, &got));
    CHECK_EQ(got.type, SATLINK_MSG_BT_STATE);
    CHECK_EQ(got.len, 2);
    CHECK_EQ(got.buf[0], 'B');
    CHECK(!take(t, &got));

    /* Once sent, the next one is new, not a replacement. */
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_BT_STATE, 0, a, 1, 0, 0, &stamp), TX_SLOT_QUEUED);
}

static void test_slots_never_lose_the_latest(void)
{
    /* A host that stopped reading, and a thousand state changes: the critical queue
     * of ten used to evict the oldest. Now each type holds its newest. */
    tx_slot_t t[NSLOTS];
    slots_init(t);
    uint32_t stamp = 0;
    unsigned replaced = 0;
    for (unsigned i = 0; i < 1000; i++) {
        uint8_t v[2] = { (uint8_t)i, (uint8_t)(i >> 8) };
        replaced += tx_slot_put(t, NSLOTS, SATLINK_MSG_BT_STATE, 0, v, 2, 0, 0, &stamp) == TX_SLOT_REPLACED;
        replaced += tx_slot_put(t, NSLOTS, SATLINK_MSG_TRACK_META, 0, v, 2, 0, 0, &stamp) == TX_SLOT_REPLACED;
    }
    CHECK_EQ(replaced, 2 * 999);

    taken_t got;
    unsigned n = 0;
    while (take(t, &got)) {
        n++;
        CHECK_EQ(got.buf[0] | (got.buf[1] << 8), 999);
    }
    CHECK_EQ(n, 2);
}

static void put_gpio(tx_slot_t *t, uint32_t *stamp, uint32_t values, uint32_t changed,
                     tx_slot_result_t want)
{
    uint8_t p[8];
    tx_put32(p, values);
    tx_put32(p + 4, changed);
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_GPIO_IN, 0, p, 8, 0, 0, stamp), want);
}

static void test_slots_merge_gpio_in(void)
{
    tx_slot_t t[NSLOTS];
    slots_init(t);
    uint32_t stamp = 0;
    taken_t got;

    /* Line 0 pressed, then line 1 pressed before the first went out: the newest
     * levels, and both lines marked changed. */
    put_gpio(t, &stamp, 0x1, 0x1, TX_SLOT_QUEUED);
    put_gpio(t, &stamp, 0x3, 0x2, TX_SLOT_MERGED);
    CHECK(take(t, &got));
    CHECK_EQ(tx_get32(got.buf), 0x3);
    CHECK_EQ(tx_get32(got.buf + 4), 0x3);

    /* Pressed and released before either went out: the level is back, and the
     * line still says it changed. */
    put_gpio(t, &stamp, 0x7, 0x4, TX_SLOT_QUEUED);
    put_gpio(t, &stamp, 0x3, 0x4, TX_SLOT_MERGED);
    CHECK(take(t, &got));
    CHECK_EQ(tx_get32(got.buf), 0x3);
    CHECK_EQ(tx_get32(got.buf + 4), 0x4);

    /* After it went out, the next starts afresh. */
    put_gpio(t, &stamp, 0x2, 0x1, TX_SLOT_QUEUED);
    CHECK(take(t, &got));
    CHECK_EQ(tx_get32(got.buf + 4), 0x1);
}

static void test_slots_format_tags(void)
{
    tx_slot_t t[NSLOTS];
    slots_init(t);
    uint32_t stamp = 0;
    taken_t got;
    const uint8_t p[14] = { 1 };

    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_STREAM_PREPARE, 0, p, 14, 7, 1, &stamp), TX_SLOT_QUEUED);
    /* A retry of a newer transaction before the first went out: its tags win, so
     * the completion recorded on the wire is the one the format machine waits for. */
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_STREAM_PREPARE, 0, p, 14, 7, 2, &stamp), TX_SLOT_REPLACED);
    CHECK(take(t, &got));
    CHECK_EQ(got.nonce, 7);
    CHECK_EQ(got.gen, 2);
    CHECK_EQ(got.len, 14);
}

static void test_slots_adc_per_line(void)
{
    tx_slot_t t[NSLOTS];
    slots_init(t);
    uint32_t stamp = 0;
    taken_t got;
    const uint8_t p[5] = { 0 };

    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_ADC_VALUE, 3, p, 5, 0, 0, &stamp), TX_SLOT_QUEUED);
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_ADC_VALUE, 5, p, 5, 0, 0, &stamp), TX_SLOT_QUEUED);
    /* The same line replaces its own value, never another line's. */
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_ADC_VALUE, 3, p, 5, 0, 0, &stamp), TX_SLOT_REPLACED);
    /* Both slots busy with other lines: a fault, counted by the caller. */
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_ADC_VALUE, 7, p, 5, 0, 0, &stamp), TX_SLOT_NO_SLOT);
    CHECK(take(t, &got));
    CHECK(take(t, &got));
    CHECK(!take(t, &got));
    /* Idle again, so any line can have one. */
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_ADC_VALUE, 7, p, 5, 0, 0, &stamp), TX_SLOT_QUEUED);
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_ADC_VALUE, 3, p, 5, 0, 0, &stamp), TX_SLOT_QUEUED);
}

static void test_slots_faults_and_wrap(void)
{
    tx_slot_t t[NSLOTS];
    slots_init(t);
    taken_t got;
    const uint8_t big[40] = { 0 };

    /* Stamps wrap; the order does not. */
    uint32_t stamp = 0xFFFFFFFEu;
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_BT_STATE, 0, big, 1, 0, 0, &stamp), TX_SLOT_QUEUED);
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_TRACK_META, 0, big, 1, 0, 0, &stamp), TX_SLOT_QUEUED);
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_GPIO_IN, 0, big, 8, 0, 0, &stamp), TX_SLOT_QUEUED);
    CHECK_EQ(stamp, 1);
    CHECK(take(t, &got)); CHECK_EQ(got.type, SATLINK_MSG_BT_STATE);
    CHECK(take(t, &got)); CHECK_EQ(got.type, SATLINK_MSG_TRACK_META);
    CHECK(take(t, &got)); CHECK_EQ(got.type, SATLINK_MSG_GPIO_IN);

    /* No slot, or too long for one: refused, and what was waiting is untouched. */
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_HELLO, 0, big, 1, 0, 0, &stamp), TX_SLOT_NO_SLOT);
    const uint8_t a[] = { 'A' };
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_BT_STATE, 0, a, 1, 0, 0, &stamp), TX_SLOT_QUEUED);
    CHECK_EQ(tx_slot_put(t, NSLOTS, SATLINK_MSG_BT_STATE, 0, big, sizeof big, 0, 0, &stamp), TX_SLOT_TOO_LONG);
    CHECK(take(t, &got));
    CHECK_EQ(got.len, 1);
    CHECK_EQ(got.buf[0], 'A');
}

static size_t enc_three(uint8_t *buf, size_t cap, const void *msg)
{
    (void)msg;
    if (cap < 3) return 0;
    buf[0] = 1; buf[1] = 2; buf[2] = 3;
    return 3;
}

static size_t enc_fails(uint8_t *buf, size_t cap, const void *msg)
{
    (void)buf; (void)cap; (void)msg;
    return 0;
}

static void test_slots_encode_in_place(void)
{
    tx_slot_t t[NSLOTS];
    slots_init(t);
    uint32_t stamp = 0;
    taken_t got;

    CHECK_EQ(tx_slot_encode(t, NSLOTS, SATLINK_MSG_TRACK_META, enc_three, NULL, &stamp), TX_SLOT_QUEUED);
    CHECK_EQ(tx_slot_encode(t, NSLOTS, SATLINK_MSG_TRACK_META, enc_three, NULL, &stamp), TX_SLOT_REPLACED);
    CHECK(take(t, &got));
    CHECK_EQ(got.len, 3);
    CHECK_EQ(got.buf[2], 3);

    CHECK_EQ(tx_slot_encode(t, NSLOTS, SATLINK_MSG_TRACK_META, enc_three, NULL, &stamp), TX_SLOT_QUEUED);
    CHECK_EQ(tx_slot_encode(t, NSLOTS, SATLINK_MSG_TRACK_META, enc_fails, NULL, &stamp), TX_SLOT_TOO_LONG);
    CHECK(!take(t, &got));      /* half-written, so not sent */
    CHECK_EQ(tx_slot_encode(t, NSLOTS, SATLINK_MSG_HELLO, enc_three, NULL, &stamp), TX_SLOT_NO_SLOT);
}

/* ------------------------------------------------------------- pause check */

#define GRACE_MS 5000u

/* bt_av.c's one-shot esp_timer and stream state, with time in milliseconds. */
typedef struct {
    bool     waiting;
    uint32_t due_ms;
    uint32_t now_ms;
    uint8_t  bt_state;
    int      suspends;
} pause_sim_t;

static void sim_apply(pause_sim_t *s, pause_timer_op_t op)
{
    if (op == PAUSE_TIMER_START) {
        s->waiting = true;
        s->due_ms = s->now_ms + GRACE_MS;
    } else if (op == PAUSE_TIMER_STOP) {
        s->waiting = false;
    }
}

static void sim_until(pause_sim_t *s, uint32_t t_ms)
{
    if (s->waiting && s->due_ms <= t_ms) {
        s->now_ms = s->due_ms;
        s->waiting = false;
        if (pause_suspends(s->bt_state)) {
            s->suspends++;
            s->bt_state = SATLINK_BT_CONNECTED;     /* what the stack reports back */
            sim_apply(s, pause_on_stream(false));
        }
    }
    s->now_ms = t_ms;
}

static void sim_host(pause_sim_t *s, uint32_t t_ms, uint8_t cmd)
{
    sim_until(s, t_ms);
    sim_apply(s, pause_on_transport(cmd, s->waiting));
}

static void sim_stream(pause_sim_t *s, uint32_t t_ms, bool started)
{
    sim_until(s, t_ms);
    s->bt_state = started ? SATLINK_BT_STREAMING : SATLINK_BT_CONNECTED;
    sim_apply(s, pause_on_stream(started));
}

static void sim_play_status(pause_sim_t *s, uint32_t t_ms, uint8_t play_state)
{
    sim_until(s, t_ms);
    sim_apply(s, pause_on_play_status(play_state));
}

static pause_sim_t sim_streaming(void)
{
    return (pause_sim_t){ .bt_state = SATLINK_BT_STREAMING };
}

static void test_pause_check(void)
{
    /* A phone that ignores the pause: its stream is suspended once the wait runs out. */
    pause_sim_t s = sim_streaming();
    sim_host(&s, 0, SATLINK_XPORT_PAUSE);
    sim_until(&s, GRACE_MS - 1);
    CHECK_EQ(s.suspends, 0);
    sim_until(&s, GRACE_MS);
    CHECK_EQ(s.suspends, 1);
    sim_until(&s, 60000);
    CHECK_EQ(s.suspends, 1);

    /* STOP is a pause too, and a stream with no AVRCP status at all is still caught. */
    s = sim_streaming();
    sim_host(&s, 0, SATLINK_XPORT_STOP);
    sim_until(&s, 60000);
    CHECK_EQ(s.suspends, 1);

    /* The phone pauses, holds its stream open, and its user plays again inside the wait. */
    s = sim_streaming();
    sim_host(&s, 0, SATLINK_XPORT_PAUSE);
    sim_play_status(&s, 300, SATLINK_PLAY_PAUSED);
    sim_play_status(&s, 2000, SATLINK_PLAY_PLAYING);
    sim_until(&s, 60000);
    CHECK_EQ(s.suspends, 0);
    CHECK_EQ(s.bt_state, SATLINK_BT_STREAMING);

    /* The phone closes its stream after the pause and its user starts it again. */
    s = sim_streaming();
    sim_host(&s, 0, SATLINK_XPORT_PAUSE);
    sim_stream(&s, 3000, false);
    sim_stream(&s, 4000, true);
    sim_until(&s, 60000);
    CHECK_EQ(s.suspends, 0);
    CHECK_EQ(s.bt_state, SATLINK_BT_STREAMING);

    /* A "playing" that does not stop the phone: the host's repeated PAUSE starts the
     * wait again, and the stream is caught a wait later. */
    s = sim_streaming();
    sim_host(&s, 0, SATLINK_XPORT_PAUSE);
    sim_play_status(&s, 1000, SATLINK_PLAY_PLAYING);
    sim_host(&s, 3000, SATLINK_XPORT_PAUSE);
    sim_until(&s, 3000 + GRACE_MS - 1);
    CHECK_EQ(s.suspends, 0);
    sim_until(&s, 3000 + GRACE_MS);
    CHECK_EQ(s.suspends, 1);

    /* A PAUSE repeated inside the wait does not restart it. */
    s = sim_streaming();
    sim_host(&s, 0, SATLINK_XPORT_PAUSE);
    sim_host(&s, 3000, SATLINK_XPORT_PAUSE);
    sim_play_status(&s, 3500, SATLINK_PLAY_PAUSED);
    sim_until(&s, GRACE_MS);
    CHECK_EQ(s.suspends, 1);

    /* Any other command from the host ends the wait. */
    for (uint8_t cmd = SATLINK_XPORT_PLAY; cmd <= SATLINK_XPORT_REW_STOP; cmd++) {
        if (cmd == SATLINK_XPORT_PAUSE || cmd == SATLINK_XPORT_STOP) {
            continue;
        }
        s = sim_streaming();
        sim_host(&s, 0, SATLINK_XPORT_PAUSE);
        sim_host(&s, 1000, cmd);
        sim_until(&s, 60000);
        CHECK_EQ(s.suspends, 0);
    }

    /* A stream that is already quiet when the wait runs out is left alone. */
    s = (pause_sim_t){ .bt_state = SATLINK_BT_CONNECTED };
    sim_host(&s, 0, SATLINK_XPORT_PAUSE);
    sim_until(&s, 60000);
    CHECK_EQ(s.suspends, 0);
}

/* ------------------------------------------------------------- format restart */

static void test_format_restart(void)
{
    /* Nothing changed: the current transaction is only announced again. */
    CHECK(!fmt_needs_restart(true, 16, 16, false, false, false, true));
    CHECK(!fmt_needs_restart(true, 32, 32, true, true, false, true));

    /* A new role with the same slots and acknowledgement still needs one: set_role
     * gated PCM and voided the resume token, so a repeat would never resume. */
    CHECK(fmt_needs_restart(true, 32, 32, false, false, true, true));
    CHECK(fmt_needs_restart(true, 32, 32, true, true, true, true));
    /* The slave-to-legacy-master case: 32-bit slots become 16, announced first. */
    CHECK(fmt_needs_restart(true, 32, 16, false, false, true, true));

    /* The reasons a greeting alone starts one. */
    CHECK(fmt_needs_restart(true, 16, 32, false, false, false, true));
    CHECK(fmt_needs_restart(true, 32, 32, false, true, false, true));
    CHECK(fmt_needs_restart(true, 32, 32, true, false, false, true));
    CHECK(fmt_needs_restart(false, 32, 32, false, false, false, true));
    CHECK(fmt_needs_restart(true, 32, 32, false, false, false, false));
}

int main(void)
{
    test_log_level();
    test_incompatible_host();
    test_abs_volume();
    test_slots_replace_in_order();
    test_slots_never_lose_the_latest();
    test_slots_merge_gpio_in();
    test_slots_format_tags();
    test_slots_adc_per_line();
    test_slots_faults_and_wrap();
    test_slots_encode_in_place();
    test_pause_check();
    test_format_restart();
    printf("Device rules: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
