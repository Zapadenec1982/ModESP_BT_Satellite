#!/usr/bin/env python3
"""Compile and run the production one-tick refill helper on the host."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="C:/msys64/ucrt64/bin/gcc.exe")
    args = parser.parse_args()
    output = ROOT / "build/host/audio_refill"
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ("test_audio_refill.exe" if os.name == "nt" else "test_audio_refill")
    env = dict(os.environ)
    env["PATH"] = str(Path(args.cc).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run(
        [args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", "-O2", "-g",
         "-I" + str(ROOT / "main"), str(ROOT / "tests/host/test_audio_refill.c"),
         "-o", str(executable)], check=True, env=env, timeout=60)
    return subprocess.call([str(executable)], env=env, timeout=30)


if __name__ == "__main__":
    raise SystemExit(main())
