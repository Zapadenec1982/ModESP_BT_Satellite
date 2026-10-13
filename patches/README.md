# Bluetooth stack patches

Apply in filename order to the exact ESP-IDF component pinned in `idf.pin`,
then copy `bt_overlay/`. `tools/setup.ps1 -Clean` reproduces the local ignored
`components/bt`; never patch the shared IDF or rely on edits to generated files.

- `0001-a2dp-multi-codec-framework.patch`: codec framework, SBC/AAC/aptX/aptX HD
  integration and project adaptations. See `docs/BASE.md` for provenance/exclusions.
- `0002-notify-btc-on-queued-stream-reopen.patch`: complete the existing incoming
  configuration notification when CONFIG is dispatched from INIT after a queued
  CLOSE. This prevents BTC remaining IDLE during a peer codec switch.
- `0003-add-lhdcv5-decoder.patch`: builds the LHDC V5 endpoint and decoder
  (`CONFIG_BT_A2DP_LHDCV5_DECODER`, the pinned decoder in
  `components/lhdcv5_decoder`) and makes room for one more stream endpoint. A peer
  may announce LHDC capabilities as masks, but SET_CONFIGURATION must select exactly
  one format; 0003 adds that check to `bta_av_co.c` with the LHDC validator. 0010
  deletes it again: the sink's SET_CONFIGURATION check is now
  `A2DP_IsSinkCodecSupported` for every codec, whose vendor dispatcher
  (`A2DP_IsVendorSinkCodecSupported` in `bt_overlay`'s `a2dp_vendor.c`) calls the
  same LHDC validator.
- `0004-pace-cover-art-obex.patch`: cover-art downloads pull one OBEX packet at a
  time, 50 ms apart, from a BTA timer (so no Bluetooth task sleeps), and no longer
  ask for SRM, which would let the phone flood cover data alongside A2DP. The
  pending continuation is cancelled on open, close, a new GET and disconnect.
- `0005-report-empty-cover-final.patch`: a successful OBEX final response with no
  body still completes the object. Stock BTA freed it silently, so an image whose
  last bytes came in the previous packet never finished.
- `0006-clean-up-sink-after-stopping-media-thread.patch`: 0001 gives the sink a
  media thread of its own and shuts it down by posting the cleanup and freeing the
  thread at once. This osi drops queued work when a thread stops, so unless the
  thread won that race the cleanup never ran: an `esp_a2d_sink_deinit()`/init
  pair, including the endpoint re-registration in `main/endpoint_policy.h`, then
  ended in "media task already running" and no A2DP until reboot. Queued control
  work is let through, the thread is stopped, then the cleanup runs on the
  calling task, as in stock IDF.
- `0007-hand-just-works-and-peer-name-to-the-application.patch`: stock BTC answers
  a Just Works confirmation itself (`btc_dm_sp_cfm_req_evt`) and never tells the
  application, so with the host confirming pairings (protocol 1.8) a peer that
  claims no display would pair past it. The request now reaches the application
  flagged `just_works`, and the confirmation and the passkey notification both
  carry the peer name BTA has already read. The application must answer every
  confirmation, Just Works included.
- `0008-audio-worker-backpressure-telemetry.patch`: the sink worker decodes one
  packet and yields to queued control work, since a packet's PCM may now wait for
  DMA. The per-packet warnings for sequence gaps, a full codec queue and low-memory
  drops become calls to weak application hooks (`sat_audio_decode_begin`/`_end`,
  `sat_audio_encoded_event`), which `main/audio_out.c` counts and reports.
- `0009-sink-getconfig-fail-when-no-config-builds.patch`: 0001 dropped stock's
  SBC pre-fill of the preferred config and ignored a failed build, so when this
  device initiates (reconnect) and the chosen source SEP passes the support check
  but builds no config (SBC min_bitpool above 53), getconfig sent uninitialised
  stack bytes as SET_CONFIGURATION and decoder setup. The search now skips a SEP
  that does not build and tries the next; with none, getconfig fails and BTA
  takes its existing next-stream / GETCAP_FAIL path.
- `0010-strict-sink-set-configuration.patch`: 0001 validated an incoming
  SET_CONFIGURATION with `A2DP_IsPeerSourceCodecSupported`, the discovery check,
  which parses capability masks and only asks that they intersect the sink's. A
  config naming 44.1|48 kHz passed a 44.1-only offer and the application, testing
  48 first, took it for 48 kHz. The sink branch of `bta_av_co_audio_setconfig` now
  calls `A2DP_IsSinkCodecSupported`, which, as stock's `A2D_ParsSbcInfo(..., FALSE)`
  did, wants one value per field, each offered: SBC, AAC, aptX, aptX HD and LHDC V5
  (whose separate check from 0003 it replaces, through the vendor dispatcher in
  `bt_overlay`'s `a2dp_vendor.c`). Discovery keeps the liberal check.
- `0011-ignore-connection-exists-on-connected-link.patch`: a stock bug, not one of
  ours. When both sides page at once, `l2c_link_hci_conn_req` accepts the peer's
  page as a collision while our Create_Connection is still running; that page
  later ends in a Connection Complete with Connection Already Exists (0x0B) for
  a peer whose LCB is already `LST_CONNECTED`. Stock `l2c_link_hci_conn_comp`
  treated it as a failure of the live link and ran `l2c_link_hci_disc_comp` on
  the live handle: A2DP and AVRCP went down, the LCB was released without an
  HCI_Disconnect, the controller kept the ACL and every packet on it hit
  "rcvd ACL for unknown handle", and the phone could not come back until reboot
  (bench, 12.10.2026, after a double reset while the phone was reconnecting).
  The event is now logged and ignored, as AOSP has done since `acb6fe3b8c84`
  (system/bt, October 2019, Android 11). ESP-IDF master still had the stock
  code on 12.10.2026. `tools/run_l2c_collision_tests.py` replays it through the
  generated function; `--bt-root` with the pinned stock component shows the
  teardown.

Patch paths are relative to `components/bt`; setup uses
`git apply --directory=components/bt` and stops on a rejected hunk.

Run `tools/run_host_tests.ps1` after setup. It includes the production-source BTA
reopen replay via `tools/run_bta_reopen_tests.py`. Use its `--bt-root` option with
the pinned stock component to reproduce the regression before patch 0002.
Tests stub transport/codec actions; a passing replay is not a phone/bench result.
