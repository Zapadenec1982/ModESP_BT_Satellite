#!/usr/bin/env python3
"""Replay startup greeting loss using production link handlers and wire codecs."""
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
    out = ROOT / 'build/host/hello'
    out.mkdir(parents=True, exist_ok=True)
    signatures = ['static size_t encode_own_hello(', 'static void send_hello(void)',
                  'static void answer_hello(bool force)',
                  'static void handle_hello(', 'static void ping_tick(void *arg)']
    (out / 'hello_handlers.inc').write_text(
        '\n'.join(function(source, signature) for signature in signatures), encoding='utf-8')
    exe = out / 'test_hello.exe'
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    subprocess.run([args.cc, '-std=c17', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-I'+str(out), '-I'+str(ROOT / 'tests/host'),
                    '-I'+str(ROOT / 'components/satlink/include'),
                    str(ROOT / 'tests/host/test_hello.c'),
                    str(ROOT / 'components/satlink/messages.c'),
                    str(ROOT / 'components/satlink/codec.c'), '-o', str(exe)],
                   env=env, check=True)
    return subprocess.call([str(exe)], env=env)

if __name__ == '__main__':
    raise SystemExit(main())
