# modesp-bt-satellite

A Bluetooth A2DP receiver on an original ESP32. It takes audio from a phone and hands it to a
host over I2S, tells the host what is playing over a UART, and lends the host a few of its own
GPIO on the side.

The host is somebody else's microcontroller. What it is does not change a single requirement
here, so this project does not know and does not care.

## Documents

Two, and they are self-contained.

| | |
|---|---|
| [`docs/DEVICE_SPEC.md`](docs/DEVICE_SPEC.md) | the device: hardware, pins, codecs, I2S, behaviour |
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | the link, byte by byte, version 1.0 |
| [`docs/BASE.md`](docs/BASE.md) | why the Bluetooth stack is patched and how those patches are carried |

A gap in the specification is a question for the customer, not an invitation to go looking in
the folder next door.

## Hardware

ESP32-WROOM-32 on a DevKitC. Not an S3, C3 or C6: only the original ESP32 has Bluetooth
Classic, and without it there is no A2DP. Wi-Fi is never brought up; this device does not go
on a network.

| Signal | GPIO |
|---|---|
| I2S BCLK, WS, DOUT | 25, 26, 27 |
| UART to the host | TX 17, RX 16 |

The I2S three sit next to each other in the left header row and the UART two in the right, so
each comes off on one ribbon. 17 and 16 are also the IOMUX pins of UART2, which keeps 460800
baud off the GPIO matrix.

Flashing and logs are on **COM13**.

## Building

```powershell
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
$env:IDF_COMPONENT_CACHE_PATH = "D:\P4_DSP\.idf_cache"
idf.py set-target esp32
idf.py build
idf.py -p COM13 flash monitor
```

Stock Bluedroid does A2DP sink in SBC and refuses to register an endpoint for anything else,
so AAC and aptX need changes inside the stack. Rather than vendoring a whole IDF tree, this
repository pins one in `idf.pin` and keeps the delta in `patches/`. `tools/setup.ps1` builds a
local `components/bt` from the two. The shared IDF on disk is read and never written — it
belongs to more than this project.

## Testing

```powershell
.\tools\run_host_tests.ps1
```

`components/satlink` has no ESP-IDF dependencies on purpose, so the framing, the CRC, the
reassembler and all nineteen message layouts build and run with a host compiler. A framing bug
is far cheaper to find in a terminal than over a UART.

The link can also be exercised on the board with no host attached, by looping the UART back on
itself inside the chip:

```powershell
idf.py -B build_loopback -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.loopback" build
idf.py -B build_loopback -p COM13 flash monitor
```

## Layout

```
components/satlink/   the protocol: COBS, CRC-16, framing, every message. No IDF, host-testable
main/link.c           the UART transport, the two send queues and the drop policy
main/bt_av.c          A2DP sink and AVRCP controller, and what the host is told about them
main/audio_out.c      I2S master out, ring buffer, clock that follows the stream
main/expander.c       the GPIO this board lends out. The table is empty and that is a valid state
main/board.h          every pin number in the project, and nowhere else
patches/              the Bluetooth stack delta, one diff per logical change
tests/host/           the link layer, tested without a board
```

## The bit clock, measured

Asked by the host, whose receiver is a slave and has to tell a stopped clock from
silence. Measured on the board itself: the pulse counter is fed from the BCLK pin's
own output, so no wire and no oscilloscope are involved (`CONFIG_SAT_CLOCK_PROBE`).

**It never stops.** With no phone and no stream, BCLK ran continuously at 1 411 256 Hz
for twenty-five seconds, and the data line carries zeros rather than a repeated buffer.

Nominal is 1 411 200 Hz, so the clock is **+40 ppm fast** — about 143 ms of drift an
hour. Both clocks in that measurement come from the same crystal, so the figure is the
APLL's own fractional error; the crystal's error adds to it in absolute terms. It is
the drift the host's rate servo has to absorb, and its sign.

One exception worth knowing: changing the sample rate disables and re-enables the
channel, so the clock genuinely stops for that moment rather than going quiet. Today
that path never runs — the rate is 44.1 kHz at boot and stock Bluedroid negotiates
nothing else — but it would if 48 kHz were ever announced.

## Status

The receiver works: a phone pairs, SBC plays at 44.1 kHz, and the samples leave on the I2S
pins. Track title, artist, album and length arrive over AVRCP and refresh on every skip.

The link is implemented in full against `docs/PROTOCOL.md` 1.0 — all nineteen messages, both
directions — and verified on the board with the UART looped back on itself: 95 frames out, 95
in, no CRC errors, no sequence gaps, every PING answered. It has not yet spoken to a real
host, because there is no second board on the bench.

Still open: AAC and aptX, which wait on the stack patches; the expander's pin table, which
waits on something to connect to it; and cover art, which is written but has never met a real
phone.
