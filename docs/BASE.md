**English** · [Українська](BASE.uk.md)

# The base: how the Bluetooth stack patches are carried

Stock Bluedroid in A2DP sink mode can do **SBC only**. Everything else — AAC, aptX, aptX HD —
needs changes in the stack itself. This document is about how to keep them.

---

## The decision

**Pinned stock IDF 5.5.5 + a patch series in this repository + a script that makes a local
`components/bt/`.** The shared IDF on disk is never touched.

```
repo/
  idf.pin            # v5.5.5 and the tag's hash
  patches/*.patch    # real unified diffs, changes to existing files only
  bt_overlay/        # files that are entirely ours: codec framework, decoders, libfreeaptx, Helix
  tools/setup.ps1    # Windows; tools/setup.sh is the same for Linux and CI
  components/bt/     # in .gitignore, made by the script
```

A fresh clone:

1. Activate stock ESP-IDF 5.5.5. The script checks the version against `idf.pin` and fails if it is
   not that one.
2. Copy `$IDF_PATH/components/bt` → `./components/bt`.
3. `git apply patches/*.patch` onto `./components/bt`. **Fail loudly** on any mismatch. (`--3way`
   does not work: `components/bt` is in `.gitignore`, its blobs are not in the object database,
   and there is nothing to merge with.)
4. Copy `bt_overlay/` over `./components/bt/`.
5. `idf.py set-target esp32 && idf.py build`.

**Why new files are not in the patches.** A patch that adds 15,000 lines of someone else's codec
cannot be read, and it hides the licence headers — the very thing we look at to see which codec
came from where. As files they can be read, assessed and compared with upstream. Only what changes
existing Bluedroid code goes into `patches/`.

To raise the IDF: change `idf.pin`, rerun the script, resolve the conflicts it names, commit the
refreshed patches. `git diff` over `patches/` is our whole delta.

---

## Why not otherwise

**Do not vendor the whole IDF tree.** That is what the successor fork we take the patches from
does: its root holds the whole of IDF 5.5.2, with no submodule and no pins, plus a committed
860 KB controller binary. The result: there will never be a 5.5.6, and its own changes can only
be found by a block-by-block diff against stock, because nobody recorded them.

**Do not patch the shared IDF on disk.** `C:/esp/v5.5.5/esp-idf` is clean now, 1037 of 1037 blobs
match the official tag, and it is shared with other projects. A script that edits it breaks them.

**Do not use a component override alone, without patches.** The override works (see below), but
silently: a frozen copy of `bt` hides the new one, and every IDF update passes by without a word.
A patch series gives a loud error instead of a quiet divergence.

---

## Component override — officially supported

ESP-IDF v5.5, Build System guide, section "Multiple Components with the Same Name": a component in
the project directory takes precedence over the IDF component of the same name, and this is named
outright as the way to override an IDF component with a modified version. After moving it,
`idf.py reconfigure` is needed.

What has been checked about `bt` specifically:

- **The binary blobs stay valid.** `libbtdm_app.a` is the **controller**; it is below the VHCI
  boundary. All the logic of A2DP, AVDTP, L2CAP and the codecs is the host, that is, source code. A
  patch to the host does not break the controller.
- **The linking is relocatable:** the controller is linked through
  `${CMAKE_CURRENT_FUNCTION_LIST_DIR}/lib_esp32/esp32`, so the copy carries its blob with it.
- **One trap with an absolute path.** In
  `components/bt/common/ble_log/extension/log_compression/CMakeLists.txt` there is
  `$ENV{IDF_PATH}/components/bt`. It is read only under `CONFIG_BLE_MESH_COMPRESSED_LOG_ENABLE` or
  `CONFIG_BLE_HOST_COMPRESSED_LOG_ENABLE`, both off by default. **Do not turn them on.** Otherwise
  the build will pull files from the stock tree, not from ours.
- Size: `components/bt` is 71 MB and 1037 files. That is why it is in `.gitignore`.

---

## What it really costs

The successor's delta against stock `bt`: **44 changed files, 2020 added**. Six files alone have
1008 changed lines: `bta_av_co.c` 518, `btc_av.c` 427, the rest small. The changes reach far beyond
the codecs — `l2c_csm.c`, `l2c_link.c`, `avdt_msg.c`, `avct_l2c.c`, `hci_hal_h4.c`, `osi/thread.c`.
This is a real fork of the stack, not a codec wrapper.

The older repository, where we once took the figure "9 lines of diff plus 6 in btc_av.c" from —
**that figure is wrong by a factor of about 150**. In reality it is 23 changed files and 2201
changed lines, 121 of them in `btc_av.c` itself, not 6.

The cost of upkeep, measured on a real update. Between **v5.5.4 and v5.5.5**, 460 of 1033 files
changed in `bt`, and of the 23 files the patch touches, **14 changed in that same patch release**,
272 lines. So every IDF raise will give conflicts in about half of the patches.

That is not a reason not to do it. It is a reason to **pin the IDF and not move it without a
reason**. For a personal device that reason may never come.

---

## There is no official way without patches

Stock 5.5.5 has `CONFIG_BT_A2DP_USE_EXTERNAL_CODEC` with `esp_a2d_sink_register_stream_endpoint()`,
and Espressif calls it the recommended path for new development. But it is **SBC-only by
construction**, three independent blocks:

1. `btc_av_reg_sep()` in `btc_av.c` refuses everything but `ESP_A2D_MCT_SBC`. The header says so
   itself: "currently only supports SBC".
