#!/usr/bin/env python3
"""Compile the real aptX LL codec, the aptX decoder and libfreeaptx against host BT stubs."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

A2D_API = '''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#define TRUE 1
#define FALSE 0
typedef uint8_t UINT8;
typedef uint8_t tA2D_STATUS;
typedef uint8_t tA2D_CODEC_TYPE;
typedef struct { uint16_t event, len, offset, layer_specific; } BT_HDR;
#define A2D_SUCCESS 0
#define A2D_FAIL 0x0A
#define A2D_INVALID_PARAMS 0x0C
#define A2D_WRONG_CODEC 0x0D
#define A2D_BAD_SAMP_FREQ 0xC2
#define A2D_NS_CODEC_TYPE 0xC3
#define A2D_NS_SAMP_FREQ 0xC4
#define A2D_BAD_CH_MODE 0xC5
#define A2D_NS_CH_MODE 0xC6
#define A2D_MEDIA_TYPE_AUDIO 0
#define A2D_MEDIA_CT_NON_A2DP 0xFF
#define A2D_SET_ONE_BIT 1
#define A2D_SET_MULTL_BIT 2
#define A2D_SET_ZERO_BIT 0
static inline UINT8 A2D_BitsSet(UINT8 num)
{
    return num == 0 ? A2D_SET_ZERO_BIT : (num & (num - 1)) ? A2D_SET_MULTL_BIT : A2D_SET_ONE_BIT;
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    out = ROOT / 'build/host/aptx_ll'
    out.mkdir(parents=True, exist_ok=True)
    stubs = {
        'common/bt_target.h': '#pragma once\n#define TRUE 1\n#define A2D_INCLUDED TRUE\n'
                              '#define APTX_DEC_INCLUDED TRUE\n',
        'common/bt_defs.h': '#pragma once\n#define UNUSED(x) ((void)(x))\n',
        'common/bt_trace.h': '#pragma once\n#include "stack/a2d_api.h"\n'
                             '#define APPL_TRACE_ERROR(...) ((void)0)\n'
                             '#define APPL_TRACE_WARNING(...) ((void)0)\n'
                             '#define LOG_ERROR(...) ((void)0)\n#define LOG_INFO(...) ((void)0)\n'
                             '#define LOG_VERBOSE(...) ((void)0)\n',
        'freertos/FreeRTOS.h': '#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\n'
                               '#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))\n',
        'freertos/task.h': '#pragma once\nstatic inline TickType_t xTaskGetTickCount(void) { return 0; }\n',
        'a2d_int.h': '#pragma once\n#include "common/bt_trace.h"\n',
        'stack/a2d_sbc.h': '',
        'stack/bt_types.h': '#include "stack/a2d_api.h"\n',
        'stack/a2d_api.h': A2D_API,
        'a2d_api.h': '#include "stack/a2d_api.h"\n',
        'avdt_api.h': '#pragma once\n#define AVDT_MEDIA_AUDIO 0\n',
    }
    for name, content in stubs.items():
        p = out / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
    ov = ROOT / 'bt_overlay/host/bluedroid/stack'
    aptx = ROOT / 'bt_overlay/host/bluedroid/external/libfreeaptx/main'
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    exe = out / 'test_aptx_ll.exe'
    sources = [ov / 'a2dp' / name for name in
               ('a2dp_vendor_aptx_ll.c', 'a2dp_vendor_aptx.c', 'a2dp_vendor_aptx_hd.c',
                'a2dp_vendor_aptx_decoder.c', 'a2dp_rtp.c')]
    sources += [aptx / 'freeaptx.c', ROOT / 'tests/host/test_aptx_ll.c']
    subprocess.run([args.cc, '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                    '-Werror=incompatible-pointer-types', '-Werror=implicit-function-declaration',
                    '-I'+str(out), '-I'+str(ov / 'include'), '-I'+str(ov / 'include/stack'),
                    '-I'+str(ov / 'a2dp/include'), '-I'+str(aptx),
                    *map(str, sources), '-lm', '-o', str(exe)],
                   env=env, check=True)
    return subprocess.call([str(exe)], env=env)

if __name__ == '__main__':
    raise SystemExit(main())
