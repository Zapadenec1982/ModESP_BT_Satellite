/*
 * A2DP sink and AVRCP controller.
 *
 * Stage 1 is SBC only: that is all stock Bluedroid can register as a stream
 * endpoint (btc_av_reg_sep rejects every other codec type). AAC and aptX arrive
 * with the patches in patches/ — see docs/BASE.md.
 */

#pragma once

#include "esp_err.h"

/* Brings up A2DP sink and AVRCP CT, then makes the device connectable and
 * discoverable. audio_out_init() must have run first: the stream can start as
 * soon as this returns. */
esp_err_t bt_av_start(const char *device_name);
