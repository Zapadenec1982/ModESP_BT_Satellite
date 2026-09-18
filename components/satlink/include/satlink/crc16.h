/*
 * CRC-16/CCITT-FALSE: polynomial 0x1021, init 0xFFFF, no reflection, no final XOR.
 *
 * Named exactly because the "CCITT" family has several mutually incompatible
 * members, and picking the wrong one produces a link that almost works.
 * Check value for the ASCII string "123456789" is 0x29B1.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define SATLINK_CRC16_INIT   0xFFFFu

uint16_t satlink_crc16(const uint8_t *data, size_t len);

/* For a checksum over pieces that are not contiguous. Seed the first call with
 * SATLINK_CRC16_INIT. */
uint16_t satlink_crc16_update(uint16_t crc, const uint8_t *data, size_t len);
