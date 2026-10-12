/*
 * The absolute volume a peer hands us over AVRCP, as pure C: bt_av.c applies it,
 * tests/host/test_rules.c walks it.
 *
 * AVRCP absolute volume is 7 bits; bit 7 is reserved. Bluedroid strips it on the
 * target path (avrc_pars_tg.c, SetAbsoluteVolume command) but not on the
 * controller path (btc_avrc.c, VOLUME_CHANGED notification, and the
 * SetAbsoluteVolume response), so a peer that sets it would otherwise reach the
 * host as a VOLUME_STATE outside PROTOCOL.md's 0..127 and be echoed back to the
 * phone in our own INTERIM. Masked, not clamped, the way the stack's target
 * parser does it.
 */

#pragma once

#include <stdint.h>

static inline uint8_t avrc_abs_volume(uint8_t raw)
{
    return raw & 0x7F;
}
