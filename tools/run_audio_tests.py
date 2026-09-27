#!/usr/bin/env python3
"""Execute production satellite transfer, packet generation and gate functions."""
import argparse, os, subprocess
from pathlib import Path
from run_bta_reopen_tests import function
ROOT = Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc',default='C:/msys64/ucrt64/bin/gcc.exe')
    a=p.parse_args()
    source=(ROOT/'main/audio_out.c').read_text()
    names=['static void counter_max(', 'void sat_audio_decode_begin(', 'void sat_audio_decode_end(',
           'static bool refill_current(', 'static uint32_t refill_now(', 'static void *refill_receive(',
           'static void refill_return_item(', 'static audio_refill_result_t audio_wait_refill(',
           'static uint32_t transfer_now(', 'static bool transfer_current(', 'static bool tx_send(',
           'static size_t i2s_write_locked(', 'static void i2s_write_silence_locked(',
           'static bool pcm_send(', 'static void audio_ring_put(',
           'uint32_t audio_out_gate(', 'void audio_out_write(', 'bool audio_out_resume(',
           'void audio_out_set_streaming(']
    out=ROOT/'build/host/audio'; out.mkdir(parents=True,exist_ok=True)
    (out/'audio_handlers.inc').write_text('\n'.join(function(source,n) for n in names))
    worker=(ROOT/'components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_sink.c').read_text()
    (out/'audio_worker.inc').write_text(function(worker,'static void btc_a2dp_sink_data_ready(UNUSED_ATTR void *context)'))
    exe=out/'test_audio.exe'; env=dict(os.environ)
    env['PATH']=str(Path(a.cc).parent)+os.pathsep+env.get('PATH','')
    subprocess.run([a.cc,'-std=c17','-Wall','-Wextra','-Werror','-O1','-g',
                    '-I'+str(out),'-I'+str(ROOT/'main'),str(ROOT/'tests/host/test_audio.c'),
                    '-o',str(exe)],check=True,env=env)
    return subprocess.call([str(exe)],env=env)
if __name__=='__main__': raise SystemExit(main())
