/*
 * Board wiring. ESP32-WROOM-32 on a DevKitC.
 *
 * Confirmed by the customer on 18.09.2026 (docs/DEVICE_SPEC.md, section "Піни").
 * The pins were chosen by where they sit on the header, not by number: I2S is three
 * consecutive pins in the left row, the UART is two consecutive in the right row.
 * Nothing here may be changed without changing that document.
 */

#pragma once

#include "driver/gpio.h"

/* I2S to the host. This device is the master: it drives BCLK and WS, the host
 * follows. MCLK is not generated and not wired. */
#define BOARD_I2S_BCLK_GPIO   GPIO_NUM_25
#define BOARD_I2S_WS_GPIO     GPIO_NUM_26
#define BOARD_I2S_DOUT_GPIO   GPIO_NUM_27

/* Control link to the host, UART2. 17 and 16 are the IOMUX pins of UART2 on the
 * ESP32 (soc/esp32/include/soc/uart_pins.h), so the signal skips the GPIO matrix —
 * which is headroom we want at 460800 baud. */
#define BOARD_LINK_UART_NUM   UART_NUM_2
#define BOARD_LINK_TX_GPIO    GPIO_NUM_17
#define BOARD_LINK_RX_GPIO    GPIO_NUM_16
#define BOARD_LINK_BAUD       460800

/* Free for the port expander once there is something to connect:
 *   in or out : 13, 14, 18, 19, 21, 22, 23, 32, 33
 *   input only: 34, 35, 36, 39
 * The set the device actually exposes is declared at runtime in GPIO_MAP, so
 * adding a line later is one row in a table here and nothing on the host side. */
