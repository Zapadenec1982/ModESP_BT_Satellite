**English** · [Українська](DEVICE_SPEC.uk.md)

# Device specification

What is to be built and how it behaves. Byte layouts and the rules of the link are in `PROTOCOL.md`,
the only other document in this project. Nothing else needs reading.

---

## Device

An ESP32 board that receives audio from a phone over Bluetooth and hands it to the **host** by two
paths: the audio over I2S, everything else over UART. Nothing but audio and its control: the port
expander that gave the host spare GPIO was removed by the customer's decision of 24.10.2026
(protocol 1.10).

The host is someone else's microcontroller. What it is does not concern this project.

---

## Hardware

| | |
|---|---|
| Chip | **ESP32-WROOM-32** on a DevKitC board. Customer's decision, 18.09.2026 |
| Why not S3 / C3 / C6 | only the original has Bluetooth Classic, and without it there is no A2DP |
| Power | its own regulator of 0.5 A or more, 22 µF at the pin. Transmit peaks around 130 mA |
| Clock | **240 MHz**, not the usual 160. The aptX decoder cannot keep up otherwise: at 160 MHz and `-Og` it held a core at 100 %, lost two thirds of the packets and woke the watchdog |
| Memory | no PSRAM. While LHDC plays, about 47 KiB of heap is free |
| Wi-Fi | disabled entirely in Kconfig. The device never goes on a network |
| External interfaces | I2S (3 lines), UART (2 lines) |
| Flashing and log port | **COM13** (USB-Enhanced-SERIAL CH9102 on the DevKitC). COM12 is the host, not this board |

WROOM rather than WROVER, and not only because of the PSRAM, which is not needed here. On a WROVER
**GPIO16 and GPIO17 are taken by the PSRAM** and do not reach the header. On a WROOM they are free,
and that is where the UART goes.

---

## Pins

Confirmed by the customer on 18.09.2026: take pins that are convenient and safe, and that sit
**next to each other on the header** of the DevKitC. Every number in the firmware lives in
`main/board.h` only.

What must not be touched on an ESP32: GPIO6–11 belong to the flash, GPIO34–39 are input only, and
GPIO0, 2, 5, 12, 15 are strapping pins and must not be made outputs.

| Signal | GPIO | Where on the header |
|---|---|---|
| I2S BCLK | 13 | left row, next to GND (since 04.10.2026) |
| I2S WS | 26 | left row, two in a row |
| I2S DOUT | 27 | ↑ |
| UART TX to the host | 17 | right row, two in a row |
| UART RX from the host | 16 | ↑ |

On a DevKitC-32 the left row runs … 32, 33, 25, **26, 27**, 14, 12, GND, **13** …, and the right
… 19, 18, 5, **17, 16**, 4 … 17 and 16 are also UART2's IOMUX pins, so 460800 baud does not go
through the GPIO matrix.

**04.10.2026, customer's decision:** BCLK moved from 25 to 13. On 25 its wire ran next to WS (26)
and put false edges on it (03.10.2026, edge probe). 13 sits next to GND, and the BCLK wire is
twisted with a ground wire.

### Bare module

A second configuration of the same firmware, chosen in Kconfig (`SAT_LINK_PORT`), built with
`sdkconfig.bare`. I2S is the same. The link moves to **UART0: TX 1, RX 3**, the very pair the ROM
bootloader listens on, so the host both flashes the device and talks to it over the same two
wires (the host drives EN and IO0). The console and the second-stage bootloader's log are off:
nothing but the link may talk on UART0.

| Signal | GPIO |
|---|---|
| UART TX to the host | 1 (TXD0) |
| UART RX from the host | 3 (RXD0) |

---

## Bluetooth

- **A2DP sink** and **AVRCP controller**, plus AVRCP target for absolute volume.
- Pairing, automatic reconnection after power-on, one active connection.

### Codecs

| Codec | Rates | Bits from the decoder | Decoder |
|---|---|---|---|
| SBC | 44.1 / 48 kHz | 16 | Bluedroid |
| AAC | 44.1 / 48 kHz | 16 | Helix, AAC-LC only |
| aptX | 44.1 / 48 kHz | 24 | libfreeaptx |
| aptX HD | 44.1 / 48 kHz | 24 | libfreeaptx |
| LHDC V5 | 44.1 / 48 / 96 kHz | 16 or 24 | from upstream at the commit in `lhdc.pin`, only with `sdkconfig.lhdc` |

