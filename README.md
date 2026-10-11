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
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | the link, byte by byte, version 1.7 |
| [`docs/BASE.md`](docs/BASE.md) | why the Bluetooth stack is patched and how those patches are carried |

A gap in the specification is a question for the customer, not an invitation to go looking in
the folder next door.

## Hardware

ESP32-WROOM-32 on a DevKitC. Not an S3, C3 or C6: only the original ESP32 has Bluetooth
Classic, and without it there is no A2DP. Wi-Fi is never brought up; this device does not go
on a network.

| Signal | GPIO |
|---|---|
| I2S BCLK | 13 |
| I2S WS, DOUT | 26, 27 |
| UART to the host | TX 17, RX 16 |

WS and DOUT sit next to each other in the left header row and BCLK apart, on 13 next to a GND
pin: on 25, beside WS, it put false edges on it (moved 04.10.2026). The UART two sit together in
the right row. 17 and 16 are also the IOMUX pins of UART2, which keeps 460800
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

On Linux, and in CI: `tools/setup.sh` then `CC=gcc PYTHON=python3 tools/run_host_tests.sh`
-- the same steps as the PowerShell scripts. `.github/workflows/build.yml` runs both in
the official `espressif/idf:v5.5.5` container on every push.

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

### And steered

Since hardening stage 10.1 (2026-09-21) the clock no longer runs free. Once a second
`audio_out.c` compares the ring's mean fill over that second with the middle of the
ring and nudges MCLK through `i2s_channel_tune_rate()`: a PI loop with its error in
milliseconds of audio (so 44.1/16 and 96/32 behave alike), 10 ppm per millisecond, an
integrator with a 400 s time constant, ±200 ppm of authority and 2 ppm steps. It runs
only while a stream plays with the gate open, freezes between tracks, and starts from
zero after every format change. The 10-second report line carries the applied ppm.

Two loops in series, and they do not fight: this one tracks the phone's crystal from
the ring, the P4's ±500 ppm ASRC tracks this device's BCLK from its own ring, and this
one is the slower of the two by design. Keep it that way if the P4's loop is ever
retuned.

The driver re-derives the APLL live for every step — sdm coefficients rewritten, a
calibration cycle run — while `audio_out.c` holds one extra APLL reference of its own,
without which IDF 5.5.5 powers the PLL down mid-stream and trips an assert
(`i2s_common.c:1487`, `clk_ctrl_os.c:111`). Whether BCLK is truly continuous through a
step is a scope measurement, not a code reading, and is the first thing to check on
the bench.

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
