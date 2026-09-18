#include "satlink/crc16.h"

/* Bitwise rather than table-driven. The link carries a few kilobytes a second at
 * most, and 512 bytes of table is worth more here than the cycles it saves. */
uint16_t satlink_crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t satlink_crc16(const uint8_t *data, size_t len)
{
    return satlink_crc16_update(SATLINK_CRC16_INIT, data, len);
}
