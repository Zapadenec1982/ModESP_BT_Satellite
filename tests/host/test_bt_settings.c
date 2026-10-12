/*
 * Protocol 1.8 on the device side, walked without a stack or a board: the settings
 * and how a BT_CONFIG_SET is taken, the bond table, pairing through the host, and
 * calling the last phone back.
 */

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "bt_settings.h"
#include "bond_table.h"
#include "pair_policy.h"
#include "reconnect_policy.h"

int g_checks, g_failures;

#define SBC   SATLINK_CODEC_BIT_SBC
#define AAC   SATLINK_CODEC_BIT_AAC
#define APTX  SATLINK_CODEC_BIT_APTX
#define APTXH SATLINK_CODEC_BIT_APTX_HD
#define LHDC  SATLINK_CODEC_BIT_LHDC_V5
#define ALL   (SBC | AAC | APTX | APTXH | LHDC)

static const uint8_t A[6] = { 0xDC, 0xE5, 0x5B, 0xEA, 0xC9, 0x3B };
static const uint8_t B[6] = { 0x34, 0xF3, 0x9A, 0xF1, 0xC4, 0x98 };
static const uint8_t C[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

/* ------------------------------------------------------------------ settings */

static void test_defaults(void)
{
    bt_settings_t s;
    bt_settings_defaults(&s, "modesp satellite");
    CHECK_EQ(s.radio, 1);
    CHECK_EQ(s.discoverable, 1);
    CHECK_EQ(s.auto_reconnect, 0);
    CHECK_EQ(s.codecs_off, 0);
    CHECK(strcmp(s.name, "modesp satellite") == 0);
    CHECK_EQ(bt_codecs_enabled(&s, ALL), ALL);
    CHECK_EQ(bt_codecs_enabled(&s, SBC | AAC), SBC | AAC);
    CHECK_EQ(bt_codecs_enabled(&s, 0), SBC);            /* SBC is not the build's to lose */

    bt_settings_defaults(&s, "a name that is a good deal longer than thirty-two bytes");
    CHECK_EQ(strlen(s.name), 32);
}

static void test_utf8(void)
{
    CHECK(bt_utf8_valid("plain", 5));
    CHECK(bt_utf8_valid("\xC3\xA9", 2));                /* é */
    CHECK(bt_utf8_valid("\xE2\x82\xAC", 3));            /* € */
    CHECK(bt_utf8_valid("\xF0\x9F\x8E\xB5", 4));        /* a musical note */
    CHECK(bt_utf8_valid("\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F", 10));   /* Кухня */
    CHECK(!bt_utf8_valid("\x80", 1));                   /* a stray continuation */
    CHECK(!bt_utf8_valid("\xC3", 1));                   /* cut short */
    CHECK(!bt_utf8_valid("\xE2\x82", 2));
    CHECK(!bt_utf8_valid("\xC0\x80", 2));               /* overlong NUL */
    CHECK(!bt_utf8_valid("\xE0\x80\x80", 3));           /* overlong */
    CHECK(!bt_utf8_valid("\xED\xA0\x80", 3));           /* a surrogate */
    CHECK(!bt_utf8_valid("\xF4\x90\x80\x80", 4));       /* past U+10FFFF */
    CHECK(!bt_utf8_valid("\xF8\x88\x80\x80\x80", 5));
    CHECK(!bt_utf8_valid("a\0b", 3));                   /* NUL inside */

    CHECK(bt_name_ok("x", 1));
    CHECK(!bt_name_ok("", 0));
    CHECK(bt_name_ok("0123456789abcdef0123456789abcdef", 32));
    CHECK(!bt_name_ok("0123456789abcdef0123456789abcdefX", 33));
    CHECK(!bt_name_ok("\xC3", 1));

    CHECK_EQ(bt_utf8_clip("abc", 3, 2), 2);
    CHECK_EQ(bt_utf8_clip("a\xC3\xA9", 3, 2), 1);       /* never half an é */
    CHECK_EQ(bt_utf8_clip("a\xC3\xA9", 3, 3), 3);
    CHECK_EQ(bt_utf8_clip("\xE2\x82\xAC\xE2\x82\xAC", 6, 5), 3);
}

static void test_apply_set(void)
{
    bt_settings_t s;
    bt_settings_defaults(&s, "modesp satellite");

    /* The same state again changes nothing. */
    CHECK_EQ(bt_settings_apply(&s, ALL, 1, 1, 0, ALL, NULL, 0), 0);

    /* Flags: anything but zero is yes. */
    unsigned c = bt_settings_apply(&s, ALL, 7, 0, 200, ALL, NULL, 0);
    CHECK_EQ(c, BT_CHANGED_DISCOVERABLE | BT_CHANGED_RECONNECT);
    CHECK_EQ(s.radio, 1);
    CHECK_EQ(s.discoverable, 0);
    CHECK_EQ(s.auto_reconnect, 1);

    /* LHDC off; SBC switched off on the wire is put back. */
    c = bt_settings_apply(&s, ALL, 1, 0, 1, AAC | APTX | APTXH, NULL, 0);
    CHECK_EQ(c, BT_CHANGED_CODECS);
    CHECK_EQ(s.codecs_off, LHDC);
    CHECK_EQ(bt_codecs_enabled(&s, ALL), SBC | AAC | APTX | APTXH);
    CHECK(!(s.codecs_off & SBC));

    /* A build without LHDC keeps the choice about it, whatever it is sent. */
    c = bt_settings_apply(&s, SBC | AAC | APTX | APTXH, 1, 0, 1, ALL, NULL, 0);
    CHECK_EQ(c, 0);
    CHECK_EQ(s.codecs_off, LHDC);
    /* ...and a build that has it again finds it still off. */
    CHECK_EQ(bt_codecs_enabled(&s, ALL), SBC | AAC | APTX | APTXH);

    /* A codec this setting never mentioned is on. */
    bt_settings_t fresh;
    bt_settings_defaults(&fresh, "x");
    bt_settings_apply(&fresh, SBC | AAC, 1, 1, 0, SBC | AAC, NULL, 0);
    CHECK_EQ(bt_codecs_enabled(&fresh, ALL), ALL);

    /* Names: empty keeps, too long and not UTF-8 keep, a good one replaces. */
    CHECK_EQ(bt_settings_apply(&s, ALL, 1, 0, 1, SBC | AAC | APTX | APTXH, "", 0), 0);
    CHECK_EQ(bt_settings_apply(&s, ALL, 1, 0, 1, SBC | AAC | APTX | APTXH,
                               "0123456789abcdef0123456789abcdefX", 33), 0);
    CHECK_EQ(bt_settings_apply(&s, ALL, 1, 0, 1, SBC | AAC | APTX | APTXH, "\xC3", 1), 0);
    CHECK(strcmp(s.name, "modesp satellite") == 0);
    c = bt_settings_apply(&s, ALL, 1, 0, 1, SBC | AAC | APTX | APTXH,
                          "\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F", 10);
    CHECK_EQ(c, BT_CHANGED_NAME);
    CHECK(strcmp(s.name, "\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F") == 0);

    c = bt_settings_apply(&s, ALL, 0, 0, 1, SBC | AAC | APTX | APTXH, "Kitchen", 7);
    CHECK_EQ(c, BT_CHANGED_RADIO | BT_CHANGED_NAME);
}

static void test_settings_blob(void)
{
    bt_settings_t s, back;
    bt_settings_defaults(&s, "modesp satellite");
    bt_settings_apply(&s, ALL, 1, 0, 1, SBC | AAC, "Kitchen", 7);

    uint8_t blob[BT_SETTINGS_BLOB_MAX];
    size_t len = bt_settings_pack(&s, blob, sizeof(blob));
    CHECK_EQ(len, 6 + 7);
    bt_settings_defaults(&back, "other");
    CHECK(bt_settings_unpack(&back, blob, len));
    CHECK_EQ(back.radio, 1);
    CHECK_EQ(back.discoverable, 0);
    CHECK_EQ(back.auto_reconnect, 1);
    CHECK_EQ(back.codecs_off, APTX | APTXH | LHDC);
    CHECK(strcmp(back.name, "Kitchen") == 0);

    uint8_t bad[BT_SETTINGS_BLOB_MAX];
    memcpy(bad, blob, len); bad[0] = 2;                     /* not our version */
    CHECK(!bt_settings_unpack(&back, bad, len));
    CHECK(!bt_settings_unpack(&back, blob, len - 1));       /* cut */
    memcpy(bad, blob, len); bad[1] = 2;                     /* a flag that is not a flag */
    CHECK(!bt_settings_unpack(&back, bad, len));
    memcpy(bad, blob, len); bad[4] |= SBC;                  /* SBC can never be off */
    CHECK(!bt_settings_unpack(&back, bad, len));
    memcpy(bad, blob, len); bad[6] = 0x80;                  /* the name is not text */
    CHECK(!bt_settings_unpack(&back, bad, len));
    static const uint8_t no_name[] = { 1, 1, 1, 0, 0, 0 };
    CHECK(!bt_settings_unpack(&back, no_name, sizeof(no_name)));
    CHECK(strcmp(back.name, "Kitchen") == 0);               /* a refusal leaves it alone */
}

/* ---------------------------------------------------------------------- bonds */

static void test_bonds_follow_the_stack(void)
{
    static bond_table_t t;
    memset(&t, 0, sizeof(t));
    const uint8_t two[2][6] = { { 0xDC, 0xE5, 0x5B, 0xEA, 0xC9, 0x3B },
                                { 0x34, 0xF3, 0x9A, 0xF1, 0xC4, 0x98 } };
    CHECK(bond_sync(&t, two, 2));
    CHECK_EQ(t.count, 2);
    CHECK(!bond_sync(&t, two, 2));                          /* nothing new */
    CHECK_EQ(bond_last(&t), -1);                            /* nobody has connected yet */

    CHECK(bond_touch(&t, B));
    CHECK(bond_touch(&t, A));
    uint8_t order[BOND_TABLE_MAX];
    CHECK_EQ(bond_order(&t, order), 2);
    CHECK(memcmp(t.e[order[0]].addr, A, 6) == 0);           /* the most recent first */
    CHECK(memcmp(t.e[order[1]].addr, B, 6) == 0);
    CHECK(memcmp(t.e[bond_last(&t)].addr, A, 6) == 0);

    /* The stack forgot B, learnt C. */
    const uint8_t now_[2][6] = { { 0xDC, 0xE5, 0x5B, 0xEA, 0xC9, 0x3B },
                                 { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
    CHECK(bond_sync(&t, now_, 2));
    CHECK_EQ(bond_index(&t, B), -1);
    CHECK(bond_index(&t, C) >= 0);
    CHECK_EQ(t.e[bond_index(&t, C)].seq, 0);
    CHECK_EQ(bond_order(&t, order), 2);
    CHECK(memcmp(t.e[order[0]].addr, A, 6) == 0);           /* never connected goes last */

    /* Names: clipped on a character, not text refused, the same one no change. */
    CHECK(bond_set_name(&t, A, "Pixel 7", 7));
    CHECK(!bond_set_name(&t, A, "Pixel 7", 7));
    CHECK(!bond_set_name(&t, A, "\x80", 1));
    CHECK(!bond_set_name(&t, A, "", 0));
    CHECK(strcmp(t.e[bond_index(&t, A)].name, "Pixel 7") == 0);
    char longname[48];
    memset(longname, 'a', 31);
    memcpy(longname + 31, "\xC3\xA9xyz", 5);                /* é across byte 32 */
    CHECK(bond_set_name(&t, C, longname, 36));
    CHECK_EQ(strlen(t.e[bond_index(&t, C)].name), 31);

    CHECK(bond_remove(&t, C));
    CHECK(!bond_remove(&t, C));
    CHECK_EQ(t.count, 1);
}

/* A pairing names the phone in BT_STATE only when it is that phone, attached. */
static void test_names_attached(void)
{
    CHECK(bond_names_attached(SATLINK_BT_CONNECTED, A, A));
    CHECK(bond_names_attached(SATLINK_BT_STREAMING, A, A));
    CHECK(!bond_names_attached(SATLINK_BT_STREAMING, A, B));    /* B pairs while A plays */
    CHECK(!bond_names_attached(SATLINK_BT_CONNECTED, A, B));
    CHECK(!bond_names_attached(SATLINK_BT_CONNECTING, A, A));
    static const uint8_t none[6] = { 0 };
    CHECK(!bond_names_attached(SATLINK_BT_DISCOVERABLE, none, B));  /* pairs, never connects */
    CHECK(!bond_names_attached(SATLINK_BT_IDLE, none, none));

    /* The name it brings, as the stack hands it over: 40 Cyrillic letters, 80 bytes,
     * kept to 32 on a character boundary and still text. */
    char raw[81];
    for (int i = 0; i < 40; i++) {
        raw[2 * i] = (char)0xD0;
        raw[2 * i + 1] = (char)0x90;                        /* А */
    }
    raw[80] = '\0';
    size_t len = bt_utf8_clip(raw, strlen(raw), SATLINK_BT_NAME_MAX);
    CHECK_EQ(len, 32);
    CHECK(bt_utf8_valid(raw, len));
    CHECK_EQ(bt_utf8_clip(raw, 80, 63), 62);                /* never half a letter */
}

static void test_bonds_full_and_saved(void)
{
    static bond_table_t t, back;
    memset(&t, 0, sizeof(t));
    uint8_t many[25][6];
    for (int i = 0; i < 25; i++) {
        memset(many[i], 0, 6);
        many[i][5] = (uint8_t)i;
    }
    bond_sync(&t, (const uint8_t (*)[6])many, 25);
    CHECK_EQ(t.count, BOND_TABLE_MAX);                      /* the rest do not fit */
    CHECK_EQ(bond_add(&t, A), -1);
    CHECK(!bond_touch(&t, A));

    for (int i = 0; i < 5; i++) {
        bond_touch(&t, many[i]);
    }
    bond_set_name(&t, many[3], "Tablet", 6);

    uint8_t blob[BOND_TABLE_BLOB_MAX];
    size_t len = bond_table_pack(&t, blob, sizeof(blob));
    CHECK(len > 6);
    memset(&back, 0, sizeof(back));
    CHECK(bond_table_unpack(&back, blob, len));
    CHECK_EQ(back.count, t.count);
    CHECK_EQ(back.next_seq, t.next_seq);
    CHECK(memcmp(back.e[bond_last(&back)].addr, many[4], 6) == 0);
    CHECK(strcmp(back.e[bond_index(&back, many[3])].name, "Tablet") == 0);

    CHECK(!bond_table_unpack(&back, blob, len - 1));
    blob[0] = 9;
    CHECK(!bond_table_unpack(&back, blob, len));
    blob[0] = BOND_TABLE_BLOB_VERSION;
    bond_put32(blob + 2, 1);                                /* a seq above the counter */
    CHECK(!bond_table_unpack(&back, blob, len));
    CHECK_EQ(back.count, t.count);                          /* a refusal leaves it alone */

    /* The counter wrapping keeps the order it had. */
    t.next_seq = UINT32_MAX;
    CHECK(bond_touch(&t, many[0]));
    uint8_t order[BOND_TABLE_MAX];
    bond_order(&t, order);
    CHECK(memcmp(t.e[order[0]].addr, many[0], 6) == 0);
    CHECK(memcmp(t.e[order[1]].addr, many[4], 6) == 0);
    CHECK(t.next_seq < 100);
}

/* -------------------------------------------------------------------- pairing */

static void test_pair_without_host(void)
{
    pair_fsm_t f;
    memset(&f, 0, sizeof(f));
    pair_out_t o = pair_on_confirm(&f, A, 123456, false, "Pixel", false, 0);
    CHECK(o.reply && o.accept && !o.ask && !o.result);      /* 1.7: yes, and nobody asked */
    CHECK_EQ(f.phase, PAIR_IDLE);
    o = pair_on_passkey(&f, A, 42, "Pixel", false, 0);
    CHECK(!o.ask && !o.reply);
    o = pair_on_done(&f, A, true);                          /* nothing was asked, nothing to report */
    CHECK(!o.result);
}

static void test_pair_numeric_yes(void)
{
    pair_fsm_t f;
    memset(&f, 0, sizeof(f));
    pair_out_t o = pair_on_confirm(&f, A, 123456, false, "Pixel", true, 1000);
    CHECK(o.ask && !o.reply);
    CHECK_EQ(f.phase, PAIR_ASKING);
    CHECK_EQ(f.kind, SATLINK_PAIR_NUMERIC);
    CHECK_EQ(f.passkey, 123456);
    CHECK(strcmp(f.name, "Pixel") == 0);
    CHECK(pair_should_repeat(&f));

    o = pair_on_reply(&f, B, true, 2000);                   /* somebody else's answer */
    CHECK(!o.reply && !o.result);
    o = pair_on_reply(&f, A, true, 2000);
    CHECK(o.reply && o.accept && !o.result);
    CHECK_EQ(f.phase, PAIR_WAITING);
    CHECK(!pair_should_repeat(&f));                         /* nothing left for the host */
    o = pair_on_reply(&f, A, false, 2100);                  /* a second answer is too late */
    CHECK(!o.reply && !o.result);

    o = pair_on_done(&f, A, true);
    CHECK(o.result);
    CHECK_EQ(o.status, SATLINK_PAIR_OK);
    CHECK_EQ(f.phase, PAIR_IDLE);
}

static void test_pair_just_works_and_no(void)
{
    pair_fsm_t f;
    memset(&f, 0, sizeof(f));
    pair_out_t o = pair_on_confirm(&f, A, 777777, true, "", true, 0);
    CHECK(o.ask);
    CHECK_EQ(f.kind, SATLINK_PAIR_CONSENT);
    CHECK_EQ(f.passkey, 0);                                 /* no code on a consent */

    o = pair_on_reply(&f, A, false, 500);
    CHECK(o.reply && !o.accept);
    CHECK(o.result);
    CHECK_EQ(o.status, SATLINK_PAIR_REJECTED);
    CHECK_EQ(f.phase, PAIR_IDLE);
    o = pair_on_done(&f, A, false);                         /* the stack's echo of our no */
    CHECK(!o.result);
}

static void test_pair_timeout_and_cancel(void)
{
    pair_fsm_t f;
    memset(&f, 0, sizeof(f));
    pair_on_confirm(&f, A, 1, false, "Pixel", true, 0xFFFFF000u);      /* across the wrap */
    pair_out_t o = pair_on_tick(&f, 0xFFFFF000u + PAIR_REPLY_TIMEOUT_MS - 1);
    CHECK(!o.reply && !o.result);
    o = pair_on_tick(&f, 0xFFFFF000u + PAIR_REPLY_TIMEOUT_MS);
    CHECK(o.reply && !o.accept);
    CHECK(o.result);
    CHECK_EQ(o.status, SATLINK_PAIR_TIMEOUT);
    CHECK(memcmp(o.addr, A, 6) == 0);
    o = pair_on_reply(&f, A, true, 0);                      /* too late: ignored */
    CHECK(!o.reply);

    /* The phone gives up while the host is still deciding. */
    pair_on_confirm(&f, A, 2, false, "Pixel", true, 0);
    o = pair_on_done(&f, A, false);
    CHECK(o.result);
    CHECK_EQ(o.status, SATLINK_PAIR_FAILED);
    o = pair_on_reply(&f, A, true, 10);
    CHECK(!o.reply);

    /* A yes the stack then never finishes still ends in a result. */
    pair_on_confirm(&f, A, 3, false, "Pixel", true, 0);
    pair_on_reply(&f, A, true, 100);
    o = pair_on_tick(&f, 100 + PAIR_RESULT_TIMEOUT_MS);
    CHECK(o.result && !o.reply);
    CHECK_EQ(o.status, SATLINK_PAIR_FAILED);
}

static void test_pair_one_at_a_time(void)
{
    pair_fsm_t f;
    memset(&f, 0, sizeof(f));
    pair_on_confirm(&f, A, 111111, false, "Pixel", true, 0);
    pair_out_t o = pair_on_confirm(&f, B, 222222, false, "Laptop", true, 10);
    CHECK(o.reply && !o.accept && !o.ask);                  /* the second is refused */
    CHECK(memcmp(o.addr, B, 6) == 0);
    CHECK(memcmp(f.addr, A, 6) == 0);                       /* the first is untouched */
    CHECK_EQ(f.passkey, 111111);
    o = pair_on_done(&f, B, false);                         /* its failure is not ours */
    CHECK(!o.result);
    CHECK_EQ(f.phase, PAIR_ASKING);
}

static void test_pair_passkey(void)
{
    pair_fsm_t f;
    memset(&f, 0, sizeof(f));
    pair_out_t o = pair_on_passkey(&f, C, 42, "Keyboard", true, 0);
    CHECK(o.ask && !o.reply);
    CHECK_EQ(f.kind, SATLINK_PAIR_PASSKEY);
    CHECK_EQ(f.passkey, 42);
    CHECK(pair_should_repeat(&f));                          /* the code stays on screen */
    o = pair_on_reply(&f, C, true, 5);                      /* nothing to answer */
    CHECK(!o.reply);
    o = pair_on_tick(&f, PAIR_REPLY_TIMEOUT_MS);            /* no 25 s limit on typing */
    CHECK(!o.result);
    o = pair_on_done(&f, C, true);
    CHECK(o.result);
    CHECK_EQ(o.status, SATLINK_PAIR_OK);

    pair_on_passkey(&f, C, 43, "Keyboard", true, 0);
    o = pair_on_tick(&f, PAIR_RESULT_TIMEOUT_MS);
    CHECK(o.result);
    CHECK_EQ(o.status, SATLINK_PAIR_FAILED);
}

/* ---------------------------------------------------------------- reconnect */

static void test_reconnect(void)
{
    rc_fsm_t f = {0};
    CHECK(!rc_due(&f, 0, true));                            /* never armed */
    rc_arm(&f, 1000);
    CHECK(!rc_due(&f, 1000, false));                        /* not now: a phone is here */
    CHECK(rc_due(&f, 1000, true));
    CHECK(!rc_due(&f, 1000 + RC_INTERVAL_MS - 1, true));
    CHECK(rc_due(&f, 1000 + RC_INTERVAL_MS, true));
    CHECK(rc_due(&f, 1000 + 2 * RC_INTERVAL_MS, true));
    CHECK(!rc_due(&f, 1000 + 3 * RC_INTERVAL_MS, true));    /* three, and no more */

    rc_arm(&f, 50000);
    CHECK(rc_due(&f, 50000, true));
    rc_stop(&f);                                            /* somebody connected */
    CHECK(!rc_due(&f, 70000, true));
}

int main(void)
{
    test_defaults();
    test_utf8();
    test_apply_set();
    test_settings_blob();
    test_bonds_follow_the_stack();
    test_names_attached();
    test_bonds_full_and_saved();
    test_pair_without_host();
    test_pair_numeric_yes();
    test_pair_just_works_and_no();
    test_pair_timeout_and_cancel();
    test_pair_one_at_a_time();
    test_pair_passkey();
    test_reconnect();
    printf("Bluetooth settings (1.8): %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
