/*
 * Device-side rules of PROTOCOL.md that live in main/ as pure C, walked without a
 * board: the port expander's GPIO_CFG handling, LOG lines from the ESP log, what
 * the link lets through for a host with another proto_major, the slots that
 * hold the messages which must not be lost, and the range of VOLUME_STATE.
 */

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "avrc_volume.h"
#include "expander_policy.h"
#include "link_policy.h"
#include "log_line.h"
#include "tx_slots.h"

int g_checks, g_failures;

/* ------------------------------------------------------------------ expander */

#define CAN_ALL (SATLINK_PIN_CAN_INPUT | SATLINK_PIN_CAN_OUTPUT | \
                 SATLINK_PIN_CAN_ADC | SATLINK_PIN_HAS_PULL)
#define IN_ONLY SATLINK_PIN_CAN_INPUT       /* GPIO34..39: no output, no pull */

static void test_legal_mode(void)
{
    exp_mode_fix_t why;

    /* Everything a capable line is asked for, it gets. */
    for (uint8_t mode = SATLINK_PIN_DISABLED; mode <= SATLINK_PIN_ANALOG; mode++) {
        CHECK_EQ(exp_legal_mode(mode, CAN_ALL, &why), mode);
        CHECK_EQ(why, EXP_MODE_AS_ASKED);
    }

    /* An input-only line without pulls: the nearest legal thing is a plain input. */
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_INPUT_PU, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_NO_PULL);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_INPUT_PD, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_NO_PULL);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_OUTPUT, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_INPUT_ONLY);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_OUTPUT_OD, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_INPUT_ONLY);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_ANALOG, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_NO_ADC);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_INPUT, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_AS_ASKED);

    /* A mode the protocol does not define is disabled, and a disabled line is not
     * an input: it used to be configured as one and polled as the other. */
    for (unsigned mode = SATLINK_PIN_ANALOG + 1; mode <= 0xFF; mode++) {
        CHECK_EQ(exp_legal_mode((uint8_t)mode, CAN_ALL, &why), SATLINK_PIN_DISABLED);
        CHECK_EQ(why, EXP_MODE_UNKNOWN);
    }
    CHECK(!exp_mode_is_input(SATLINK_PIN_DISABLED));
    CHECK(exp_mode_is_input(SATLINK_PIN_INPUT));
    CHECK(exp_mode_is_input(SATLINK_PIN_INPUT_PU));
    CHECK(exp_mode_is_input(SATLINK_PIN_INPUT_PD));
    CHECK(!exp_mode_is_input(SATLINK_PIN_OUTPUT));
    CHECK(!exp_mode_is_input(SATLINK_PIN_OUTPUT_OD));
    CHECK(!exp_mode_is_input(SATLINK_PIN_ANALOG));

    CHECK_EQ(exp_legal_mode(9, CAN_ALL, NULL), SATLINK_PIN_DISABLED);   /* `why` is optional */
}

/* Ticks until each of the first `n` samples, for a line configured with `initial`. */
static void sample_ticks(uint8_t initial, unsigned *at, unsigned n, unsigned limit)
{
    const uint16_t period = exp_adc_period_ticks(initial);
    uint16_t countdown = period;
    unsigned got = 0;
    for (unsigned tick = 1; tick <= limit && got < n; tick++) {
        if (exp_adc_tick(&countdown, period)) {
            at[got++] = tick;
        }
    }
    for (; got < n; got++) {
        at[got] = 0;
    }
}

static void test_adc_period(void)
{
    /* `initial` is hundreds of milliseconds: 1 is 100 ms, which is 20 polls of 5 ms.
     * It used to be read as poll ticks, (initial + 1) x 5 ms. */
    CHECK_EQ(exp_adc_period_ticks(0), 0);
    CHECK_EQ(exp_adc_period_ticks(1), 100 / EXP_POLL_MS);
    CHECK_EQ(exp_adc_period_ticks(10), 1000 / EXP_POLL_MS);
    CHECK_EQ(exp_adc_period_ticks(255), 25500 / EXP_POLL_MS);   /* fits in u16 */

    unsigned at[3];
    sample_ticks(1, at, 3, 1000);
    CHECK_EQ(at[0] * EXP_POLL_MS, 100);
    CHECK_EQ(at[1] * EXP_POLL_MS, 200);
    CHECK_EQ(at[2] * EXP_POLL_MS, 300);

    sample_ticks(10, at, 3, 10000);
    CHECK_EQ(at[0] * EXP_POLL_MS, 1000);
    CHECK_EQ(at[1] * EXP_POLL_MS, 2000);
    CHECK_EQ(at[2] * EXP_POLL_MS, 3000);

    sample_ticks(255, at, 2, 20000);
    CHECK_EQ(at[0] * EXP_POLL_MS, 25500);
    CHECK_EQ(at[1] * EXP_POLL_MS, 51000);

    /* 0: only on ADC_READ, never by itself. */
    sample_ticks(0, at, 1, 100000);
    CHECK_EQ(at[0], 0);
}

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

int main(void)
{
    test_legal_mode();
    test_adc_period();
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
    printf("Device rules: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
