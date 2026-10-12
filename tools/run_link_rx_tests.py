#!/usr/bin/env python3
"""Replay UART events that announce fewer bytes than are buffered, using production link RX code."""
import argparse
import os
from pathlib import Path
import subprocess
from run_bta_reopen_tests import function

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'main/link.c')
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    out = ROOT / 'build/host/link_rx'
    out.mkdir(parents=True, exist_ok=True)
    signatures = ['static void rx_feed_chunk(', 'static void rx_drain_residue(',
                  'static void rx_data_event(']
    (out / 'link_rx.inc').write_text(
        '\n'.join(function(source, signature) for signature in signatures), encoding='utf-8')
    exe = out / 'test_link_rx.exe'
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    sat = ROOT / 'components/satlink'
    subprocess.run([args.cc, '-std=c17', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-I'+str(out), '-I'+str(ROOT / 'tests/host'), '-I'+str(sat / 'include'),
                    str(ROOT / 'tests/host/test_link_rx.c'), str(sat / 'frame.c'),
                    str(sat / 'cobs.c'), str(sat / 'crc16.c'), '-o', str(exe)],
                   env=env, check=True)
    return subprocess.call([str(exe)], env=env)

if __name__ == '__main__':
    raise SystemExit(main())
