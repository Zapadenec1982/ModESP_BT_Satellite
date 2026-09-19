#!/usr/bin/env python3
"""Compile the real BTA state tables/dispatcher and incoming notification on a host.

Only hardware actions and codec acceptance are stubbed. Generated C stays in build/.
Run after setup.ps1, or pass --bt-root to test the pinned upstream component.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bt-root", type=Path, default=ROOT / "components/bt")
    parser.add_argument("--cc", default="C:/msys64/ucrt64/bin/gcc.exe")
    args = parser.parse_args()
    av = args.bt_root / "host/bluedroid/bta/av"
    ssm = (av / "bta_av_ssm.c").read_text(encoding="utf-8")
    actions = (av / "bta_av_aact.c").read_text(encoding="utf-8")
    header = (av / "include/bta_av_int.h").read_text(encoding="utf-8")
    btc = (args.bt_root / "host/bluedroid/btc/profile/std/a2dp/btc_av.c").read_text(encoding="utf-8")
    # Preserve the upstream licence on extracted source, including generated files.
    licence = ssm[:ssm.index("#include")]
    events = header[header.index("enum"):header.index("};", header.index("enum"))+2]
    tables = ssm[ssm.index("/* state machine states */"):ssm.index("static char *bta_av_sst_code")]
    dispatch = function(ssm, "void bta_av_ssm_execute(")
    notify = function(actions, "if (p_scb->force_incoming)")
    idle = function(btc, "static BOOLEAN btc_av_state_idle_handler(btc_sm_event_t event, void *p_data)")
    incoming = function(idle, "case BTA_AV_INCOMING_CFG_EVT:")
    # Extract the production notification case, wrapped in a one-event dispatcher.
    incoming = "switch (event) { " + incoming + " default: break; }"
    out = ROOT / "build/host/bta_reopen"
    out.mkdir(parents=True, exist_ok=True)
    (out / "bta_tables.inc").write_text(licence+events+"\n"+tables, encoding="utf-8")
    (out / "bta_dispatch.inc").write_text(licence+dispatch, encoding="utf-8")
    (out / "bta_notify.inc").write_text(licence+notify, encoding="utf-8")
    (out / "btc_incoming.inc").write_text(licence+incoming, encoding="utf-8")
    exe = out / "test_bta_reopen.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(args.cc).parent)+os.pathsep+env.get("PATH", "")
    subprocess.run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", "-O1",
                    "-I"+str(out), "-I"+str(ROOT / "tests/host"),
                    str(ROOT / "tests/host/test_bta_reopen.c"), "-o", str(exe)],
                   env=env, check=True)
    return subprocess.call([str(exe)], env=env)

if __name__ == "__main__":
    raise SystemExit(main())
