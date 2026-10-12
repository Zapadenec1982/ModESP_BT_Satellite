"""Shared pieces of the host replays that compile generated A2DP codec code.

run_sbc_tests.py, run_sink_getconfig_tests.py and run_sink_setconfig_tests.py all
shadow the target's config, type and trace headers with the same small stubs; they
differ only in the switches each one defines. The two sink runners also cut single
function definitions out of generated source.
"""
import re

_BT_DEFS = '''#pragma once
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
{defines}#include "common/bt_trace.h"
'''

_BT_TRACE = '''#pragma once
#define APPL_TRACE_ERROR(...) ((void)0)
#define APPL_TRACE_WARNING(...) ((void)0)
#define APPL_TRACE_DEBUG(...) ((void)0)
#define APPL_TRACE_EVENT(...) ((void)0)
#define APPL_TRACE_API(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_VERBOSE(...) ((void)0)
'''

def _defines(macros):
    return ''.join('#define %s %s\n' % (name, value) for name, value in macros.items())

def stubs(defs=None, target=None):
    """The stub headers by path, with the macros of defs in bt_defs.h and of target in bt_target.h."""
    return {
        'common/bt_defs.h': _BT_DEFS.replace('{defines}', _defines(defs or {})),
        'common/bt_target.h': '#pragma once\n#include "common/bt_defs.h"\n' + _defines(target or {}),
        'common/bt_trace.h': _BT_TRACE,
        'stack/bt_types.h': '#pragma once\n#include "common/bt_defs.h"\n',
        'stack/sdp_api.h': '#pragma once\ntypedef struct { int unused; } tSDP_DISCOVERY_DB;\n',
        'a2d_int.h': '#pragma once\n#include "stack/a2d_api.h"\n',
        'esp_log.h': '#pragma once\n',
        'osi/future.h': '#pragma once\n',
    }

def write_stubs(stub_dir, defs=None, target=None):
    """Write the stub headers under stub_dir; see stubs()."""
    for name, content in stubs(defs, target).items():
        p = stub_dir / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)

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