2. `ESP_A2D_MAX_SEPS == 1` — one endpoint, so announcing SBC and a vendor codec at the same time is
   impossible in principle.
3. `esp_a2d_mcc_t.cie` is a 7-byte union of sbc/m12/m24/atrac, **with no vendor member**. An aptX
   capability (4 bytes of vendor ID plus 2 bytes of codec ID) does not fit into that structure.

The constant `ESP_A2D_MCT_NON_A2DP` exists, but only as a **reporting** type. There is no
registration hook for it.

Also: `CONFIG_BT_A2DP_LDAC_DECODER`, `CONFIG_BT_A2DP_APTX_DECODER` and `CONFIG_BT_A2DP_AAC_DECODER`
**do not exist** in stock 5.5.5. The patch introduces them. Stock has only `BT_A2DP_ENABLE` and
`BT_A2DP_USE_EXTERNAL_CODEC`.

---

## A second trap with an absolute path, and it is worse

`components/bt/Kconfig` pulls in the submenu like this:

```
source "$IDF_PATH/components/bt/host/bluedroid/Kconfig.in"
```

That is the **shared tree**, not our copy. So **any patch to `Kconfig.in` under `components/bt/` is
read by nobody**. There will be no error: the build says `unknown kconfig symbol` in one line among
hundreds and carries on with the codec off.

`components/bt/Kconfig` itself is read from the component, so the codec options live in it.
Checked: before the move `CONFIG_BT_A2DP_APTX_DECODER` did not exist, after it, it works.

---

## What the build has verified

- **The override works.** `build/project_description.json` shows `bt` at our path, not at the IDF's.
  It builds, with no warnings.
- **The controller is unharmed.** The blob is linked by a relative path and travels with the copy.
- **The flash cost of the codecs:** SBC alone 0xfe710; with the framework, aptX, aptX HD and AAC,
  0x10f580. That is **+68 KB**.

## Not verified

- Whether the controller libraries for other chips can be dropped from the `bt` copy, so as not to
  drag 71 MB along. It looks safe (`register_bt_ctrl_libs()` branches on `CONFIG_IDF_TARGET_*`), but
  it is not verified.
- Whether aptX and AAC negotiate with a real phone — verified since: SBC, aptX and AAC played on the
  hardware, each on its own, with the codecs above them disabled (a comment in `main/bt_av.c`).
- The 460/1033 figure is inflated by the large `ble_log` feature, which arrived in 5.5.5 exactly. The
  figure that matters is 14 of 23 files and 272 lines.

---

## What was taken from the successor, and what was not

The successor's delta against stock is two different pieces of work in one tree.

**Taken: the codec architecture.** The model from Android — `a2dp_codec_config.c`, `a2dp_vendor.c`,
the decoder interface, one file per codec. `btc_a2dp_sink.c` stops calling SBC directly and goes
through `decoder->decode_packet()`. `bta_av_main.c` iterates over codec indices. `bta_av_co.c`
negotiates through `A2DP_BuildCodecConfig`.

**Not taken: workarounds for the author's particular adapter.** Their own comments say "for Realtek
interop". What was left out:

- `l2c_csm.c` **turns off the L2CAP security check** for PSM 0x0019 and 0x0017;
- `avdt_api.c` reaches into `tL2C_CCB` on a timer and sets `chnl_state = CST_OPEN` behind L2CAP's
  back;
- `avct_l2c.c`, `avdt_l2c.c` remove the MTU from ConfigReq;
- `hci_hal_h4.c`, `packet_fragmenter.c` turn `assert(0)` on allocation failure into silently
  dropping the packet;
- `thread.c` puts task stacks in PSRAM, which a WROOM does not have;
- `TRACE_DEBUG` → `TRACE_WARNING` in dozens of places.

**Not taken: the rollback of `btc_av.c` to 5.5.2.** Their version of that file removes what
Espressif added after 5.5.2: cover art (`BTA_AV_CA_STATUS_EVT`, `BTA_AV_CA_DATA_EVT`), `conn_hdl` and
`audio_mtu` in the connection reports. We hand cover art to the host, so the file is taken from
stock, with only the two changes that are needed on top: generalised parsing of the codec
configuration, and answering a codec renegotiation in the OPENED state.

**Not taken: LDAC, Opus, LC3plus, aptX LL.** LDAC has no licence. The rest are not in
`DEVICE_SPEC.md`, and an endpoint the device announces but cannot name to the host is worse than
none.

aptX LL was carried over on 11.10.2026 (with protocol 1.10) and reverted the same day: there is no
source that would send it. The work stays in the history — `7727564` and `7f983dc`.

## RTP parsing moved into the decoders — together with its checks

Stock `avdt_scb_hdl_pkt_no_frag()` strips the RTP header itself. With several codecs that will not
do: plain aptX has no header, aptX HD has one, AAC has one with LATM on top. The parsing has to be
in the decoder.

But between 5.5.2 and 5.5.5 Espressif hardened that function — every field that moves the offset is
checked against the packet length. The successor was cut from 5.5.2 and carried over the version
**without the checks**, with two out-of-bounds reads on top: `(uint16_t*)(header + header_len + 2)`
multiplies the arithmetic by the size of the type, and the padding byte is read from `len`, not
`len - 1`.

So the checks moved together with the parsing, into the shared `a2dp_rtp_strip_header()`
(`bt_overlay/host/bluedroid/stack/a2dp/a2dp_rtp.c`). Every decoder goes through it.
