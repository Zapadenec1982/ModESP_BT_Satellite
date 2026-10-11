/* The vendored Helix AAC decoder against malformed raw_data_blocks, the way
 * aac_decoder_helix.c drives it: a static decoder buffer, raw-block mode.
 * Every frame here is a few hand-built header fields, not a real stream; the
 * point is the guards on max_sfb, section, pulse, TNS and gain control data
 * and over-reads. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "aacdec.h"
#include "coder.h"

#define CANARY 0xa5

/* aac_decoder_helix.c's pool, with a guard band behind it for builds without ASan. */
static struct {
    uint8_t pool[sizeof(AACDecInfo) + sizeof(PSInfoBase) + 64];
    uint8_t guard[4096];
} s_mem __attribute__((aligned(8)));
static short s_pcm[2 * 1024];

typedef struct { uint8_t data[1200]; unsigned bits; } frame_t;

static void put(frame_t *f, unsigned value, unsigned n) {
    while (n--) {
        assert(f->bits < 8 * sizeof(f->data));
        if (value >> n & 1) f->data[f->bits >> 3] |= (uint8_t)(0x80 >> (f->bits & 7));
        f->bits++;
    }
}

static void begin(frame_t *f, unsigned id) {
    memset(f, 0, sizeof(*f));
    put(f, id, 3);  /* id_syn_ele */
    put(f, 0, 4);   /* element_instance_tag */
}

/* global_gain and a long-window ics_info. */
static void ics_long(frame_t *f, unsigned max_sfb) {
    put(f, 100, 8);
    put(f, 0, 1); put(f, 0, 2); put(f, 0, 1);  /* reserved, ONLY_LONG, window shape */
    put(f, max_sfb, 6);
    put(f, 0, 1);                              /* predictor_data_present */
}

/* One section of codebook cb, sect_len coded in 5-bit escapes. */
static void section_long(frame_t *f, unsigned cb, unsigned len) {
    put(f, cb, 4);
    for (; len >= 31; len -= 31) put(f, 31, 5);
    put(f, len, 5);
}

/* No pulse, TNS or gain control data, then ID_END. */
static void finish(frame_t *f) {
    put(f, 0, 3);
    put(f, AAC_ID_END, 3);
}

static int decode(const frame_t *f, int channels) {
    HAACDecoder dec;
    AACFrameInfo info;
    unsigned char *in = (unsigned char *)f->data;
    int left = (int)((f->bits + 7) / 8);

    memset(&s_mem, 0, sizeof(s_mem));
    memset(s_mem.guard, CANARY, sizeof(s_mem.guard));
    dec = AACInitDecoderPre(s_mem.pool, (int)sizeof(s_mem.pool));
    assert(dec);
    memset(&info, 0, sizeof(info));
    info.nChans = channels;
    info.sampRateCore = 44100;
    info.profile = AAC_PROFILE_LC;
    assert(AACSetRawBlockParams(dec, 0, &info) == ERR_AAC_NONE);

    int err = AACDecode(dec, &in, &left, s_pcm);
    for (size_t i = 0; i < sizeof(s_mem.guard); i++) {
        if (s_mem.guard[i] != CANARY) {
            fprintf(stderr, "test_aac: decoder wrote %zu bytes past its buffer\n", i + 1);
            assert(!"guard band overwritten");
        }
    }
    return err;
}

