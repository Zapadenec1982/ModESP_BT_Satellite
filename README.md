# modesp-bt-satellite

**English** · [Українська](README.uk.md)

Firmware for an original ESP32 that receives Bluetooth audio from a phone and hands it to a
**host** microcontroller: the audio over I2S, everything else over a UART. Audio and its control,
nothing more: the port expander that lent the host spare GPIO was removed on 24.10.2026.

What the host is changes no requirement here, so this project neither knows nor cares. Its
other end is a second, independent implementation of the same protocol.

## What it does

- **A2DP sink** with SBC, AAC, aptX and aptX HD, and LHDC V5 when its decoder is fetched (see
  [Building](#building)). 44.1 and 48 kHz, and 96 kHz for LHDC V5 when the host says it can take
  it. LDAC is deliberately absent: the only decoder in circulation has no licence.
- **AVRCP controller**: title, artist, album, position, play state, absolute volume, transport
  commands, and the album cover as a JPEG thumbnail.
- **I2S out**, 16- or 32-bit slots, Philips format, no MCLK. The device is the clock master, or,
  from protocol 1.9, the host clocks the link and the device drives data only.
- **Control link**: UART 460800 8N1 with COBS framing and CRC-16. The host learns the stream
  format before the first sample, and Bluetooth settings, pairing confirmation and the list of
  paired phones are the host's to show and change.
No Wi-Fi, no network, no screen, no files, no OTA, no GPIO for the host.

## Documents

| | |
|---|---|
| [`docs/DEVICE_SPEC.md`](docs/DEVICE_SPEC.md) | the device: hardware, pins, codecs, I2S, behaviour, customer decisions |
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | the link to the host, byte by byte, version 1.10 |
| [`docs/BASE.md`](docs/BASE.md) | why the Bluetooth stack is patched and how the patches are carried |
| [`patches/README.md`](patches/README.md) | what each stack patch does |
| [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | third-party code in the image and its terms, as found |

The specification and the protocol are written in Ukrainian. `PROTOCOL.md` is a shared contract:
a word-for-word copy lives in the host's repository, and the two change together. The host this
was built for is [ModESP Streamer](https://github.com/Zapadenec1982/ModESP_Streamer), a network
streamer on the ESP32-P4; its copy is `docs/LINK_PROTOCOL.md` there.

A gap in the specification is a question for the customer, not an invitation to go looking in
the folder next door.

## Hardware

ESP32-WROOM-32 on a DevKitC. Not an S3, C3 or C6: only the original ESP32 has Bluetooth Classic,
and without it there is no A2DP. WROOM rather than WROVER, because WROVER spends GPIO16 and 17 on
PSRAM.

| Signal | GPIO |
|---|---|
| I2S BCLK | 13 |
| I2S WS, DOUT | 26, 27 |
| UART TX to the host, RX from it | 17, 16 |

WS and DOUT sit next to each other in the left header row, BCLK on 13 beside a GND pin; on 25,
next to WS, it put false edges on it (moved 04.10.2026). The UART pair sits together in the
right row; 17 and 16 are also UART2's IOMUX pins, which keeps 460800 baud off the GPIO matrix.

The same firmware has a second configuration for the **bare module**: the link moves to UART0,
TX 1 and RX 3, the pair the ROM bootloader listens on, so the host both flashes the device and
talks to it over the same two wires (and drives EN and IO0). The console is off there. It is a
Kconfig choice (`SAT_LINK_PORT`) made by building with `sdkconfig.bare`; see
[`docs/DEVICE_SPEC.md`](docs/DEVICE_SPEC.md), "Голий модуль".

The CPU runs at 240 MHz: at 160 the aptX decoder could not keep up.

## Building

The ESP-IDF is pinned in [`idf.pin`](idf.pin) (v5.5.5). Stock Bluedroid does A2DP sink in SBC
only, so the stack is patched: `tools/setup.*` copies `components/bt` out of the pinned IDF,
applies `patches/` and copies `bt_overlay/` on top. The shared IDF on disk is read and never
written. Details in [`docs/BASE.md`](docs/BASE.md).

Windows, PowerShell:

```powershell
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
$env:IDF_COMPONENT_CACHE_PATH = "D:\P4_DSP\.idf_cache"
.\tools\setup.ps1
idf.py set-target esp32
idf.py build
idf.py -p COM13 flash monitor
```

Linux:

```bash
. ~/.espressif/tools/activate_idf_v5.5.5.sh   # or $IDF_PATH/export.sh
tools/setup.sh
idf.py set-target esp32
idf.py build
```

`components/bt` is generated and ignored by git; `tools/setup.sh --clean` (`-Clean` on Windows)
builds it again from scratch. Flashing and logs on this bench are on **COM13**. COM12 is the
host, not this board.

LHDC V5 needs a decoder this repository does not carry; [`lhdc.pin`](lhdc.pin) says why.
`tools/setup.sh --with-lhdc` (`-WithLhdc` on Windows) fetches it at the pinned commit into
`components/lhdcv5_decoder`, also ignored by git, and the `sdkconfig.lhdc` overlay below switches
it on. Without the overlay the decoder is not built; with the overlay and no decoder the build
stops and says so.

Other builds are overlays, each a `sdkconfig.*` file named on the command line:

| Overlay | What it builds |
|---|---|
| `sdkconfig.bare` | the bare module: the link on UART0 (GPIO1/3), console off |
| `sdkconfig.linkonly` | the link and nothing else: HELLO, PING/PONG, LOG |
| `sdkconfig.loopback` | the full firmware with the UART looped back inside the chip |
| `sdkconfig.clockprobe` | measures the I2S bit clock on the board itself and logs it |
| `sdkconfig.lhdc` | LHDC V5, with the decoder from `--with-lhdc`; goes with any of the above |

```bash
idf.py -B build_bare -D SDKCONFIG=build_bare/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bare" build
```

## Testing

```bash
CC=gcc PYTHON=python3 tools/run_host_tests.sh      # Windows: .\tools\run_host_tests.ps1
```

The suites build with a host compiler and need no board: framing, CRC and every message layout
with golden vectors from the host side, the endpoint re-registration and Bluetooth settings
rules, and replays of real stack, link, cover, audio and LHDC code against stubs. Some of them
need `components/bt`, so run `tools/setup.*` first; the LHDC replay also needs `--with-lhdc` and
is skipped without it. A passing replay is not a phone or bench result.

`.github/workflows/build.yml` runs setup, the build and the host tests in the official
`espressif/idf:v5.5.5` container on every push, once as is and once with LHDC V5.

## Layout

```
components/satlink/        the protocol: COBS, CRC-16, framing, every message. No IDF, host-testable
components/lhdcv5_decoder/ the LHDC V5 decoder: fetched by setup --with-lhdc, never committed
main/link.c                the UART transport, the send queues and the drop policy
main/bt_av.c               A2DP sink, AVRCP controller, pairing and settings, and what the host is told
main/audio_out.c           I2S out, the PCM ring, the clock servo
main/board.h               every pin number in the project, and nowhere else
bt_overlay/                stack files that are entirely ours: codec framework, decoders
patches/                   the Bluetooth stack delta, one diff per logical change
tests/host/, tools/        host tests and the scripts that run them; setup scripts
```

## Licence

This project's own code is under the [MIT License](LICENSE): `main/`, `components/satlink/`,
`patches/`, `tests/`, `tools/` and the docs. A file with a licence notice of its own keeps it,
changes made here included: in `bt_overlay/` that is Apache-2.0 for the codec framework, the
RealNetworks Public Source License for Helix AAC (`RPSL.txt` beside it) and LGPL-2.1-or-later for
libfreeaptx. The patches change Bluedroid, which stays Apache-2.0.
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) lists all of it, and what a built image owes
each piece; `tools/relink_kit.ps1` produces the LGPL relink kit for a release. The LHDC V5 decoder
is not in this repository at all ([`lhdc.pin`](lhdc.pin)).
