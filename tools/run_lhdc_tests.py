#!/usr/bin/env python3
"""Compile the real LHDC core, CIE adapter and RTP wrapper against host BT stubs."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    out = ROOT / 'build/host/lhdc'
    out.mkdir(parents=True, exist_ok=True)
    stubs = {
        'common/bt_target.h': '#pragma once\n#define TRUE 1\n#define A2D_INCLUDED TRUE\n#define LHDCV5_DEC_INCLUDED TRUE\n',
        'common/bt_defs.h': '#pragma once\n#define UNUSED(x) ((void)(x))\n',
        'common/bt_trace.h': '#include "stack/a2d_api.h"\n#define APPL_TRACE_WARNING(...) ((void)0)\n',
        'esp_log.h': '#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n',
        'a2d_int.h': '', 'stack/a2d_sbc.h': '',
        'stack/bt_types.h': '#include "stack/a2d_api.h"\n',
        'stack/a2d_api.h': '''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
typedef uint8_t UINT8;
typedef uint8_t tA2D_STATUS;
typedef uint8_t tA2D_CODEC_TYPE;
typedef struct { uint16_t event, len, offset, layer_specific; } BT_HDR;
#define A2D_SUCCESS 0
#define A2D_FAIL 1
#define A2D_INVALID_PARAMS 2
#define A2D_WRONG_CODEC 3
#define A2D_BAD_SAMP_FREQ 4
#define A2D_MEDIA_TYPE_AUDIO 0
#define A2D_MEDIA_CT_NON_A2DP 255
''',
    }
    for name, content in stubs.items():
        p = out / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
    ov = ROOT / 'bt_overlay/host/bluedroid/stack'
    dec = ROOT / 'components/lhdcv5_decoder/decoder'
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    exe = out / 'test_lhdc.exe'
    sources = [ov / 'a2dp' / name for name in
               ('a2dp_vendor_lhdcv5.c', 'a2dp_vendor_lhdcv5_decoder.c', 'a2dp_rtp.c')]
    sources += sorted(dec.glob('*.c'))
    sources += [ROOT / 'tests/host/test_lhdc.c']
    subprocess.run([args.cc, '-std=c17', '-O2', '-Wall', '-Wextra',
                    '-Werror=incompatible-pointer-types', '-Werror=implicit-function-declaration',
                    '-DLHDC_HOST_BUILD', '-I'+str(out), '-I'+str(ov / 'include'),
                    '-I'+str(ov / 'include/stack'), '-I'+str(ov / 'a2dp/include'),
                    '-I'+str(dec), *map(str, sources), '-lm', '-o', str(exe)],
                   env=env, check=True)
    return subprocess.call([str(exe)], env=env)

if __name__ == '__main__':
    raise SystemExit(main())
