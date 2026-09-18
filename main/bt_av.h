/*
 * A2DP sink and AVRCP controller, and the bridge between what the phone does and
 * what the host is told about it.
 *
 * Stage 1 is SBC only: that is all stock Bluedroid can register as a stream
 * endpoint (btc_av_reg_sep rejects every other codec type). AAC and aptX arrive
 * with the patches in patches/ — see docs/BASE.md.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Brings up A2DP sink and AVRCP CT, then makes the device connectable and
 * discoverable. audio_out_init() must have run first: the stream can start as
 * soon as this returns. */
esp_err_t bt_av_start(const char *device_name);

/* What the host may ask for, straight from CMD_TRANSPORT, CMD_VOLUME and
 * CMD_RADIO. All are safe to call with no phone connected: they simply do
 * nothing, because a command with no one to send it to is not an error. */
void bt_av_transport(uint8_t cmd);          /* satlink_transport_t */
void bt_av_set_volume(uint8_t volume);      /* absolute AVRCP volume, 0..127 */
void bt_av_set_radio(bool on, bool discoverable, bool disconnect);

/* Pushes the current Bluetooth state at the host. Used when a host appears after
 * the phone did, so that it does not have to wait for the next change. */
void bt_av_report_state(void);
