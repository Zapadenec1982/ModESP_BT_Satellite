#!/usr/bin/env python3
"""Compile the generated l2c_link_hci_conn_comp on a host and replay a paging collision.

The function comes from l2c_link.c, the link states from l2c_int.h and the HCI
codes from hcidefs.h, all in components/bt as tools/setup.sh generates it. Pass
--bt-root with the pinned stock component to see the live link torn down.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
from a2dp_host_stubs import definition

ROOT = Path(__file__).resolve().parents[1]
HCI_CODES = ('HCI_SUCCESS', 'HCI_ERR_PAGE_TIMEOUT', 'HCI_ERR_MAX_NUM_OF_CONNECTIONS',
             'HCI_ERR_CONNECTION_EXISTS', 'HCI_INVALID_HANDLE')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bt-root', type=Path, default=ROOT / 'components/bt')
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    stack = args.bt_root / 'host/bluedroid/stack'
    link = (stack / 'l2cap/l2c_link.c').read_text(encoding='utf-8')
    l2c_int = (stack / 'l2cap/include/l2c_int.h').read_text(encoding='utf-8')
    hcidefs = (stack / 'include/stack/hcidefs.h').read_text(encoding='utf-8')
    # Preserve the upstream licence on extracted source.
    licence = link[:link.index('#include')]
    end = l2c_int.index('} tL2C_LINK_STATE;') + len('} tL2C_LINK_STATE;')
    states = l2c_int[l2c_int.rindex('typedef enum {', 0, end):end]
    codes = []
    for name in HCI_CODES:
        m = re.search(r'^#define\s+%s\s+(\S+)' % name, hcidefs, re.M)
        if not m:
            raise SystemExit('no %s in hcidefs.h' % name)
        codes.append('#define %s %s' % (name, m.group(1)))
    out = ROOT / 'build/host/l2c_collision'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'l2c_defs.inc').write_text(licence + states + '\n' + '\n'.join(codes) + '\n',
                                      encoding='utf-8')
    # static: the replay is one translation unit and calls it directly.
    func = definition(link, 'BOOLEAN l2c_link_hci_conn_comp (')
    (out / 'l2c_conn_comp.inc').write_text(licence + 'static ' + func + '\n', encoding='utf-8')
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    exe = out / 'test_l2c_collision.exe'
    subprocess.run([args.cc, '-std=c17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-I' + str(out), '-I' + str(ROOT / 'tests/host'),
                    str(ROOT / 'tests/host/test_l2c_collision.c'), '-o', str(exe)],
                   env=env, check=True)
    return subprocess.call([str(exe)], env=env)

if __name__ == '__main__':
    raise SystemExit(main())
