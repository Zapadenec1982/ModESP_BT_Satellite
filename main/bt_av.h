/*
 * A2DP sink and AVRCP controller, and the bridge between what the phone does and
 * what the host is told about it.
 *
 * SBC, AAC, aptX and aptX HD. Stock Bluedroid registers an SBC endpoint and
 * nothing else; the rest arrive with patches/ and bt_overlay/ — see docs/BASE.md.
 * Whichever the phone picks, the host is handed 16-bit slots.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "satlink/format_sync.h"
#include "satlink/messages.h"
void bt_av_format_ready(const fmt_ready_t *ready);

#include "esp_err.h"

/* Brings up A2DP sink and AVRCP CT, then makes the device connectable and
 * discoverable, or whatever the saved Bluetooth settings say instead (protocol
 * 1.8). `device_name` is the factory name, for a device the host never renamed.
 * audio_out_init() must have run first: the stream can start as soon as this
 * returns. */
esp_err_t bt_av_start(const char *device_name);

/* What the host may ask for, straight from CMD_TRANSPORT, CMD_VOLUME and
 * CMD_RADIO. All are safe to call with no phone connected: they simply do
 * nothing, because a command with no one to send it to is not an error. */
void bt_av_transport(uint8_t cmd);          /* satlink_transport_t */
void bt_av_set_volume(uint8_t volume);      /* absolute AVRCP volume, 0..127 */
void bt_av_set_radio(bool on, bool discoverable, bool disconnect);

/*
 * Protocol 1.8: the Bluetooth settings, the bonds and pairing, from the link's task.
 *
 * Each only queues the request for the Bluetooth task, which owns all of it, and
 * returns. bt_av_host_hello() goes on every HELLO: the pairing mode follows the
 * host's PAIR_CONFIRM bit, and a 1.8 host is sent BT_CONFIG and BOND_LIST.
 */
void bt_av_host_hello(void);
void bt_av_pair_reply(const uint8_t addr[6], bool accept);
void bt_av_config_set(const satlink_bt_config_set_t *set);
void bt_av_bond_remove(const uint8_t addr[6]);
void bt_av_bt_refresh(void);

/*
 * Tells a host that greeted everything it needs in order to stop guessing, and
 * takes the slot width it asked for. Safe to call at any time.
 *
 * The format and the cover are otherwise announced exactly once, when the phone
 * connects -- and a host that comes up after the phone would never hear them.
 * STREAM_FMT in particular is the one the specification says must arrive before
 * the first sample, precisely so that nobody has to measure the rate off the
 * stream. The width is announced before it is taken.
 *
 * Called at startup and again whenever a greeting arrives, because a host that
 * was slow to say hello used to leave the device narrow until someone power
 * cycled it. Contract 1.2 permits the change at any time provided it is
 * announced, so there is no longer a reason to wait for a reboot.
 *
 * When the width already matches it only repeats the announcement, so calling it
 * on every HELLO is cheap. The sample rates the stream endpoints advertise are not in its gift:
 * those are fixed when the endpoints are registered, and a greeting that would
 * change them has the sink de-initialised and initialised again once no phone
 * is attached (main/endpoint_policy.h).
 */
void bt_av_apply_host_caps(void);

/* From the link's task: queue a BT_STATE for the Bluetooth task, which owns the
 * phone's state, address and name, so the message is never built from a half-written
 * name. False when it could not be queued, so the caller can ask again later. */
bool bt_av_request_state(void);

/*
 * Protocol 1.7. The host received this pair's cover broken and wants it again.
 *
 * Honoured only when the pair names this boot and the track that is playing now:
 * a stale pair means the phone has moved on, and the COVER_RESET the next track
 * brings carries its own image anyway. Bounded, because the observed fault is one
 * dropped UART frame in a 50 KiB image and one repeat settles that, while a link
 * that loses frames steadily is a different fault and must not be answered with
 * unbounded OBEX traffic.
 *
 * Called from the link's task. It only raises the same flags a late HELLO does,
 * because every byte of cover state belongs to the Bluetooth callback task.
 */
void bt_av_cover_retry(uint32_t boot_nonce, uint32_t track_generation);
