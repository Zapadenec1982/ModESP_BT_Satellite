# modesp-bt-satellite

Bluetooth A2DP sink and I/O expander for [`modesp-player`](../modesp-player), on an original
ESP32. A separate project that sits next to the player rather than inside it.

## Why it exists

The player runs on an ESP32-P4 and cannot receive Bluetooth audio at all: A2DP needs
Bluetooth Classic, which exists only in the original ESP32, and the ESP32-C6 that carries the
player's Wi-Fi has BLE only.

It also closes a second gap. The player's pin budget is exhausted, so the amplifier's fault
lines and the extra buttons were headed for a separate I2C expander chip. A satellite with
spare GPIO makes that chip unnecessary, and gives an ADC, timers and edge counting that a
PCF8574 cannot.

## What it does

- Receives A2DP and emits I2S to the player. SBC, AAC, aptX and aptX HD.
- Reports what is playing over a UART link: title, artist, album, position, and the cover art
  the phone sends over AVRCP.
- Accepts transport and volume commands back.
- Owns a handful of GPIO on the player's behalf, with debounce and edge detection done
  locally so the link stays quiet.

## The link

UART at 460800, COBS framed with a CRC-16. The full protocol, and the reasoning behind
choosing it over I2C, is in the player's `docs/SATELLITE_BRIEF.md` section 5. That document is
canonical for the protocol and both sides reference it.

The player side is written against the protocol, not against this firmware. Replacing the
satellite with a bought Bluetooth module later means changing this firmware or writing a
translator, and not touching the player.

## Boundary

This project never writes to the player's tree and never flashes the player's panel. See
`CLAUDE.md`.

## Status

Stage 0. Three gates are open and they all live on the player's bench, so they belong to the
customer to schedule. No satellite code yet.
