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
 * Two different faults used to share the name `underruns`, and only one of them
 * was ever measured. The field that carried that name counted polls of the
 * application ring that came back empty -- and the ring sits upstream of a
 * ~32 ms TX DMA pool (6 descriptors of 240 frames), so an empty ring is not a
 * gap in the music. It is the ordinary way every drain cycle ends: the writer
 * polls with no timeout and keeps pulling until there is nothing left.
 *
 * That is why it climbed for a whole session (33 -> 4806 over the satellite's
 * uptime, 21-22.09.2026) while nobody heard anything wrong, and why no amount
 * of work on the host's DMA moved it. It was never about the host.
 *
 * It also could not report severity. The increment was rate limited to one per
 * 100 ms, so the ceiling was 10/s whatever happened; the observed "~2/s, steady"
 * was a count of 100 ms windows, not of events. One empty poll in a window and a
 * thousand printed the same number.
 *
 * The figure it was read next to did not help either. `ring 40..60 ms` in the
 * log is `fill_max`, a ten-second HIGH-water mark, and the `fill_min` beside it
 * was written to zero by the empty-ring branch itself. So "the ring looks full
 * while the DMA starves" was one artefact standing next to another. A mean did
 * exist -- the clock servo has averaged the fill over one second since item
 * 10.1 -- but it never left the servo, so nothing anyone could read from a log
 * said whether the ring was steadily full or filled in bursts. `fill_avg`
 * publishes one over the report window, and the two are finally distinguishable.
 *
 * So the names below say what each one measures:
 *
 *  - `ring_dry`    -- polls that found the ring empty, in THIS report window.
 *                     Expected to be non-zero while playing. Interesting only
 *                     next to `fill_avg`: many dry polls and a low mean is a
 *                     producer that cannot keep up, many dry polls and a healthy
 *                     mean is just the writer winning the race, as it should.
 *  - `dma_starved` -- since boot, and the one that is audible: the TX DMA
 *                     descriptor queue overflowed, meaning every buffer was
 *                     handed back unclaimed and the peripheral clocked out the
 *                     zeros `auto_clear` leaves behind. Zero is the only good
 *                     value. Counted only once the writer has fed the DMA at
 *                     least once since the stream started, so the priming of a
 *                     fresh stream does not register as a fault.
 */
typedef struct {
    uint32_t dropped_bytes;    /* ring was full: the producer outran the consumer */
    uint32_t ring_dry;         /* this window: the ring was empty when polled */
    uint32_t dma_starved;      /* since boot: the TX DMA ran out of written data */
    uint32_t fill_min;         /* bytes, lowest seen since the last report */
    uint32_t fill_avg;         /* bytes, mean over the reads in that window */
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
