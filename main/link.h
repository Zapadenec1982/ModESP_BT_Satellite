/*
 * The control link to the host: UART2, 460800 8N1, framed per docs/PROTOCOL.md.
 *
 * Nothing here ever blocks a caller. A host that has stopped reading is not an
 * emergency — the device keeps playing, drops what the spec says is droppable,
 * and holds on to what it says is not.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "satlink/messages.h"
#include "satlink/format_sync.h"

/* Everything the host can ask of us. Every one of these is called from the link's
 * own task, never from an interrupt. */
typedef struct {
    void (*on_hello)(const satlink_hello_t *hello);
    void (*on_format_ready)(const fmt_ready_t *ready);
    void (*on_transport)(uint8_t cmd);
    void (*on_volume)(uint8_t volume);
    void (*on_radio)(bool on, bool discoverable, bool disconnect);
    void (*on_gpio_cfg)(const satlink_gpio_cfg_t *cfg);
    void (*on_gpio_out)(uint32_t values, uint32_t mask);
    void (*on_adc_read)(uint8_t index);
    /* Protocol 1.7. The host's transfer of this pair produced no usable image and
     * it wants the picture sent again. A hint, not a request: nothing answers it,
     * nothing waits for it, and ignoring it leaves the host exactly where 1.6
     * left it -- with an empty tile. */
    void (*on_cover_retry)(uint32_t boot_nonce, uint32_t track_generation);
    /* Protocol 1.8. The name in `set` points into the frame and is gone when the
     * handler returns: copy it. */
    void (*on_pair_reply)(const uint8_t addr[6], bool accept);
    void (*on_bt_config_set)(const satlink_bt_config_set_t *set);
    void (*on_bond_remove)(const uint8_t addr[6]);
    void (*on_bt_refresh)(void);
} link_handlers_t;

/* `caps` is what this build can actually do, not what the protocol defines. The
 * host believes it: declaring A2DP in a build without A2DP would have it waiting
 * for a stream that is never coming. */
esp_err_t link_start(const link_handlers_t *handlers, uint32_t caps);

/* True once a HELLO with a matching major version has arrived. Until then the
 * host is assumed absent, which is a normal state and not a fault. */
bool link_host_present(void);

/* Host capability bits from its HELLO, zero when it has not spoken yet. The one
 * that matters is SATLINK_HOST_CAP_RELOCK: without it we promise 44.1 kHz and
 * never move. */
uint32_t link_host_caps(void);

/* The host's proto_minor from its HELLO, zero when it has not spoken. Messages a
 * minor introduced go only to a host at that minor or above. */
uint8_t link_host_minor(void);

/*
 * Blocks until the host has greeted us, or the timeout runs out.
 *
 * Wait before registering codec endpoints so supported rates reflect the host.
 * Slot width is selected later by the serialized format transaction, per codec
 * for an acknowledged host and by the legacy capability policy otherwise.
 *
 * Returns true if a greeting arrived. False is not a failure: a device with no
 * host still has to come up, and it comes up conservative.
 */
bool link_wait_for_host(uint32_t timeout_ms);

/* Device to host. All non-blocking; all safe to call before a host exists. */
void link_send_bt_state(uint8_t state, uint8_t codec, const uint8_t addr[6], const char *name);
void link_send_track_meta(const char *title, const char *artist, const char *album);
void link_send_track_pos(uint8_t state, uint32_t elapsed_ms, uint32_t total_ms);
void link_send_stream_fmt(uint32_t rate_hz, uint8_t bits, uint8_t channels);
/* Tagged completion is recorded only after UART TX is physically drained. */
void link_send_format(const fmt_request_t *request, bool acknowledged);
bool link_format_sent(const fmt_request_t *request, uint32_t *when_ms);
void link_send_volume_state(uint8_t volume, uint8_t source);
void link_send_gpio_in(uint32_t values, uint32_t changed);
void link_send_gpio_map(const satlink_gpio_map_t *map);
void link_send_adc_value(uint8_t index, uint16_t raw, uint16_t mv);
void link_send_log(uint8_t level, const char *text);

/* Protocol 1.8. Silent for a host below minor 8, which would not know the types.
 * None of them is droppable: each answers something the host is waiting on. */
void link_send_pair_request(const satlink_pair_request_t *m);
void link_send_pair_result(const uint8_t addr[6], uint8_t status);
void link_send_bt_config(const satlink_bt_config_t *m);
void link_send_bond_list(const satlink_bond_list_t *m);

/* Cover art, protocol 1.6 only: RESET on the critical queue, then one DATA per
 * OBEX chunk and an END, all copied into bounded heap frames on the caller's
 * task. Both are silent for a host that did not announce COVER_STREAM_CAP; the
 * 1.5 collect-then-send path and the 1.0 COVER message are no longer sent, see
 * docs/PROTOCOL.md. */
void link_send_cover_reset(uint32_t nonce, uint32_t generation);
bool link_try_cover_stream(uint32_t nonce, uint32_t generation, uint32_t offset,
                           const uint8_t *data, size_t len, uint32_t crc);

/* Counters, for the report that explains a bad cable or a wedged host. */
typedef struct {
    uint32_t tx_frames;
    uint32_t tx_dropped_bulk;      /* TRACK_POS, LOG, COVER: the spec allows this */
    uint32_t tx_dropped_critical;  /* anything else: the spec does not, so it is a fault */
    uint32_t rx_bytes;             /* raw, before framing: tells a dead wire from a noisy one */
    uint32_t rx_frames;
    uint32_t rx_bad;
    uint32_t rx_seq_gaps;
    /* Frames of a type this build does not implement. A peer that is newer within
     * the same major may send some, which is legal; a count that climbs while
     * nothing else arrives means the two sides disagree about what they speak,
     * and that used to be indistinguishable from silence. */
    uint32_t rx_unknown;
    /* Times the receiver was not run often enough and the UART driver threw bytes
     * away. Distinct from rx_bad, which counts frames that arrived and did not
     * decode: an overrun is our fault, a bad frame is the wire's. Keeping them in
     * one number hid the first behind the second. */
    uint32_t rx_overruns;
    /* How many greetings we have actually put on the wire. "We never answered"
     * and "we answered and it was lost" are different faults with the same
     * symptom, and this is what tells them apart. */
    uint32_t hello_sent;
    /* Our own HELLOs heard back, which a floating receive line does: counted
     * and ignored, never taken for the host's. */
    uint32_t rx_echoes;
    uint32_t pings_unanswered;
    uint32_t rtt_ms;               /* last round trip, from our PING to its PONG */
} link_stats_t;

void link_get_stats(link_stats_t *out);

/*
 * How many droppable messages are still waiting to go out.
 *
 * Exists for the cover stream, which is the only producer that can generate
 * more messages in one burst than the queue holds. Everything else emits one
 * message per event and cannot outrun the wire.
 */
uint32_t link_bulk_pending(void);
#define LINK_BULK_COVER_HEADROOM 4
