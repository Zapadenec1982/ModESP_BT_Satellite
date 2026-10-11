/*
 * The port expander: this device's spare GPIOs, lent to the host.
 *
 * The table of lines lives here and nowhere else, because it describes this
 * board. What hangs off a line and what it is called is the host's business, and
 * it says so with GPIO_CFG. That split is why adding a line later is one row here
 * and no change at all on the other side.
 *
 * The table is empty today. That is a working configuration, not a stub: the
 * device still announces itself as understanding the expander messages, and still
 * sends a GPIO_MAP saying it has nothing to offer yet.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "satlink/messages.h"

/* The lock and the ADC units. Call before link_start(): from then on the host can
 * send GPIO_CFG, and a mode 6 line needs its unit to exist. */
esp_err_t expander_init(void);

/* Sends GPIO_MAP and starts the debounce poller if there is anything to poll.
 * Call after link_start(), so that the map follows the HELLO. */
esp_err_t expander_start(void);

/* GPIO_MAP again. PROTOCOL.md 3.4 sends it after every host HELLO, because a host
 * that greets later than boot, or restarts, has missed the first one. Safe before
 * expander_start(). */
void expander_send_map(void);

/* The capability bits this module can honestly claim. EXPANDER always, because it
 * means "understands these messages" rather than "has pins"; ADC only when some
 * line really can read one, which with an empty table is never. */
uint32_t expander_caps(void);

/* Handlers the link calls when the host asks for something. */
void expander_apply_cfg(const satlink_gpio_cfg_t *cfg);
void expander_set_outputs(uint32_t values, uint32_t mask);
void expander_read_adc(uint8_t index);
