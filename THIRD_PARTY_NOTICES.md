**English** · [Українська](THIRD_PARTY_NOTICES.uk.md)

# Third-party code in the satellite image

What is linked into `modesp_bt_satellite.bin`, where it comes from and the licence
**as found in the files**, on 2026-09-21 and again on 2026-10-24, before the
repository was opened. This is an inventory of facts for whoever has to decide what
they mean; it is not legal advice and draws no conclusion.

| Body of code | Where in this tree | Licence as found in the files | Notes |
|---|---|---|---|
| ESP-IDF v5.5.5 (`b774170ff46c393eeb5e495ea37936038d3f4f4f`) | shared checkout named by `idf.pin`; not in this repository | `LICENSE` at the IDF root: Apache-2.0. Components inside the IDF carry their own notices (FreeRTOS kernel, newlib, mbedTLS and others); the IDF tree is authoritative for those | Everything below `components/bt` is copied out of it by `tools/setup.ps1` / `tools/setup.sh` |
| ESP32 Bluetooth controller, `libbtdm_app.a` | copied into `components/bt/controller/lib_esp32/` | `LICENSE` in that directory: Apache-2.0; `README.rst`: "Copyright (C) 2015-2016 Espressif Systems ... provided under the same license as the parent esp-idf project - the Apache License 2.0 ... (The license text refers to this as 'Object' form.)" | Binary only. Below the VHCI boundary; the patches do not touch it |
| Bluedroid host stack | copied into `components/bt/host/bluedroid/`, then `patches/0001`..`0011` applied | File headers: "Copyright (C) 2004-2012 Broadcom Corporation", "Copyright (C) 2014 Google, Inc.", Apache-2.0 | The patch series is this project's; its content is a delta derived from `WillyBilly06/ESP32-A2DP-SINK-WITH-CODECS-UPDATED` (`docs/BASE.md`) |
| Multi-codec A2DP framework (codec config, vendor codec parsers, decoder shims) | `bt_overlay/host/bluedroid/stack/a2dp/`, `.../stack/include/stack/` | File headers: "SPDX-FileCopyrightText: 2016 The Android Open Source Project" (Apache-2.0), "SPDX-FileCopyrightText: 2024 ESP32 A2DP Project" (Apache-2.0), and `SPDX-License-Identifier: Apache-2.0` alone on `a2dp_vendor_aptx_decoder.c`, `a2dp_decoder.h` and `a2dp_vendor_aptx_decoder.h`. `a2dp_rtp.c` was written here: "SPDX-FileCopyrightText: 2026 Zapadenec1982", Apache-2.0 | Taken from the donor fork `WillyBilly06/ESP32-A2DP-SINK-WITH-CODECS-UPDATED`. Its README ends "MIT License. See the license file for details.", but it ships no licence file of its own, only the vendored IDF's Apache-2.0 `esp-idf/LICENSE`; the files themselves say Apache-2.0. Every file changed in this project carries "SPDX-FileContributor: 2026 Zapadenec1982", the notice Apache-2.0 §4(b) asks of modified files; a file without it is as the donor had it. The five `a2dp_vendor_lhdcv5*` files were adapted from the LHDC decoder repository's `a2dp_integration/`, whose files carry no notice at all; each now says so, with `SPDX-License-Identifier: Apache-2.0` after the upstream README and an `SPDX-FileContributor` line for the changes made here. They stay in this repository: they hold none of the data extracted from Savitech's encoder (see the LHDC row) |
| Helix fixed-point AAC decoder (RealNetworks, 2005) | `bt_overlay/host/bluedroid/external/libaac-lc/libhelix_aac/` | Every source file: "Portions Copyright (c) 1995-2005 RealNetworks, Inc. ... subject to the current version of the RealNetworks Public Source License (the 'RPSL') ... unless you have licensed the file under the current version of the RealNetworks Community Source License (the 'RCSL')", with URLs at helixcommunity.org. The copy taken from the donor shipped no licence text file; `readme.txt` is a technical overview and says nothing about terms | Built when `CONFIG_BT_A2DP_AAC_DECODER=y` (the default in `sdkconfig.defaults`). **The RPSL text is `libhelix_aac/RPSL.txt`**, added on 2026-10-24 because §2.1(b) wants a copy with every copy of the source; it is the text as copies of the Helix sources ship it, identical in `pschatzmann/codec-helix`, `ultraembedded/libhelix-mp3` and `Panda381/PicoLibSDK` (one typo, "wtih", included). The three glue headers the Helix sources include, `ConfigHelix.h`, `utils/helix_pgm.h` and `utils/helix_memory.h`, are this project's own, MIT, written on 2026-10-24: the files they replace were adapted from files of the same names in `pschatzmann/arduino-libhelix`, a GPL-3.0 repository, and RPSL Exhibit B forbids applying GPL terms to Helix. The wrapper `libaac-lc/aac_decoder_helix.c` is the donor's; it named no licence, and now says so and where it came from. **Locally modified** on 2026-10-12 (commits `21876ec`, `54ce0fc`): `noiseless.c`, `decelmnt.c`, `bitstream.c` and `coder.h` gained bounds checks that reject malformed frames (`max_sfb`, section lengths, pulse positions, negative bit counts, TNS order, CPE M/S mask end), and the gain-control parser in `noiseless.c` was removed in favour of rejecting the frame. RPSL §2.1(c) asks that each modified file keep the Exhibit A notice and state that it was changed and when: the licence block is left untouched, and each modified file carries a dated change note in its description comment below it |
| libfreeaptx (aptX / aptX HD) | `bt_overlay/host/bluedroid/external/libfreeaptx/main/` | File headers: "Copyright (C) 2017 Aurelien Jacobs, 2018-2020 Pali Rohár, 2021 Hunter Wardlaw ... GNU Lesser General Public License ... version 2.1 ... or (at your option) any later version"; full text in `main/COPYING` and in `LICENSE` beside it | LGPL-2.1-or-later, statically linked into `libbt.a`. Carried as the donor had it, unchanged. `tools/relink_kit.ps1` produces the relink kit for a release |
| LHDC V5 decoder | **not in this repository.** `tools/setup.sh --with-lhdc` fetches it at the commit in `lhdc.pin` into `components/lhdcv5_decoder/`, which git ignores | **No licence file upstream.** The upstream `README.md` says the implementation "is released under the Apache License 2.0" and that this "does not grant any rights to the LHDC codec itself, which is the property of Savitech". The sources carry no copyright line and no SPDX identifier; `lhdc_tables.c` says its tables are "REAL EXTRACTED DATA FROM liblhdcv5.so (LHDC V5 Encoder Binary)" | `docs/LHDC_UPSTREAM.md` has the detail. Built only when fetched and `CONFIG_BT_A2DP_LHDCV5_DECODER=y`, which `sdkconfig.lhdc` sets; off by default |
| LDAC | not present | — | Deliberately excluded: the only decoder in circulation (`external/libldac-dec` in the donor fork) ships without copyright or licence, and Sony publishes an encoder only (`CLAUDE.md`, `docs/BASE.md`) |
| This firmware (`main/`, `components/satlink/`, `patches/`, `tests/`, `tools/`, docs) | this repository | **MIT**, `LICENSE` at the root: "Copyright (c) 2026 Zapadenec1982" | Since 2026-10-24. A file with a licence notice of its own keeps it, changes made here included; the patches change Bluedroid, which stays Apache-2.0 |