Every endpoint is stereo only. 48 kHz is offered only to a host that can relock, and 96 kHz only
to a host with bit 5 (`PROTOCOL.md`, 6.2). How the bits land in the slots is there too. If a host
that will not take the current rate says hello in the middle of a stream, the audio stays closed
and is not announced: for 96 kHz `PROTOCOL.md` 6.5 says so, and for 48 kHz without bit 0 it is the
same (customer's decision, 11.10.2026). The audio comes back with a host that takes the rate, or
when the phone reconnects.

**LDAC is not carried over**: the only decoder on the net has no licence, and Sony publishes an
encoder only. Customer's decision, confirmed 11.10.2026. aptX LL was carried over the same day and
reverted: there is no source that would send it (`BASE.md`).

**The phone chooses the codec.** It takes the best endpoint the receiver offers, and it cannot be
asked for less. Android 17 has no codec choice in its developer options, and its HD audio switch
drops straight to SBC. The phone's order of preference: aptX HD, aptX, AAC, SBC; where LHDC V5 sits
in it is not recorded. So the only way to bring the phone down a step is to withdraw the top codec
(`BT_CONFIG_SET`). SBC cannot be withdrawn: A2DP requires it of everyone.

**Endpoints are registered again only with no phone connected.** Bluedroid reads what to offer
when it registers the endpoints, and never again. So after power-on the device waits up to 10 s
for the host's `HELLO`, and when the host's capabilities or the enabled codecs change, it registers
the endpoints again as soon as no phone is connected (`main/endpoint_policy.h`). This works only
with patch 0006.

### A host pause the phone ignores

05.10.2026, customer's decision. If the A2DP stream is still running 5 s after a `CMD_TRANSPORT`
pause or stop, the device suspends it itself (AVDTP SUSPEND). The phone stays connected, the radio
goes quiet. Any other transport command before the 5 s are up cancels the check; a repeated pause
does not restart the count. The phone itself cancels it too: when its stream stops after the
pause, or when it reports that it is playing again (the user resumed playback on the phone;
customer's decision, 12.10.2026). The host's next pause after that starts the count again
(`main/pause_policy.h`). The reason: a live stream beside the panel's Wi-Fi lost AirPlay packets
(24 gaps of up to 280 ms in 90 s).

A stream the device suspended itself, it also starts itself (AVDTP START): on play from the host,
or when the phone reports that it is playing again. Android does not resume a stream the receiver
suspended, not even with its own play button. The first version, with a 1.5 s wait, also suspended
phones that had obeyed the pause (Android keeps the stream for about 3 s more), and after that the
audio did not come back until Bluetooth reconnected.

### Volume

The device is an AVRCP absolute volume target. Without a target the phone has nowhere to send the
volume, and applies it itself before encoding, losing resolution in the phone already. With a
target the phone sends the stream at full scale and reports only a number. The device forwards it
to the host (`VOLUME_STATE`) and does not scale the samples: the host has a DSP with headroom, and
that is the right place to scale.

### Bluetooth settings from the host (protocol 1.8, 25.09.2026)

The customer's request: Bluetooth settings on the panel's screen — the pairing code, removing
paired phones, enabling and disabling codecs, the name. The code is "on screen": the same six-digit
code on the phone and on the panel, and the person confirms on the panel. Layouts and rules are in
`PROTOCOL.md`, section 9.

What the device keeps, in NVS:

- the settings: `radio`, `discoverable`, `auto_reconnect`, the disabled codecs, the name. Disabled,
  not enabled: a codec a new build brings will be enabled until someone disables it;
- its own table of paired phones: address, name, order of connection. Bluedroid's bond store holds
  only the key, its type, the PIN length and the Secure Connections flag; there are no names in it.

The pairing method follows the host's last `HELLO`: with bit 7, DisplayYesNo and a request to the
host; without it, NoInputNoOutput and consent without a request, as in 1.7.

What had to change in the stack for this, because stock Bluedroid 5.5.5 does not provide it:

- **Just Works to the application** (patch 0007). `btc_dm_sp_cfm_req_evt()` in `btc_dm.c` answers
  "yes" to Just Works itself ("just work, not show to users"), and the application never learns of
  such a request. Now the request reaches the application flagged Just Works and with the phone's
  name, which BTA has already read, and the application must answer every confirmation.
- **Hidden mode.** Being invisible does not by itself stop pairing (`BTM_ALLOW_CONN_IF_NONDISCOVER`
  = TRUE): whoever knows the address can still pair. So `discoverable` 0 is also
  `BTM_SetPairableMode(FALSE, FALSE)`: new pairings are refused, paired phones connect with their
  key. Not `connect_only_paired`: it lets through only keys with MITM protection, and every bond
  made in 1.7 mode (NoInputNoOutput) has none.
- **An LHDC V5 switch** in `bt_overlay`, the same as for AAC, aptX and aptX HD (`A2DP_SinkOffer*`).

### LHDC V5 (customer request, 19.09.2026)

The decoder is `WillyBilly06/LHDC-V5-Decoder`, pinned in `lhdc.pin`. Since 24.10.2026 (customer's
decision, before the repository was opened) it is not in the repository:
`tools/setup.sh --with-lhdc` fetches it into `components/lhdcv5_decoder/`, outside git, and the
codec is built only with the `sdkconfig.lhdc` overlay. Where it comes from, on what terms and why it is not carried:
`docs/LHDC_UPSTREAM.md`. The A2DP glue was adapted into `bt_overlay/` (`a2dp_vendor_lhdcv5*.c`) and
stayed in the repository; patch 0003 makes room for the endpoint and adds the configuration check.

- Identifiers: vendor `0x0000053a`, codec `0x4c35`, capability element length 13.
- Offered: 44.1 / 48 / 96 kHz, stereo, 16- or 24-bit source, 5 ms frames. **Not** offered: 192 kHz,
  32-bit source, lossless, LL, spatial processing, the META/AR/JAS bits.
- The phone's configuration must name exactly one rate out of those the host allows, a depth of 16
  or 24 and no optional bit; otherwise it is refused. The device mirrors the phone's minimum and
  maximum bitrate: without that there was no sound at 256 kbit/s.
- Decoding on one core. Reassembly of fragments up to 3072 bytes, only from a first fragment, with a
  counter that falls by one and consecutive RTP sequence numbers; any gap discards the part. Every
  frame is checked (return code, bytes consumed, samples, channels, depth, rate), and a mismatch
  resets the decoder and drops the packet. A failed configuration leaves the decoder off, that is,
  silence.
- **What is not verified.** The decoder's arithmetic has not been compared with a reference decoder:
  checking the original by encoding and decoding needs the proprietary encoder, which was not
  carried over. The host tests check bounds and lifetime, not sound.

### Cover art (protocols 1.5–1.7, 20–22.09.2026)

The device takes the linked thumbnail from the phone over AVRCP Cover Art (OBEX) and streams it
to the host: the rules are in `PROTOCOL.md`, section 10.

- OBEX connects only when the phone has announced the Cover Art feature in its AVRCP target
  capabilities. After OBEX connects, the device reads the metadata again, because Android may not
  give the image handle before that.
- One OBEX GET at a time. A response still arriving after a track change is read to the end and
  thrown away, and then the new track's image is taken. A failed transfer also disconnects OBEX. If
  it is the draining of the old response that failed, the device connects again after OBEX closes
  and takes the new track's image: not a byte of it has gone to the host yet.
- OBEX MTU 512, and the stack pulls one packet every 50 ms from a BTA timer, without SRM, so that
  the phone does not flood cover data beside A2DP (patch 0004). An empty final OBEX response
  completes the object (patch 0005).
- **Streaming only, no assembly.** The 1.5 path, which assembled images of up to 48 KiB, took heap
  in 1000-byte pieces in competition with the decoder, and on the bench every assembled image was
  followed by bursts of `ring full`. So since 21.09.2026 every OBEX piece goes straight into one
  bounded UART frame, and nothing is kept. A piece is not queued if the droppable queue already
  holds more than 4 frames or less than 16 KiB of heap would be left; the image is then cut short.
  A host below 1.6 gets no cover art at all: an empty tile is better than the clicks of a starving
  decoder.
- The phone does not report the size (the stack's callback carries only a status, a final flag and
  the piece), so `total_bytes` = 0, and `COVER_END` carries the length and CRC.

---

## I2S output

- Lines: `BCLK`, `WS`, `DOUT`. **No MCLK is needed or output.** Philips I2S format, stereo.
- **Who clocks** (protocol 1.9, 30.09.2026). The device starts as **slave** and drives DOUT only; a
  host with bit 8 clocks the link itself. For a host without bit 8 the device becomes **master** and
  generates `BCLK` and `WS`.
- **Rate and width** are announced over UART **before the first sample** and before every change,
  not guessed by the host. Rates, slot widths and how the bits land in them: `PROTOCOL.md`,
  section 6.
- **A codec can carry more than fits in a slot.** aptX and aptX HD give 24 bits; in 16-bit slots
  they are narrowed with triangular dither, not by dropping the low byte. Dropping it shifts every
  sample the same way and leaves an error correlated with the signal, which on quiet passages is
  heard as a tone. 32-bit slots remove the question together with the narrowing.

### Master

The device receives a stream whose pace is set by the phone's crystal. As master it controls its
own APLL and follows the drift.

**Trim** (stage 10.1, 21.09.2026). Once a second the average fill of the ring over that second is
compared with the middle of the ring, and MCLK is adjusted through `i2s_channel_tune_rate()`: a PI
controller whose error is in milliseconds of audio (the same dynamics at 44.1/16 and 96/32), 10 ppm
per millisecond, an integrator with a 400 s time constant, ±200 ppm of authority, steps of at least
2 ppm. It works only while streaming with the audio open, freezes between tracks and resets on a
format change; the applied ppm is in the report every 10 s. Two loops in series: this one follows
the phone, the host's ASRC (±500 ppm) follows this device's BCLK, and the host's loop must stay the
faster one. The driver rewrites the APLL coefficients live at every step, and `audio_out.c` holds a
reference of its own to the APLL, without which IDF 5.5.5 turns the PLL off in the middle of a
stream. A build with `-DAUDIO_DIAGNOSTIC_FIXED_TRIM=ON` turns the trim off.

**Measured on the board itself** (`CONFIG_SAT_CLOCK_PROBE`: the pulse counter is fed by the BCLK
pin's output, with no wire and no oscilloscope). With no phone and no stream, BCLK ran continuously
at 1,411,256 Hz for twenty-five seconds, and the data line carried zeros, not a repeated buffer.
Nominal is 1,411,200 Hz, so the clock runs **40 ppm fast**, about 143 ms of drift per hour. Both
clocks of the measurement come from the same crystal, so this is the APLL's own fractional error;
the crystal's error adds to it. The exception: a rate change turns the channel off and on, and for
that moment the clock really stops.

### Slave (protocol 1.9)

The host's clock does not depend on the phone, so the device absorbs the drift between them: it
resamples to the host's clock (`main/audio_asrc.c`). Slots are always 32 bits, and the samples on
the wire are no longer bit for bit what the decoder gave. The audio reserve is held at 150 ms.

### Audio path

- The PCM ring is 24 KiB of internal memory: 139 ms at 44.1/16, 32 ms at 96/32.
- Only the decoder task may write into the ring, in 1024-byte pieces. Per packet, 100 ms including
  the decoding; waiting for space goes in 10 ms slices, each checking whether the audio is still
  open and whether this is the same generation. No other caller ever waits: its PCM is dropped.
- A format transition closes the audio before anything is announced, and every codec or disconnect
  event increments the epoch: the audio opens only if the confirmation refers to the latest epoch.
  A Bluetooth callback never waits for a task, a mutex or the UART.
- While it waits, I2S runs and carries zeros at the DMA's pace. Playback starts after 12 ms of PCM
  (as master); as slave, when the ring and the queue hold half the reserve, or after 250 ms.
- A DMA write that has not succeeded within 100 ms resets the DMA on the same format. If that fails
  too, the device closes the audio until reboot: the driver may have freed the old buffers without
  allocating new ones.

---

## Control link

UART **460800 8N1**, no hardware flow control, COBS frames with CRC-16. Everything byte by byte:
`PROTOCOL.md`, version 1.10.

D = device, H = host.

| Code | Direction | Content |
|---|---|---|
| `0x01` HELLO | both | protocol version, build, capability mask |
| `0x02` PING / `0x03` PONG | both | liveness, once a second |
| `0x10` BT_STATE | D → H | connection state, codec, the phone's address and name |
| `0x11` TRACK_META | D → H | title, artist, album |
| `0x12` TRACK_POS | D → H | play state, elapsed ms, total ms |
| `0x13` STREAM_FMT | D → H | rate, slot width, channels. **Before the first sample** |
| `0x14` COVER | D → H | obsolete cover art of 1.0–1.4; the device does not send it |
| `0x15` VOLUME_STATE | D → H | the volume changed, and by whom |
| `0x16` STREAM_PREPARE | D → H | a confirmed format transition (1.3) |
| `0x17` COVER_RESET | D → H | a new cover image (1.5) |
| `0x18` COVER_DATA | D → H | a piece of JPEG (1.5) |
| `0x19` COVER_END | D → H | end of the image, length and CRC (1.6) |
| `0x1A` PAIR_REQUEST | D → H | a pairing request (1.8) |
| `0x1B` PAIR_RESULT | D → H | how the pairing ended (1.8) |
| `0x1C` BT_CONFIG | D → H | the stored Bluetooth settings and what the build has (1.8) |
| `0x1D` BOND_LIST | D → H | the paired phones (1.8) |
| `0x20` CMD_TRANSPORT | H → D | play, pause, stop, next, prev, seek |
| `0x21` CMD_VOLUME | H → D | AVRCP absolute volume, 0..127 |
| `0x22` CMD_RADIO | H → D | radio, visibility, drop the connection |
| `0x23` STREAM_READY | H → D | the host is ready for the format, or refuses (1.3) |
| `0x24` COVER_RETRY | H → D | send the cover art again (1.7) |
| `0x25` PAIR_REPLY | H → D | yes or no to a pairing request (1.8) |
| `0x26` BT_CONFIG_SET | H → D | new Bluetooth settings, the full state (1.8) |
| `0x27` BOND_REMOVE | H → D | forget one paired phone, or all (1.8) |
| `0x28` BT_REFRESH | H → D | send `BT_CONFIG` and `BOND_LIST` (1.8) |
| `0x40` LOG | D → H | level and text |

Codes `0x30`–`0x35` belonged to the port expander before 1.9; from 1.10 they mean nothing and are
not reused, and the device counts them as an unknown type (`PROTOCOL.md`, section 11).

**Bench 24.10.2026, device 1.10 with panel 1.9.** The device announced caps `0x1cf` (no bits 4 and
5), the panel `0x1fb` (without its bit 2), and the panel sent no expander commands: `unknown` 0. In
90 s from reset no `PING` went unanswered, `bad frames` and `gaps` were 0, round trip 3–6 ms.
Without `GPIO_MAP` the panel confirmed the switch to LHDC V5 96 kHz / 32 bits, got the metadata and
the cover art (40,613 bytes), and the audio on it played normally (the customer). A panel that sends
expander commands has not been tested on the bench: this one does not send them.

**Outgoing queues** (11.10.2026, `main/link.c`, `main/tx_slots.h`). The droppable `TRACK_POS` and
`LOG` wait in a 10-place queue; when it is full, the oldest is lost. Cover art pieces are in a
queue of their own: a full queue refuses a new piece, the image is cut short, and a repeat is the
business of `COVER_RETRY`. Nothing else is lost: every type has one place in static memory, and a
message not yet sent is replaced by a newer one of the same type, so the host gets the freshest
state rather than a stale one. These places go first, in order of their last update. They take no
heap; the price is about 2.4 KB of static memory.

---

## Behaviour

1. **Never block.** If the host stops reading the UART, drop the oldest `TRACK_POS`, `LOG` and
   `COVER`. Do not drop `BT_STATE` or `STREAM_FMT`.
2. **Audio matters more than telemetry.** No work on the link may cause a gap in the I2S stream.
3. **A silent host is not a failure.** No `PONG`: keep playing and keep accumulating state. The
   exception: a format transition waiting for confirmation stays silent until it is confirmed; a
   missing `PONG` or `STREAM_READY` never lets a new format sound.
4. **Logs go into `LOG`**, not only to the UART console.

---

## Out of scope

No screen. No network. No files. No web interface. No OTA. No GPIO for the host (since
24.10.2026). Add none of these.

---

## Known deviations from the protocol

What the firmware currently does differently from what `PROTOCOL.md` or the "Behaviour" section
promise. Found by reading the code on 11.10.2026; the rest was fixed that day, and this was left on
purpose.

1. **A host with a different `proto_major` after a compatible one.** Such a host's commands are not
   carried out, and it gets no cover art, but the previous compatible host's capabilities stay: a
   confirmed format transition will wait for a `STREAM_READY` that will not come, that is,
   silence; pairing will ask a host that will not answer, and refuse after 25 s; the I2S role stays
   the one the previous host set. **Left so by the customer's decision of 11.10.2026.** Resetting
   the capabilities to "no host" would mean becoming I2S master (`PROTOCOL.md` 3.5), and an
   incompatible host may be clocking the line itself — two drivers on one line are worse than
   silence. The case is rare: a host reflashed to another `proto_major` while the device runs.

---

## Not yet verified

- **BCLK continuity during an APLL trim step**, with an oscilloscope on the bench.
- **Codec switching without reconnection after patch 0002**: SBC ↔ aptX HD several times, 15–20 s
  per codec, both ways, and a reconnection separately as a control. Playback on the bench after the
  fix is not recorded. Sign of a regression: BTC in the IDLE state logs that it ignored
  `BTC_AV_CONFIG_EVT`, `BTA_AV_OPEN_EVT` and `BTA_AV_START_EVT`, the host shows a rate of 0, and
  only a reconnection helps. That the application really saw the new codec shows in the
  `format ready` line, not in the decoder's `configure` line.
- **Format transitions**: 44.1 ↔ 48 kHz and 16 ↔ 32 bits, restarts of both sides, link loss,
  switching the source on the host while waiting, a codec change under load, the DMA counters and
  the I2S line itself.
- **LHDC V5**: 96 → 48 → 96 kHz without reconnection; LHDC → SBC → aptX HD → LHDC without
  reconnection; pause and resume, phone reconnection, both reboot orders; memory headroom with
  cover art and LHDC at once. LHDC 96 kHz / 32 bits played on the bench (30.09.2026).
- **The fixes of the deviations, 11.10.2026**, verified only by host tests and the build: `LOG`
  levels on the panel; silence of a 48 kHz stream when a host without RELOCK says hello, and audio
  after the phone reconnects; the new outgoing queues under load (cover art and LHDC together): in
  the link report `coalesced` grows, and `dropped` stays 0 for the non-droppable ones.
- **The bare module with 1.9 code.** The host flashed the `sdkconfig.bare` build through the ROM
  bootloader and brought the link up at 460800 on 08.10.2026, still on protocol 1.8 code. With the
  1.9 I2S slave and the outgoing queues of 11.10.2026 it only builds.
- **The fixes from the deep review of 12.10.2026**, verified only by host tests and the build of
  every variant:
  - codecs: SBC without mono/dual in the sink's capabilities (a phone set to "Mono" must switch to
    Joint/Stereo, with no `Decoding failure: 101`); the strict SET_CONFIGURATION check for SBC, AAC,
    aptX, aptX HD and LHDC; skipping a codec without stereo when the device initiates the
    connection itself; AAC 44.1/48 kHz with no new `Decode error` after Helix was hardened;
    `seqgap` in LHDC on radio loss;
  - audio: silence on DOUT between an I2S role change and the next format announcement; the
    control queue up to `s_lock` (the worst wait) during the first connection as slave and during
    codec and rate switches; the clock probe in the slave role does not enable the BCLK and WS
    output drivers;
  - link: `HELLO` repeated exactly once a second with no host; bytes left in the UART with no
    event; `LOG` from several tasks at once;
  - Bluetooth: a pause with play on the phone within 5 s; only the own phone's name in `BT_STATE`,
    up to 32 bytes of UTF-8; `TRACK_META` from a single track, and empty after disconnection; an
    unknown position as 0; the decision about the phone by the live state of the radio; repeated
    cover art after a failed drain, and the OBEX retry limit.
- **Host tests** are pure logic: no FreeRTOS scheduler, no real UART and I2S, and no listening, so
  they say nothing about clicks. A passing test is not a bench result.

---

## Open questions for the customer

None that block.
