/*
 * The slave-mode rate converter (protocol 1.9).
 *
 * While the host clocks the link, the rate at which samples leave this device is the
 * host's, and the rate at which the phone sends them is the phone's: two crystals, tens of
 * ppm apart. Before 1.9 the servo moved this device's APLL until the two agreed; a slave has
 * no clock to move, so the correction happens in the samples instead. The ring-fill servo
 * sets the ratio, this converts at it.
 *
 * The ratio stays within a few hundred ppm of one, so this is a fractional delay that walks
 * slowly, not a general resampler: 16 taps of Kaiser-windowed sinc per output sample, read
 * from a 512-phase table in flash (tools/gen_audio_asrc_table.py) and interpolated between
 * neighbouring phases. The generator states the accuracy: -117 dB to 20 kHz at 96 kHz,
 * worst case over every phase. At a ratio of exactly one and phase zero it is bit-exact
 * for samples of up to 24 significant bits.
 *
 * Pure C, no ESP-IDF: tests/host/test_audio_asrc.c runs it on the host.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define AUDIO_ASRC_TAPS   16u   /* per channel; the latency is half of it, in frames */
#define AUDIO_ASRC_BLOCK  64u   /* input frames taken in at a time: the RAM this costs */

typedef struct {
    /* Interleaved stereo: the history the filter still needs, then the block taken in. */
    float    x[(AUDIO_ASRC_TAPS - 1u + AUDIO_ASRC_BLOCK) * 2u];
    uint32_t have;      /* frames in x */
    uint32_t pos;       /* read position: whole frames into x ... */
    uint32_t frac;      /* ... and the fraction past it, Q32 */
    int32_t  step;      /* input frames per output frame, minus one, Q32 */
} audio_asrc_t;

/* Silence in the history and the read position on the first sample to come. The ratio is
 * kept: it belongs to the two clocks, not to the stream. */
void audio_asrc_reset(audio_asrc_t *a);

/* Input frames consumed per output frame = 1 + ppm / 1e6. Positive drains the input faster. */
void  audio_asrc_set_ppm(audio_asrc_t *a, float ppm);
float audio_asrc_ppm(const audio_asrc_t *a);

/*
 * Converts whole stereo frames of 32-bit little-endian samples (the wire format; `in` may
 * have any alignment) into at most `out_cap` interleaved frames at `out`. Returns the
 * frames written and sets *used to the input frames taken. Input is taken only when the
 * output has room for what it makes, so a call that fills `out` may leave input behind:
 * call again with the rest.
 */
size_t audio_asrc_run(audio_asrc_t *a, const uint8_t *in, size_t in_frames,
                      int32_t *out, size_t out_cap, size_t *used);
