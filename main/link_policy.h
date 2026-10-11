/*
 * Rules of the link itself (PROTOCOL.md 3 and 13), as pure C: main/link.c applies
 * them, tests/host/test_rules.c walks them.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "satlink/proto.h"

/*
 * A host with another proto_major: the link does not come up. Both sides keep
 * HELLO, PING and PONG going and the device keeps sending LOG; nothing else goes
 * out, and nothing else the host sends is acted on.
 */
static inline bool link_incompatible_allows(uint8_t type)
{
    return type == SATLINK_MSG_HELLO || type == SATLINK_MSG_PING ||
           type == SATLINK_MSG_PONG || type == SATLINK_MSG_LOG;
}
