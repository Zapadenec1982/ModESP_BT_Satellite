# Bluetooth stack patches

Apply in filename order to the exact ESP-IDF component pinned in `idf.pin`,
then copy `bt_overlay/`. `tools/setup.ps1 -Clean` reproduces the local ignored
`components/bt`; never patch the shared IDF or rely on edits to generated files.

- `0001-a2dp-multi-codec-framework.patch`: codec framework, SBC/AAC/aptX/aptX HD
  integration and project adaptations. See `docs/BASE.md` for provenance/exclusions.
- `0002-notify-btc-on-queued-stream-reopen.patch`: complete the existing incoming
  configuration notification when CONFIG is dispatched from INIT after a queued
  CLOSE. This prevents BTC remaining IDLE during a peer codec switch.
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

Patch paths are relative to `components/bt`; setup uses
`git apply --directory=components/bt` and stops on a rejected hunk.

Run `tools/run_host_tests.ps1` after setup. It includes the production-source BTA
reopen replay via `tools/run_bta_reopen_tests.py`. Use its `--bt-root` option with
the pinned stock component to reproduce the regression before patch 0002.
Tests stub transport/codec actions; a passing replay is not a phone/bench result.
