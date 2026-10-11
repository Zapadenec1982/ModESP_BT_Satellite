#!/usr/bin/env python3
"""Compile the generated sink-initiator getconfig with the real SBC, aptX and aptX HD code on a host.

The peer table types come from btc_av_co.h and the three functions from bta_av_co.c,
both in components/bt as tools/setup.sh generates it. Pass --bt-root to try another tree.
"""
import argparse
import os
from pathlib import Path
import re
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
#define A2D_INCLUDED TRUE
#define SBC_DEC_INCLUDED TRUE
#define APTX_DEC_INCLUDED TRUE
#define AVDT_CODEC_SIZE 30
#define AVDT_PROTECT_SIZE 90
#include "common/bt_trace.h"
''',
    # a2dp_vendor_aptx.c reaches a2d_api.h before bt_target.h, so the switches
    # live here, where every header sees them.
    'common/bt_target.h': '#pragma once\n#include "common/bt_defs.h"\n',
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

def definition(text, signature):
    """The body of the function whose definition (not prototype) starts with signature."""
    for m in re.finditer(re.escape(signature), text):
        close = text.index(")", m.end())
        rest = text[close + 1:].lstrip()
        if not rest.startswith("{"):
            continue
        brace = text.index("{", close)
        depth, end = 1, brace + 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        return text[m.start():end]
    raise ValueError("no definition of " + signature)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bt-root', type=Path, default=ROOT / 'components/bt')
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    out = ROOT / 'build/host/sink_getconfig'
    out.mkdir(parents=True, exist_ok=True)
    for name, content in STUBS.items():
        p = out / 'stub' / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
    bd = args.bt_root / 'host/bluedroid'
    a2dp = bd / 'btc/profile/std/a2dp'
    co = (a2dp / 'bta_av_co.c').read_text(encoding='utf-8')
    hdr = (a2dp / 'include/btc_av_co.h').read_text(encoding='utf-8')
    # Preserve the upstream licence on extracted source.
    licence = co[:co.index('#include')]
    first = hdr.index('typedef struct {', hdr.index('BTC_SV_AV_AA_SOURCE_COUNT'))
    types = hdr[first:hdr.index('} tBTA_AV_CO_PEER;') + len('} tBTA_AV_CO_PEER;')]
    funcs = [definition(co, 'bta_av_build_src_cfg ('),
             definition(co, 'static tBTA_AV_CO_SINK* bta_av_co_audio_find_peer_src('),
             definition(co, 'UINT8 bta_av_audio_sink_getconfig(')]
    (out / 'co_types.inc').write_text(licence + types + '\n', encoding='utf-8')
    # bta_av_build_src_cfg is looked up without its return type, which 0009 changed
    # from void, so the replay also builds on a tree without it; put the type back.
    build_line = co[:co.index(funcs[0])].rsplit('\n', 1)[1]
    funcs[0] = build_line + funcs[0]
    (out / 'co_getconfig.inc').write_text(licence + '\n\n'.join(funcs) + '\n', encoding='utf-8')

    oi = bd / 'external/sbc/decoder'
    sources = [bd / 'stack/a2dp' / name for name in
               ('a2d_sbc.c', 'a2d_sbc_decoder.c', 'a2dp_rtp.c', 'a2dp_vendor_aptx.c',
                'a2dp_vendor_aptx_hd.c')]
    sources += sorted((oi / 'srce').glob('*.c'))
    sources += [ROOT / 'tests/host/test_sink_getconfig.c']
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    exe = out / 'test_sink_getconfig.exe'
    # The stubs come first so they shadow the target's own config and type headers.
    subprocess.run([args.cc, '-std=c17', '-O1', '-g',
                    '-Werror=implicit-function-declaration', '-Werror=incompatible-pointer-types',
                    '-I'+str(out), '-I'+str(out / 'stub'), '-I'+str(bd / 'stack/include'),
                    '-I'+str(bd / 'stack/include/stack'), '-I'+str(bd / 'stack/a2dp/include'),
                    '-I'+str(oi / 'include'), *map(str, sources), '-o', str(exe)],
                   env=env, check=True)
    return subprocess.call([str(exe)], env=env)

if __name__ == '__main__':
    raise SystemExit(main())
