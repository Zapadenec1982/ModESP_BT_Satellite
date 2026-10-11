#!/usr/bin/env python3
"""Compile the vendored Helix AAC decoder and feed it malformed raw blocks."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    out = ROOT / 'build/host/aac'
    out.mkdir(parents=True, exist_ok=True)
    helix = ROOT / 'bt_overlay/host/bluedroid/external/libaac-lc/libhelix_aac'
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    # The decoder's overruns are what this is about; use ASan where the compiler has it.
    probe = out / 'asan_probe.c'
    probe.write_text('int main(void){return 0;}\n')
    asan = subprocess.call([args.cc, '-fsanitize=address', str(probe), '-o', str(out / 'asan_probe.exe')],
                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
    if not asan:
        print('run_aac_tests: no ASan here, relying on the guard band.')
    exe = out / 'test_aac.exe'
    # The same files as the component's CMakeLists, in patches/0001.
    names = ('aacdec', 'aactabs', 'bitstream', 'buffers', 'dct4', 'decelmnt', 'dequant', 'fft',
             'filefmt', 'huffman', 'hufftabs', 'imdct', 'noiseless', 'pns', 'stproc', 'tns', 'trigtabs')
    sources = [helix / (name + '.c') for name in names] + [ROOT / 'tests/host/test_aac.c']
    subprocess.run([args.cc, '-std=c17', '-O1', '-g', *(['-fsanitize=address'] if asan else []),
                    '-Werror=implicit-function-declaration', '-I'+str(helix),
                    *map(str, sources), '-o', str(exe)],
                   env=env, check=True)
    # A decoder that spins on a truncated frame must fail here, not hang the suite.
    try:
        return subprocess.call([str(exe)], env=env, timeout=60)
    except subprocess.TimeoutExpired:
        print('run_aac_tests: test_aac did not finish in 60 s')
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
