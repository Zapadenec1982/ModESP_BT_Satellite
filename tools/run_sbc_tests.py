#!/usr/bin/env python3
"""Compile the generated SBC capabilities, SBC decoder glue and OI decoder on a host."""
import argparse
import os
from pathlib import Path
import subprocess
from a2dp_host_stubs import write_stubs

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bt-root', type=Path, default=ROOT / 'components/bt')
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    out = ROOT / 'build/host/sbc'
    out.mkdir(parents=True, exist_ok=True)
    write_stubs(out / 'stub', target={'A2D_INCLUDED': 'TRUE', 'SBC_DEC_INCLUDED': 'TRUE'})
    bd = args.bt_root / 'host/bluedroid'
    oi = bd / 'external/sbc/decoder'
    sources = [bd / 'stack/a2dp' / name for name in ('a2d_sbc.c', 'a2d_sbc_decoder.c', 'a2dp_rtp.c')]
    sources += sorted((oi / 'srce').glob('*.c'))
    sources += [ROOT / 'tests/host/test_sbc.c']
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    exe = out / 'test_sbc.exe'
    # The stubs come first so they shadow the target's own config and type headers.
    subprocess.run([args.cc, '-std=c17', '-O1', '-g',
                    '-Werror=implicit-function-declaration', '-Werror=incompatible-pointer-types',
                    '-I'+str(out / 'stub'), '-I'+str(bd / 'stack/include'),
                    '-I'+str(bd / 'stack/include/stack'), '-I'+str(bd / 'stack/a2dp/include'),
                    '-I'+str(oi / 'include'), *map(str, sources), '-o', str(exe)],
                   env=env, check=True)
    return subprocess.call([str(exe)], env=env)

if __name__ == '__main__':
    raise SystemExit(main())
