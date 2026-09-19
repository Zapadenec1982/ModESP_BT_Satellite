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

/*
 * Slot width on the wire: 16 or 32 bits, two channels either way.
 *
 * Sixteen is the default and the only width a host gets unless it says in HELLO
 * that it can take thirty-two. Thirty-two exists for one reason: aptX HD carries
 * 24-bit samples, and narrowing them to 16 throws away eight bits that the codec
 * went to some trouble to deliver.
 *
 * Must be set before the first sample, and it changes what STREAM_FMT announces.
 * BCLK follows: 32 clocks per frame at 16-bit slots, 64 at 32-bit.
 */
esp_err_t audio_out_set_slot_bits(uint8_t bits);
uint8_t   audio_out_slot_bits(void);

/* What the master clock is actually producing. Needed to announce the format
 * before changing it, which is what PROTOCOL.md 1.2 requires and what this
 * firmware failed to do when it switched slot width at startup. */
uint32_t  audio_out_rate_hz(void);

/*
 * What the decoder is about to start handing to audio_out_write(): 16-bit
 * samples, or 32-bit ones carrying 24 significant bits in the top.
 *
 * SBC and AAC decode to 16. aptX and aptX HD decode to 24, widened to 32 on the
 * way out of the decoder so that nothing is lost before this point. Set it when
 * the codec is known and before the stream starts; the output stage converts to
 * whatever the wire is, dithering when it has to narrow.
 */
void audio_out_set_source_bits(uint8_t bits);

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
 * Throws away whatever is still queued.
 *
 * Used when the format is about to change: samples captured under the old rate
 * or the old width are not merely late, they are wrong, and playing them out
 * after the clock has moved is worse than dropping them.
 */
void audio_out_flush(void);

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
