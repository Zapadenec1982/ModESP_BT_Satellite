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

/* Close the callback-side PCM gate without waiting; invalidates old resume tokens. */
uint32_t audio_out_gate(void);
/* Control task only: exclude producer/writer, drain ring/carry, clear DMA to silence. */
esp_err_t audio_out_begin_format(uint32_t *token);
/* After the announcement was transmitted; stay gated if any step fails. */
esp_err_t audio_out_configure(uint32_t rate_hz, uint8_t bits, uint8_t source_bits);
bool audio_out_resume(uint32_t token);
uint8_t audio_out_slot_bits(void);
uint32_t audio_out_rate_hz(void);

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
    /* The same measurement repeated on the bytes going into the DMA, and the
     * first frame of the loudest block. Two meters at opposite ends of the same
     * pipe: agreement means the pipe is sound, disagreement says which end to
     * open. */
    uint32_t wire_peak;
    uint8_t  wire_head[8];
} audio_out_stats_t;

/* Reads the counters and resets the high and low water marks. */
void audio_out_get_stats(audio_out_stats_t *out);

/*
 * What is playing, for the periodic report.
 *
 * The output stage knows the rate and the slot width but not which codec produced
 * the samples, and the codec was only ever logged at the moment it was negotiated
 * -- so ten minutes into a session there was no way to find out what was playing
 * short of reconnecting the phone. The caller owns the string and must keep it
 * alive; a string literal is the expected argument.
 */
void audio_out_set_codec_name(const char *name);

/* Tells the output whether a stream is running, so that an empty ring can be told
 * from the silence between tracks. */
void audio_out_set_streaming(bool streaming);