int main(void) {
    frame_t f;

    /* Sanity: a silent mono frame and a full-band one decode. */
    begin(&f, AAC_ID_SCE); ics_long(&f, 0); finish(&f);
    assert(decode(&f, 1) == ERR_AAC_NONE);
    begin(&f, AAC_ID_SCE); ics_long(&f, 49); section_long(&f, 0, 49); finish(&f);
    assert(decode(&f, 1) == ERR_AAC_NONE);

    /* 44.1 kHz has 49 long bands: max_sfb 50..63 indexes past the band tables. */
    begin(&f, AAC_ID_SCE); ics_long(&f, 50); section_long(&f, 0, 50); finish(&f);
    assert(decode(&f, 1) < 0);
    begin(&f, AAC_ID_SCE); ics_long(&f, 63); section_long(&f, 0, 63); finish(&f);
    assert(decode(&f, 1) < 0);

    /* And 14 short ones; all eight windows in one group. */
    for (unsigned max_sfb = 14; max_sfb <= 15; max_sfb++) {
        begin(&f, AAC_ID_SCE);
        put(&f, 100, 8);
        put(&f, 0, 1); put(&f, 2, 2); put(&f, 0, 1);  /* EIGHT_SHORT_SEQUENCE */
        put(&f, max_sfb, 4); put(&f, 0x7f, 7);        /* max_sfb, scale_factor_grouping */
        put(&f, 0, 4);
        for (unsigned len = max_sfb; ; len -= 7) { put(&f, len < 7 ? len : 7, 3); if (len < 7) break; }
        finish(&f);
        assert((decode(&f, 1) == ERR_AAC_NONE) == (max_sfb == 14));
    }

    /* A section far longer than max_sfb: before the fix this wrote ~49 KB of
     * codebook numbers through sfbCodeBook[0][120]. */
    begin(&f, AAC_ID_SCE); ics_long(&f, 49);
    put(&f, 1, 4);
    while (f.bits + 5 <= 8 * 1000) put(&f, 31, 5);
    assert(decode(&f, 1) < 0);

    /* A frame cut inside section data: GetBits() yields zeros, i.e. empty
     * sections, and before the fix the section loop never ended. */
    begin(&f, AAC_ID_SCE); ics_long(&f, 49); section_long(&f, 1, 3);
    assert(decode(&f, 1) < 0);

    /* A pair whose first channel reads past the frame: the second channel
     * must not start a bitstream reader with a negative byte count. */
    begin(&f, AAC_ID_CPE);
    put(&f, 0, 1);  /* common_window */
    ics_long(&f, 49); section_long(&f, 1, 49);
    assert(decode(&f, 2) < 0);

    /* Pulses: start band 48 is at 928, so offsets of 31 reach 959 (fine) or
     * 1052 (past coef[]); start band 49 is past the table. */
    for (unsigned pulses = 1; pulses <= 4; pulses += 3) {
        begin(&f, AAC_ID_SCE); ics_long(&f, 0);
        put(&f, 1, 1); put(&f, pulses - 1, 2); put(&f, 48, 6);
        for (unsigned i = 0; i < pulses; i++) { put(&f, 31, 5); put(&f, 1, 4); }
        put(&f, 0, 2); put(&f, AAC_ID_END, 3);
        assert((decode(&f, 1) == ERR_AAC_NONE) == (pulses == 1));
    }
    begin(&f, AAC_ID_SCE); ics_long(&f, 0);
    put(&f, 1, 1); put(&f, 0, 2); put(&f, 49, 6); put(&f, 0, 5); put(&f, 1, 4);
    put(&f, 0, 2); put(&f, AAC_ID_END, 3);
    assert(decode(&f, 1) < 0);

    /* TNS, long window: three filters of order 20 fill coef[60] exactly; the
     * 5-bit order field allows 31, i.e. 93 coefficients into coef[60]. */
    for (unsigned order = 20; order <= 31; order += 11) {
        begin(&f, AAC_ID_SCE); ics_long(&f, 0);
        put(&f, 0, 1);                       /* pulse_data_present */
        put(&f, 1, 1); put(&f, 3, 2); put(&f, 1, 1);  /* tns: n_filt 3, coef_res */
        for (unsigned i = 0; i < 3; i++) {
            put(&f, 0, 6); put(&f, order, 5); put(&f, 0, 1); put(&f, 0, 1);
            for (unsigned k = 0; k < order; k++) put(&f, 0, 4);
        }
        put(&f, 0, 1); put(&f, AAC_ID_END, 3);
        assert((decode(&f, 1) == ERR_AAC_NONE) == (order == 20));
    }

    /* Gain control is SSR only. With max_band 3 Helix wrote row 3 of the
     * [MAX_GAIN_BANDS = 3] arrays, 8 windows deep for a short block. */
    begin(&f, AAC_ID_SCE);
    put(&f, 100, 8);
    put(&f, 0, 1); put(&f, 2, 2); put(&f, 0, 1);  /* EIGHT_SHORT_SEQUENCE */
    put(&f, 0, 4); put(&f, 0x7f, 7);              /* max_sfb 0, one group */
    put(&f, 0, 1); put(&f, 0, 1);                 /* no pulse (unused), no TNS */
    put(&f, 1, 1); put(&f, 3, 2);                 /* gain_control_data, max_band 3 */
    for (unsigned i = 0; i < 3 * 8; i++) put(&f, 0, 3);
    put(&f, AAC_ID_END, 3);
    assert(decode(&f, 1) < 0);

    puts("test_aac: all passed");
    return 0;
}
