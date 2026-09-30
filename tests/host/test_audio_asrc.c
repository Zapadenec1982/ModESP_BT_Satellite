/*
 * The slave-mode converter (main/audio_asrc.c) on the host: bit-exact at a ratio of one,
 * within the generator's error bound off it, and the same output however the input and
 * the output room are cut up, because the writer hands it whatever the ring returns.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_asrc.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_checks, g_failures;

#define CHECK(cond)                                                             \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                       \
    } while (0)

#define HALF (AUDIO_ASRC_TAPS / 2u)

static void put(uint8_t *p, int32_t v)
{
    const uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); p[2] = (uint8_t)(u >> 16); p[3] = (uint8_t)(u >> 24);
}

/* A 24-bit sample in a 32-bit slot, left-justified: what the wire carries. */
static int32_t slot24(double v)
{
    double s = floor(v * 8388608.0 + 0.5);
    if (s >  8388607.0) s =  8388607.0;
    if (s < -8388608.0) s = -8388608.0;
    return (int32_t)((uint32_t)(int32_t)s << 8);
}

/* Everything through, `chunk` input frames and `cap` output frames at a time. */
static size_t convert(audio_asrc_t *a, const uint8_t *in, size_t frames, int32_t *out,
                      size_t out_max, size_t chunk, size_t cap)
{
    size_t made = 0, off = 0;
    while (off < frames) {
        size_t n = frames - off < chunk ? frames - off : chunk;
        while (n) {
            size_t room = out_max - made < cap ? out_max - made : cap;
            size_t used = 0;
            made += audio_asrc_run(a, in + off * 8u, n, out + 2u * made, room, &used);
            off += used;
            n -= used;
            if (used == 0 && room == 0) return made;
        }
    }
    /* A call that fills its room may keep input it has not turned into output yet; the
     * writer gets that out on its next pass, this gets it out now. */
    for (;;) {
        size_t used = 0;
        const size_t k = audio_asrc_run(a, in, 0, out + 2u * made, out_max - made, &used);
        if (k == 0) break;
        made += k;
    }
    return made;
}

static uint8_t *wire(size_t frames)
{
    uint8_t *w = malloc(frames * 8u + 1u);
    if (!w) { printf("out of memory\n"); exit(2); }
    return w;
}

static uint32_t g_rng = 12345u;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

static void test_exact_at_one(void)
{
    enum { N = 5000 };
    uint8_t *in = wire(N);
    int32_t ref[2 * N];
    for (size_t i = 0; i < 2u * N; i++) {
        ref[i] = (int32_t)((rnd() & 0xFFFFFF00u));      /* any 24-bit value, sign included */
        put(in + 4u * i, ref[i]);
    }
    static int32_t out[2 * N];
    audio_asrc_t a;
    memset(&a, 0, sizeof a);
    audio_asrc_set_ppm(&a, 0.0f);
    audio_asrc_reset(&a);
    size_t made = convert(&a, in, N, out, N, 97, 1000);
    /* Every frame but the last HALF, whose right half has not arrived yet. */
    CHECK(made == N - HALF);
    int same = 1;
    for (size_t i = 0; i < 2u * made; i++) same &= out[i] == ref[i];
    CHECK(same);
    free(in);
}

static double worst_db(double hz, float ppm)
{
    enum { N = 40000 };
    const double fs = 96000.0, amp = 0.5;
    uint8_t *in = wire(N);
    for (size_t i = 0; i < N; i++) {
        const int32_t v = slot24(amp * sin(2.0 * M_PI * hz * (double)i / fs));
        put(in + 8u * i, v);
        put(in + 8u * i + 4u, (int32_t)((uint32_t)(-(v >> 8)) << 8));   /* R = -L */
    }
    static int32_t out[2 * N];
    audio_asrc_t a;
    memset(&a, 0, sizeof a);
    audio_asrc_set_ppm(&a, ppm);
    audio_asrc_reset(&a);
    const size_t made = convert(&a, in, N, out, N, 64, 128);
    /* Output n reads the input at n * (1 + step / 2^32), exactly: the phase is fixed-point. */
    const double r = 1.0 + (double)a.step / 4294967296.0;
    double worst = 0.0;
    for (size_t n = AUDIO_ASRC_TAPS; n < made; n++) {
        const double t = (double)n * r;
        if (t > (double)(N - AUDIO_ASRC_TAPS)) break;
        const double ideal = amp * sin(2.0 * M_PI * hz * t / fs);
        const double l = (double)out[2u * n] / 2147483648.0;
        const double rr = (double)out[2u * n + 1u] / 2147483648.0;
        const double e = fmax(fabs(l - ideal), fabs(rr + ideal));
        if (e > worst) worst = e;
    }
    free(in);
    return 20.0 * log10(worst / amp + 1e-30);
}

