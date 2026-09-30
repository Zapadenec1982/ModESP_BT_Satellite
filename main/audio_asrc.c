#include "audio_asrc.h"
#include "audio_asrc_table.h"

#include <string.h>

_Static_assert(AUDIO_ASRC_TABLE_TAPS == AUDIO_ASRC_TAPS, "regenerate audio_asrc_table.h");
_Static_assert((1u << AUDIO_ASRC_TABLE_PHASE_BITS) == AUDIO_ASRC_TABLE_PHASES,
               "the phase is the top bits of the fraction");

#define HALF        (AUDIO_ASRC_TAPS / 2u)
#define CAPACITY    (AUDIO_ASRC_TAPS - 1u + AUDIO_ASRC_BLOCK)
#define PHASE_SHIFT (32u - AUDIO_ASRC_TABLE_PHASE_BITS)
#define FRAC_MASK   ((1u << PHASE_SHIFT) - 1u)

#if defined(__GNUC__) && !defined(__clang__)
#define UNROLL _Pragma("GCC unroll 16")
#else
#define UNROLL
#endif

/* The last float below 2^31: the largest value that still converts to an int32. */
#define OUT_MAX     2147483520.0f
#define OUT_MIN     (-2147483648.0f)

void audio_asrc_reset(audio_asrc_t *a)
{
    const int32_t step = a->step;
    memset(a, 0, sizeof *a);
    a->step = step;
    /* HALF - 1 frames of silence ahead of the read position: the first sample to come is
     * the first one out, with the filter's left half reading the silence. */
    a->have = HALF - 1u;
    a->pos  = HALF - 1u;
}

void audio_asrc_set_ppm(audio_asrc_t *a, float ppm)
{
    if (ppm >  1000.0f) ppm =  1000.0f;
    if (ppm < -1000.0f) ppm = -1000.0f;
    const float q32 = ppm * 4294.967296f;      /* 2^32 / 1e6 */
    a->step = (int32_t)(q32 >= 0.0f ? q32 + 0.5f : q32 - 0.5f);
}

float audio_asrc_ppm(const audio_asrc_t *a)
{
    return (float)a->step / 4294.967296f;
}

static inline int32_t sample_at(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                     (uint32_t)p[3] << 24);
}

static inline int32_t to_int(float v)
{
    if (v > OUT_MAX) v = OUT_MAX;
    if (v < OUT_MIN) v = OUT_MIN;
    return (int32_t)v;
}

size_t audio_asrc_run(audio_asrc_t *a, const uint8_t *in, size_t in_frames,
                      int32_t *out, size_t out_cap, size_t *used)
{
    size_t made = 0, taken = 0;
    for (;;) {
        /* Every output whose right half is already in. */
        while (made < out_cap && a->pos + HALF < a->have) {
            const uint32_t phase = a->frac >> PHASE_SHIFT;
            const float f = (float)(a->frac & FRAC_MASK) * (1.0f / (float)(FRAC_MASK + 1u));
            const float *c0 = audio_asrc_table[phase];
            const float *c1 = audio_asrc_table[phase + 1u];
            const float *xp = &a->x[2u * (a->pos - (HALF - 1u))];
            /* Two passes and four sums, not one pass and two: every multiply-add in a single
             * sum waits for the one before it, and the FPU's latency is several cycles. */
            float c[AUDIO_ASRC_TAPS];
            UNROLL
            for (unsigned k = 0; k < AUDIO_ASRC_TAPS; k++) c[k] = c0[k] + f * (c1[k] - c0[k]);
            float l0 = 0.0f, l1 = 0.0f, r0 = 0.0f, r1 = 0.0f;
            UNROLL
            for (unsigned k = 0; k < AUDIO_ASRC_TAPS; k += 2u) {
                l0 += c[k]      * xp[2u * k];
                r0 += c[k]      * xp[2u * k + 1u];
                l1 += c[k + 1u] * xp[2u * k + 2u];
                r1 += c[k + 1u] * xp[2u * k + 3u];
            }
            out[2u * made]      = to_int(l0 + l1);
            out[2u * made + 1u] = to_int(r0 + r1);
            made++;
            /* Advance 1 + step / 2^32 input frames: 0, 1 or 2 whole ones. */
            const uint64_t t = (uint64_t)a->frac + (1ull << 32) + (uint64_t)(int64_t)a->step;
            a->pos  += (uint32_t)(t >> 32);
            a->frac  = (uint32_t)t;
        }
        if (made == out_cap) break;
        /* Drop what no output will read again, then take more in. */
        const uint32_t drop = a->pos - (HALF - 1u);
        if (drop != 0u) {
            memmove(a->x, &a->x[2u * drop], (size_t)(a->have - drop) * 2u * sizeof(float));
            a->have -= drop;
            a->pos  -= drop;
        }
        size_t n = in_frames - taken;
        if (n == 0u) break;
        if (n > CAPACITY - a->have) n = CAPACITY - a->have;
        const uint8_t *p = in + taken * 8u;
        float *dst = &a->x[2u * a->have];
        for (size_t i = 0; i < 2u * n; i++, p += 4) dst[i] = (float)sample_at(p);
        a->have += (uint32_t)n;
        taken   += n;
    }
    *used = taken;
    return made;
}
