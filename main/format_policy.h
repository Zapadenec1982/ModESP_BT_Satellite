/*
 * Whether a host greeting or an I2S role change needs a new format transaction, as
 * pure C: bt_av.c decides with it, tests/host/test_rules.c walks it.
 *
 * A new transaction gates PCM, announces a new generation and configures I2S before
 * it resumes (PROTOCOL.md 6.4). Without one the current announcement is only repeated
 * for a host that may have restarted. A role change always needs one: audio_out_set_role
 * gates PCM and voids the pending resume token, and it may have changed the slot width,
 * so repeating the old announcement would leave the stream silent for good.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool fmt_needs_restart(bool valid, uint8_t bits_now, uint8_t bits_new,
                                     bool ack_now, bool ack_new, bool role_changed,
                                     bool can_follow)
{
    return !valid || bits_new != bits_now || ack_new != ack_now || role_changed || !can_follow;
}
