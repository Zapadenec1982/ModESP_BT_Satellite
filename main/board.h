/*
 * Board wiring. ESP32-WROOM-32 on a DevKitC.
 *
 * Confirmed by the customer on 18.09.2026 (docs/DEVICE_SPEC.md, section "Піни").
 * The pins were chosen by where they sit on the header, not by number: WS and DOUT are
 * consecutive in the left row, BCLK sits apart next to a GND pin, the UART is two
 * consecutive in the right row. Nothing here may be changed without changing that document.
 */

#pragma once

#include "driver/gpio.h"

/* I2S with the host. A host of link protocol 1.9 or later clocks the link and BCLK and WS
 * are inputs here; an older host gets this device as the master. MCLK is not generated and
 * not wired.
 *
 * BCLK moved from 25 to 13 on 04.10.2026 (customer): on 25 its wire ran beside WS on 26 and
 * put false edges on it (03.10.2026, the clock probe). 13 sits next to a GND pin on the
 * DevKitC, and the BCLK wire is twisted with a ground wire. */
#define BOARD_I2S_BCLK_GPIO   GPIO_NUM_13
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
