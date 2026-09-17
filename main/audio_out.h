/*
 * I2S output to the host. This device is the I2S master.
 */

#pragma once

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
