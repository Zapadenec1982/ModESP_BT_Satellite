# Bluetooth stack patches

Apply in filename order to the exact ESP-IDF component pinned in `idf.pin`,
then copy `bt_overlay/`. `tools/setup.ps1 -Clean` reproduces the local ignored
`components/bt`; never patch the shared IDF or rely on edits to generated files.

- `0001-a2dp-multi-codec-framework.patch`: codec framework, SBC/AAC/aptX/aptX HD
  integration and project adaptations. See `docs/BASE.md` for provenance/exclusions.
- `0002-notify-btc-on-queued-stream-reopen.patch`: complete the existing incoming
  configuration notification when CONFIG is dispatched from INIT after a queued
  CLOSE. This prevents BTC remaining IDLE during a peer codec switch.

Patch paths are relative to `components/bt`; setup uses
`git apply --directory=components/bt` and stops on a rejected hunk.

Run `tools/run_host_tests.ps1` after setup. It includes the production-source BTA
reopen replay via `tools/run_bta_reopen_tests.py`. Use its `--bt-root` option with
the pinned stock component to reproduce the regression before patch 0002.
Tests stub transport/codec actions; a passing replay is not a phone/bench result.
