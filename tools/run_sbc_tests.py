#!/usr/bin/env python3
"""Compile the generated SBC capabilities, SBC decoder glue and OI decoder on a host."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    'common/bt_defs.h': '''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#define TRUE 1
#define FALSE 0
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32;
typedef int8_t INT8; typedef int16_t INT16; typedef int32_t INT32;
typedef bool BOOLEAN;
typedef UINT8 BD_ADDR[6];
typedef int bt_status_t;
typedef struct { UINT16 event, len, offset, layer_specific; } BT_HDR;
#define UNUSED(x) ((void)(x))
#include "common/bt_trace.h"
''',
    'common/bt_target.h': '''#pragma once
#include "common/bt_defs.h"
#define A2D_INCLUDED TRUE
#define SBC_DEC_INCLUDED TRUE
''',
    'common/bt_trace.h': '''#pragma once
#define APPL_TRACE_ERROR(...) ((void)0)
#define APPL_TRACE_WARNING(...) ((void)0)
#define APPL_TRACE_DEBUG(...) ((void)0)
#define APPL_TRACE_EVENT(...) ((void)0)
#define APPL_TRACE_API(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_VERBOSE(...) ((void)0)
''',
    'stack/bt_types.h': '#pragma once\n#include "common/bt_defs.h"\n',
    'stack/sdp_api.h': '#pragma once\ntypedef struct { int unused; } tSDP_DISCOVERY_DB;\n',
    'a2d_int.h': '#pragma once\n#include "stack/a2d_api.h"\n',
    'esp_log.h': '#pragma once\n',
    'osi/future.h': '#pragma once\n',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bt-root', type=Path, default=ROOT / 'components/bt')
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    out = ROOT / 'build/host/sbc'
    out.mkdir(parents=True, exist_ok=True)
    for name, content in STUBS.items():
        p = out / 'stub' / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
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
