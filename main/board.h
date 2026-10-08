/*
 * Board wiring. ESP32-WROOM-32, on a DevKitC or as a bare module (ABT_play).
 *
 * Confirmed by the customer on 18.09.2026 (docs/DEVICE_SPEC.md, section "Піни").
 * The pins were chosen by where they sit on the header, not by number: I2S is three
 * consecutive pins in the left row, the UART is two consecutive in the right row.
 * Nothing here may be changed without changing that document.
 */

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"

/* I2S to the host. This device is the master: it drives BCLK and WS, the host
 * follows. MCLK is not generated and not wired. */
#define BOARD_I2S_BCLK_GPIO   GPIO_NUM_25
#define BOARD_I2S_WS_GPIO     GPIO_NUM_26
#define BOARD_I2S_DOUT_GPIO   GPIO_NUM_27

/* Control link to the host. Which UART is a Kconfig choice (main/Kconfig.projbuild):
 *
 *  - UART2 on 17/16 for the DevKitC, where UART0 belongs to the USB bridge that flashes
 *    the board and shows its console.
 *  - UART0 on 1/3 for the bare module (sdkconfig.abt): the host reaches the ROM
 *    bootloader over the same two wires, so it flashes this device and talks to it on one
 *    pair. The console has to leave UART0 for that.
 *
 * Both pairs are the IOMUX pins of their UART on the ESP32
 * (soc/esp32/include/soc/uart_pins.h), so the signal skips the GPIO matrix — which is
 * headroom we want at 460800 baud. */
#if CONFIG_SAT_LINK_UART0
#if CONFIG_ESP_CONSOLE_UART && CONFIG_ESP_CONSOLE_UART_NUM == 0
#error "The link is on UART0 and so is the console. Build with sdkconfig.abt (console NONE)."
#endif
#define BOARD_LINK_UART_NUM   UART_NUM_0
#define BOARD_LINK_TX_GPIO    GPIO_NUM_1
#define BOARD_LINK_RX_GPIO    GPIO_NUM_3
#else
#define BOARD_LINK_UART_NUM   UART_NUM_2
#define BOARD_LINK_TX_GPIO    GPIO_NUM_17
#define BOARD_LINK_RX_GPIO    GPIO_NUM_16
#endif
#define BOARD_LINK_BAUD       460800

/* Free for the port expander once there is something to connect:
 *   in or out : 13, 14, 18, 19, 21, 22, 23, 32, 33
 *   input only: 34, 35, 36, 39
 * The set the device actually exposes is declared at runtime in GPIO_MAP, so
 * adding a line later is one row in a table here and nothing on the host side. */
