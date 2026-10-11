#!/usr/bin/env python3
"""Compile the generated sink SET_CONFIGURATION check with the real codec code on a host.

bta_av_co_audio_setconfig comes out of bta_av_co.c, the codec dispatchers out of
a2dp_codec_config.c and a2dp_vendor.c, all in components/bt as tools/setup.sh
generates it; SBC, AAC, aptX, aptX HD and LHDC V5 are compiled whole. Pass
--bt-root to try another tree.
"""
import argparse
import os
from pathlib import Path
import subprocess
from a2dp_host_stubs import definition, write_stubs

ROOT = Path(__file__).resolve().parents[1]

def licence_of(text):
    """The leading comment, to keep the upstream licence on extracted source."""
    return text[:text.index('*/') + 2] + '\n'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bt-root', type=Path, default=ROOT / 'components/bt')
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    out = ROOT / 'build/host/sink_setconfig'
    out.mkdir(parents=True, exist_ok=True)
    # a2dp_vendor_aptx.c reaches a2d_api.h before bt_target.h, so the switches
    # live in bt_defs.h, where every header sees them.
    write_stubs(out / 'stub', defs={
        'A2D_INCLUDED': 'TRUE', 'SBC_DEC_INCLUDED': 'TRUE', 'APTX_DEC_INCLUDED': 'TRUE',
        'AAC_DEC_INCLUDED': 'TRUE', 'LHDCV5_DEC_INCLUDED': 'TRUE',
        'AVDT_CODEC_TYPE_INDEX': '2', 'AVDT_CODEC_SIZE': '30', 'AVDT_PROTECT_SIZE': '90'})
    bd = args.bt_root / 'host/bluedroid'
    a2dp = bd / 'btc/profile/std/a2dp'
    stack = bd / 'stack/a2dp'
    co = (a2dp / 'bta_av_co.c').read_text(encoding='utf-8')
    hdr = (a2dp / 'include/btc_av_co.h').read_text(encoding='utf-8')
    cfg = (stack / 'a2dp_codec_config.c').read_text(encoding='utf-8')
    ven = (stack / 'a2dp_vendor.c').read_text(encoding='utf-8')
    first = hdr.index('typedef struct {', hdr.index('BTC_SV_AV_AA_SOURCE_COUNT'))
    types = hdr[first:hdr.index('} tBTA_AV_CO_PEER;') + len('} tBTA_AV_CO_PEER;')]
    (out / 'co_types.inc').write_text(licence_of(co) + types + '\n', encoding='utf-8')
    (out / 'co_setconfig.inc').write_text(
        licence_of(co) + definition(co, 'void bta_av_co_audio_setconfig(') + '\n',
        encoding='utf-8')
    funcs = [definition(cfg, sig) for sig in (
        'tA2D_CODEC_TYPE A2DP_GetCodecType(', 'void A2DP_SinkAllow48kHz(',
        'tA2D_STATUS A2DP_IsSinkCodecSupported(', 'tA2D_STATUS A2DP_IsPeerSourceCodecSupported(')]
    vfuncs = [definition(ven, sig) for sig in (
        'uint32_t A2DP_VendorCodecGetVendorId(', 'uint16_t A2DP_VendorCodecGetCodecId(',
        'tA2D_STATUS A2DP_IsVendorSinkCodecSupported(',
        'tA2D_STATUS A2DP_IsVendorPeerSourceCodecSupported(')]
    (out / 'codec_dispatch.inc').write_text(
        licence_of(cfg) + '\n\n'.join(funcs) + '\n' + licence_of(ven) + '\n\n'.join(vfuncs) + '\n',
        encoding='utf-8')

    oi = bd / 'external/sbc/decoder'
    sources = [stack / name for name in
               ('a2d_sbc.c', 'a2d_sbc_decoder.c', 'a2dp_rtp.c', 'a2dp_aac.c',
                'a2dp_vendor_aptx.c', 'a2dp_vendor_aptx_hd.c', 'a2dp_vendor_lhdcv5.c')]
    sources += sorted((oi / 'srce').glob('*.c'))
    sources += [ROOT / 'tests/host/test_sink_setconfig.c']
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    exe = out / 'test_sink_setconfig.exe'
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