static void test_accuracy(void)
{
    static const double hz[] = { 1000.0, 10000.0, 20000.0 };
    static const float ppm[] = { 250.0f, -250.0f, 37.5f };
    for (size_t i = 0; i < sizeof hz / sizeof hz[0]; i++) {
        for (size_t j = 0; j < sizeof ppm / sizeof ppm[0]; j++) {
            const double db = worst_db(hz[i], ppm[j]);
            printf("  %5.0f Hz at %+7.1f ppm: worst error %.1f dB\n", hz[i], ppm[j], db);
            CHECK(db < -105.0);
        }
    }
}

static void test_cuts_do_not_matter(void)
{
    enum { N = 3000 };
    uint8_t *in = wire(N);
    for (size_t i = 0; i < 2u * N; i++) put(in + 4u * i, (int32_t)(rnd() & 0xFFFFFF00u));
    static int32_t ref[2 * N], out[2 * N];
    audio_asrc_t a;
    memset(&a, 0, sizeof a);
    audio_asrc_set_ppm(&a, 313.0f);
    audio_asrc_reset(&a);
    const size_t made = convert(&a, in, N, ref, N, N, N);
    static const size_t chunks[] = { 1, 7, 64, 65, 129 };
    static const size_t caps[] = { 1, 5, 66, 1000 };
    for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        for (size_t j = 0; j < sizeof caps / sizeof caps[0]; j++) {
            audio_asrc_reset(&a);
            const size_t m = convert(&a, in, N, out, N, chunks[i], caps[j]);
            CHECK(m == made);
            CHECK(memcmp(out, ref, m * 8u) == 0);
        }
    }
    /* Any alignment: the ring hands back byte offsets. */
    uint8_t *odd = wire(N);
    memcpy(odd + 1, in, (size_t)N * 8u);
    audio_asrc_reset(&a);
    const size_t m = convert(&a, odd + 1, N, out, N, 64, 66);
    CHECK(m == made);
    CHECK(memcmp(out, ref, m * 8u) == 0);
    free(odd);
    free(in);
}

static void test_rate(void)
{
    enum { N = 200000 };
    uint8_t *in = wire(N);
    memset(in, 0, (size_t)N * 8u);
    static int32_t out[2 * (N + N / 100)];
    static const float ppm[] = { 500.0f, -500.0f, 0.25f };
    for (size_t i = 0; i < sizeof ppm / sizeof ppm[0]; i++) {
        audio_asrc_t a;
        memset(&a, 0, sizeof a);
        audio_asrc_set_ppm(&a, ppm[i]);
        audio_asrc_reset(&a);
        const size_t made = convert(&a, in, N, out, N + N / 100, 64, 66);
        /* Output n reads position n * (2^32 + step) in Q32, and is made once its right half
         * is in: while that position is below N - HALF frames. Exact integer count. */
        const int64_t per = ((int64_t)1 << 32) + a.step;
        const int64_t limit = (int64_t)(N - HALF) << 32;
        const int64_t expect = (limit + per - 1) / per;
        printf("  %+7.2f ppm: %zu in, %zu out, %lld expected\n", ppm[i], (size_t)N, made,
               (long long)expect);
        CHECK((int64_t)made == expect);
    }
    free(in);
}

static void test_reset_and_clamp(void)
{
    enum { N = 400 };
    uint8_t *in = wire(N);
    static int32_t out[2 * N];
    audio_asrc_t a;
    memset(&a, 0, sizeof a);
    audio_asrc_set_ppm(&a, 400.0f);
    CHECK(fabsf(audio_asrc_ppm(&a) - 400.0f) < 0.001f);
    /* Full-scale DC in both directions: the filter's overshoot must clamp, never wrap. */
    for (size_t i = 0; i < N; i++) {
        put(in + 8u * i, (int32_t)0x7FFFFF00);
        put(in + 8u * i + 4u, (int32_t)0x80000000u);
    }
    audio_asrc_reset(&a);
    size_t made = convert(&a, in, N, out, N, 64, 66);
    int ok = 1;
    for (size_t n = AUDIO_ASRC_TAPS; n < made; n++)
        ok &= out[2u * n] > 2147000000 && out[2u * n + 1u] < -2147000000;
    CHECK(ok);
    /* After a reset the history is silence again: an impulse comes out alone. */
    audio_asrc_set_ppm(&a, 0.0f);
    audio_asrc_reset(&a);
    memset(in, 0, (size_t)N * 8u);
    put(in + 8u * 3u, 1 << 20);
    made = convert(&a, in, N, out, N, 64, 66);
    ok = made == N - HALF;
    for (size_t n = 0; n < made; n++) ok &= out[2u * n] == (n == 3 ? (1 << 20) : 0) && out[2u * n + 1u] == 0;
    CHECK(ok);
    free(in);
}

int main(void)
{
    printf("audio_asrc:\n");
    test_exact_at_one();
    test_accuracy();
    test_cuts_do_not_matter();
    test_rate();
    test_reset_and_clamp();
    printf("audio_asrc: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
