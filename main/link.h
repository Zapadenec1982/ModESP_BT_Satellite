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

void link_send_cover_reset(uint32_t nonce, uint32_t generation);
bool link_try_cover_stream(uint32_t nonce, uint32_t generation, uint32_t offset,
                           const uint8_t *data, size_t len, uint32_t crc);
void link_send_cover_data(uint32_t nonce, uint32_t generation, uint32_t total,
                          uint32_t offset, const uint8_t *data, uint16_t len);

/* One chunk of cover art. The caller does the slicing; this only frames it. */
void link_send_cover(uint16_t chunk, uint16_t total, uint8_t mime,
                     uint32_t total_len, const uint8_t *data, uint16_t len);

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
    uint32_t pings_unanswered;
    uint32_t rtt_ms;               /* last round trip, from our PING to its PONG */
} link_stats_t;

void link_get_stats(link_stats_t *out);

/*
 * How many droppable messages are still waiting to go out.
 *
 * Exists for the cover art sender, which is the only producer that can generate
 * more messages in one burst than the queue holds. Everything else emits one
 * message per event and cannot outrun the wire.
 */
uint32_t link_bulk_pending(void);
#define LINK_BULK_COVER_HEADROOM 4
