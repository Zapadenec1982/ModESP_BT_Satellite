/*
 * I2S output to the host. This device is the I2S master.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/* Brings up the I2S master and the writer task. The channel starts at 44.1 kHz,
 * 16-bit stereo, which is what stock Bluedroid advertises for SBC. */
esp_err_t audio_out_init(void);

/* Hands PCM to the output. Called from the Bluetooth stack's own task, so it never
 * blocks: when the ring is full the samples are dropped and counted. A stalled
 * writer must not be allowed to stall the Bluetooth stack. */
void audio_out_write(const uint8_t *pcm, size_t len);

/* Retunes the output clock. A2DP tells us the rate when the stream is configured,
 * which is before the first sample arrives. */
esp_err_t audio_out_set_rate(uint32_t rate_hz);

/* Samples dropped since boot because the ring was full. Zero is the only good value;
 * anything else means the host is not taking the data fast enough. */
uint32_t audio_out_dropped_bytes(void);

/*
 * How the output is actually doing.
 *
 * `underruns` is the one that was missing and the one that is audible: when the
 * ring runs dry the DMA sends zeros by design, which is a gap in the music, and
 * nothing in this firmware used to notice. A full ring is loud in the log; an
 * empty one was silent in every sense.
 */
typedef struct {
    uint32_t dropped_bytes;    /* ring was full: the producer outran the consumer */
    uint32_t underruns;        /* ring was empty when the writer wanted data */
    uint32_t fill_min;         /* bytes, lowest seen since the last report */
    uint32_t fill_max;
    /* Largest absolute sample seen since the last report, 0..32768. It answers
     * one question and answers it without argument: is this device handing over a
     * quiet signal, or a loud one that something downstream is turning down. */
    uint32_t peak;
} audio_out_stats_t;

/* Reads the counters and resets the high and low water marks. */
void audio_out_get_stats(audio_out_stats_t *out);

/* Tells the output whether a stream is running, so that an empty ring can be told
 * from the silence between tracks. */
void audio_out_set_streaming(bool streaming);