How to re-check any row: the licence column quotes the files, so `grep -rn -i
"copyright\|SPDX\|licen" <path>` on the row's directory reproduces it.

## A binary release

What the terms above ask of whoever hands out a built image rather than this source.
Recorded here, not resolved. A CI workflow artifact of a public repository would be
such a hand-out, so the CI keeps none.

- **Apache-2.0** (ESP-IDF, Bluedroid, the controller, the codec framework): a copy of
  the licence goes with the image (§4(a)).
- **LGPL-2.1-or-later** (libfreeaptx, statically linked): the library's source, and
  what it takes to relink the image against a changed build of it (§6).
  `tools/relink_kit.ps1` collects that.
- **RPSL 1.0** (Helix AAC, in every image built with `sdkconfig.defaults`): §2.1(e)
  wants a prominent notice, "in the code itself as well as in related documentation",
  that the source is available under the RPSL and where, and Exhibit A's Object Code
  Notice wherever copyright notices are shown: "Helix DNA Client technology included.
  Copyright (c) RealNetworks, Inc., 1995-2002. All rights reserved." The image does
  not carry that notice today. §4.2 also asks that code combined with Helix be under
  one of the licences in Exhibit B and available as source; the image also links
  Espressif's binary-only libraries, the Bluetooth controller `libbtdm_app.a` among
  them.
- **LHDC V5**: this repository hands out no image with the decoder in it.
