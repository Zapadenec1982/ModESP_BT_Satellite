"""Exercise the production cover stream feeder and patched BTA continuation callbacks."""
from pathlib import Path
import argparse, os, subprocess
from run_bta_reopen_tests import function
ROOT=Path(__file__).resolve().parents[1]
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--cc',default='C:/msys64/ucrt64/bin/gcc.exe')
    args=ap.parse_args(); out=ROOT/'build/host/cover'; out.mkdir(parents=True,exist_ok=True)
    source=(ROOT/'components/bt/host/bluedroid/bta/av/bta_av_ca_act.c').read_text()
    declarations='''static TIMER_LIST_ENT cover_continue_timer[BTA_AV_NUM_RCB];
static BOOLEAN cover_continue_pending[BTA_AV_NUM_RCB];
static UINT16 cover_continue_handle[BTA_AV_NUM_RCB];
'''
    names=['cover_cancel_continue','cover_continue_cb','cover_schedule_continue']
    (out/'cover_pacing.inc').write_text(declarations+'\n'.join(function(source,'static void '+name+'(') for name in names))
    assert 'GOEPC_RequestSetSRM(p_rcb' not in function(source,'void bta_av_ca_api_get(')
    assert 'cover_schedule_continue(p_rcb)' in function(source,'void bta_av_ca_response(')
    for name in ['close_goepc_and_disconnect','bta_av_ca_api_open','bta_av_ca_api_close','bta_av_ca_api_get','bta_av_ca_response_final','bta_av_ca_goep_disconnect','bta_av_ca_force_disconnect']:
        assert 'cover_cancel_continue(p_rcb)' in function(source,('static void ' if name=='close_goepc_and_disconnect' else 'void ')+name+'(')
    env=dict(os.environ);env['PATH']=str(Path(args.cc).parent)+os.pathsep+env.get('PATH','')
    exe=out/'test_cover.exe'
    subprocess.run([args.cc,'-std=c17','-Wall','-Wextra','-Werror','-O1',
        '-I'+str(out),'-I'+str(ROOT/'main'),'-I'+str(ROOT/'tests/host'),
        '-I'+str(ROOT/'components/satlink/include'),str(ROOT/'tests/host/test_cover.c'),'-o',str(exe)],env=env,check=True)
    return subprocess.call([str(exe)],env=env)
if __name__=='__main__':raise SystemExit(main())
