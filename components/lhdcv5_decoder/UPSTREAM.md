# LHDC V5 decoder provenance

Facts as found on 2026-09-21. This file records what is known about where the
code came from and under what terms; it draws no legal conclusion.

- Source: https://github.com/WillyBilly06/LHDC-V5-Decoder
- Pinned commit: `676af03d2b7a2ed5c1f9bdca7bf8938cc85f704b`.
- Imported: `decoder/`, the component `CMakeLists.txt` and `README.md`, byte for
  byte. Not imported: the bundled IDF tree, applications, the proprietary encoder,
  binaries, the LDAC decoder. The A2DP glue was adapted into `bt_overlay/`
  (`docs/DEVICE_SPEC.md`, section "LHDC V5").

## Licence: what the upstream repository actually contains

- **No licence file.** The repository at the pinned commit ships `CMakeLists.txt`,
  `README.md`, `idf_component.yml` and the directories `WROVER_A2DP_Sink/`,
  `a2dp_integration/`, `decoder/`, `docs/`, `esp-idf-v6.1-codecs/`, `test/`. There is
  no `LICENSE`, `LICENSE.md`, `COPYING` or `NOTICE` at any level that was imported.
- **The only licence statement is one sentence in `README.md`** (section "License"):
  "This decoder implementation is released under the **Apache License 2.0**. Note
  that this does **not** grant any rights to the LHDC codec itself, which is the
  property of Savitech; obtaining LHDC licensing for your use case is your
  responsibility."
- **The README's own disclaimer** (top of the file): "LHDC is a proprietary
  high-resolution Bluetooth codec developed by Savitech. This repository is an
  independent decoder implementation for interoperability/research. It contains no
  Savitech/Qualcomm binaries. You are responsible for ensuring you have the rights
  to use LHDC in your jurisdiction and product."
- **The decoder sources carry no copyright line and no SPDX identifier.** None of the
  files under `decoder/` names an author, a year or a licence.
- **`decoder/lhdc_tables.c` states its own provenance**: its header reads "REAL
  EXTRACTED DATA FROM liblhdcv5.so (LHDC V5 Encoder Binary)", i.e. the band, window
  and quantisation tables were extracted from Savitech's proprietary encoder
  library, not derived from a specification.

## What this repository used to say, and why it no longer does

Until 2026-09-21 this directory carried a `LICENSE` file with the Apache-2.0 text.
It was not from upstream: `vendor-review/integrate_lhdc.py` copied ESP-IDF's own
`LICENSE` into place, and the copy still had the template's
`Copyright [yyyy] [name of copyright owner]` line unfilled. A licence text that the
upstream author never shipped, attributed to nobody, documents nothing, so it was
removed. The README sentence above is the whole of the upstream grant, and this file
is where that fact is kept. See also `THIRD_PARTY_NOTICES.md` at the repository root.
