/*
 * Counts the I2S bit clock's own edges, to answer what it does when nothing is
 * playing. Compiled out unless CONFIG_SAT_CLOCK_PROBE is set.
 */

#pragma once

#include "esp_err.h"

/* Call after audio_out_init(): it measures the clock that call starts. */
esp_err_t clock_probe_start(void);
