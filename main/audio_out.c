#include "audio_out.h"
#include "audio_transfer.h"
#include "audio_telemetry.h"
#include "audio_refill.h"
#include "audio_asrc.h"

#include <inttypes.h>
#include <string.h>
#include <stdatomic.h>

#include "board.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "clk_ctrl_os.h"            /* periph_rtc_apll_acquire, see audio_servo_apply */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "freertos/ringbuf.h"

static const char *TAG = "sat.i2s";

/* 44.1 kHz because that is the only rate stock Bluedroid advertises for SBC
 * (bta_av_co.c, BTC_AV_SBC_DEFAULT_SAMP_FREQ), and the only one the host is
 * promised unless it says it can relock. */
#define AUDIO_RATE_DEFAULT      44100
#define AUDIO_BITS_DEFAULT      16

/* Conversion happens a chunk at a time on the Bluetooth stack's own task, so the
 * scratch buffer lives on that stack and has to be modest. Widening doubles the
 * data, so this holds 256 output samples either way. */
#define AUDIO_CONV_BYTES        512

/* Fixed WROOM internal-RAM budget: 139 ms at 44.1/16, only 32 ms at 96/32.
 * Backpressure and a 12 ms reserve absorb bursts without enlarging this ring. */
#define AUDIO_RING_BYTES        (24 * 1024)
#define AUDIO_CHUNK_BYTES       1024
#define AUDIO_WRITE_TIMEOUT_MS  10
#define AUDIO_TRANSFER_BUDGET_MS 100
#ifndef AUDIO_DIAGNOSTIC_FIXED_TRIM
#define AUDIO_DIAGNOSTIC_FIXED_TRIM 0
#endif

#define AUDIO_TASK_STACK        3072
#define AUDIO_TASK_PRIO         (configMAX_PRIORITIES - 3)

/*
 * Diagnostic: clock a sine out of I2S instead of anything that came over the air.
 * Zero disables it, and zero is how this ships.
 *
 * It exists because "the slots are wrong" and "what arrives in them is wrong" sound
 * identical through a speaker, and every counter this file keeps sits upstream of
 * the pins. A tone generated here skips Bluetooth, the decoder, the conversion and
 * the ring: what reaches the host is this table and the peripheral's framing, and
 * nothing else. Clean tone means the wire and the host agree and the fault is
 * upstream; noise means the fault is the framing, and no amount of staring at
 * decoder counters would have said so.
 *
 * Paced by the DMA, so it needs no clock of its own: i2s_channel_write blocks
 * until there is room, which is exactly the sample rate.
 */
#define AUDIO_TEST_TONE_HZ      0
#define AUDIO_TEST_TONE_DBFS    (-6)

static i2s_chan_handle_t  s_tx;
static RingbufHandle_t    s_ring;
static SemaphoreHandle_t  s_lock;       /* guards the channel against retune-while-writing */
static SemaphoreHandle_t  s_producer_lock;
/* Control callers blocked on s_lock, see ctrl_take_lock(). */
static atomic_uint       s_ctrl_waiting;
static atomic_uint       s_gate = 1; /* bit 0 closed; upper bits invalidate old resumes */
static bool              s_running;
static bool              s_slave = true;   /* protocol 1.9, see i2s_open() */
static bool              s_fault;
/* s_fault was raised because audio_out_set_role found no channel it could open. That one
 * ends when a later call opens one: the fault was the missing channel, and a fresh channel
 * has fresh DMA buffers. Every other fault holds until reboot. */
static bool              s_open_fault;
static TaskHandle_t s_writer_task;
static _Atomic(TaskHandle_t) s_decoder_task;
static uint32_t s_packet_gate, s_packet_epoch, s_packet_start_ms, s_packet_src_frame;
static uint32_t s_packet_frames;        /* PCM frames the packet being decoded produced */
static atomic_uint s_frames_per_packet_q4;  /* their running mean, x16: what one queued packet holds */
static atomic_uint s_play_epoch;
static uint32_t s_writer_gate, s_writer_epoch;
static atomic_uint s_decoder_stack;
static int64_t s_decode_start_us;
static atomic_uint s_decode_last_us;
static uint32_t s_packet_wait_us;
static atomic_uint s_short_writes, s_tx_errors, s_tx_cancelled_bytes;
/* Slave only: writes that timed out while no DMA descriptor finished, i.e. while the
 * host was not driving BCLK. A state of the link, not a fault, so not in s_tx_errors. */
static atomic_uint s_tx_noclock, s_tx_noclock_us;
static atomic_uint s_pcm_cancelled_frames, s_dropped_frames, s_backpressure;
static atomic_uint s_tx_events, s_tx_gap_us, s_tx_last_us, s_tx_qovf_us;
static atomic_uint s_format_qovf, s_rebuffers, s_writer_gap_us;
static atomic_uint s_decode_gap_us, s_decode_max_us, s_decode_active_max_us;
static atomic_uint s_encoded_depth, s_encoded_max, s_encoded_drops, s_sequence_gaps;
static atomic_uint s_servo_changes, s_servo_last_us;
static bool s_tx_recover;
static bool s_buffering = true;
static atomic_uint s_encoded_drop_us, s_pcm_drop_us, s_tx_error_us;
static atomic_uint s_empty_polls, s_wait_recovered, s_wait_timeout, s_wait_cancel, s_wait_max_us;

/* Independent of s_lock: a delayed stats snapshot must not delay event draining.
 * 864 bytes of event storage; all copies under this mux, all logging outside it. */
#define AUDIO_REBUFFER_EVENTS 16u
#define AUDIO_TRIM_EVENTS 8u
static portMUX_TYPE s_event_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_event_fifo_t s_rebuffer_events, s_trim_events;
static audio_event_t s_rebuffer_history[AUDIO_REBUFFER_EVENTS], s_trim_history[AUDIO_TRIM_EVENTS];
static audio_event_state_t s_events;

static void audio_record_rebuffer(uint32_t gate, uint32_t epoch, uint32_t fill, unsigned reason)
{
    audio_event_t event;
    uint32_t now = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_event_lock);
    bool record = audio_event_rebuffer(&s_events, &event, now, gate, epoch, fill, reason);
    portEXIT_CRITICAL(&s_event_lock);
    if (!record) return;
    if (event.kind == AUDIO_EVENT_REBUFFER_BEGIN) {
        /* Rare read-only query, never inside the event mux. IDF water_mark is
         * a descriptor/cursor percentage, not a count of remaining valid PCM.
         * In particular IDF can skip a nearly stale descriptor before qovf. */
        i2s_tuning_info_t info = {0};
        if (i2s_channel_tune_rate(s_tx, NULL, &info) == ESP_OK)
            event.dma_proxy_pct = (int32_t)info.water_mark;
    }
    portENTER_CRITICAL(&s_event_lock);
    audio_event_push(&s_rebuffer_events, s_rebuffer_history, AUDIO_REBUFFER_EVENTS, event);
    portEXIT_CRITICAL(&s_event_lock);
}

static void audio_record_trim(uint32_t begin, uint32_t end, int old_ppm, int new_ppm, int result)
{
    audio_event_t event = {AUDIO_EVENT_TRIM, begin, end, s_writer_gate, s_writer_epoch,
                           old_ppm, new_ppm, result, -1};
    portENTER_CRITICAL(&s_event_lock);
    audio_event_push(&s_trim_events, s_trim_history, AUDIO_TRIM_EVENTS, event);
    portEXIT_CRITICAL(&s_event_lock);
}

static void counter_max(atomic_uint *counter, uint32_t value)
{
    unsigned old = atomic_load(counter);
    while (value > old && !atomic_compare_exchange_weak(counter, &old, value)) {}
}
static void flush_ring(void);
static esp_err_t clear_dma(void);
static uint32_t           s_rate_hz = AUDIO_RATE_DEFAULT;
static atomic_uint       s_dropped;
/* Polls of the ring that came back empty, per report window. See audio_out.h for
 * why this is not the underrun it used to be named after. */
static volatile uint32_t  s_ring_dry;
/* TX DMA descriptor-queue overflows: the real starvation, counted in the I2S ISR. */
static atomic_uint       s_dma_starved;
/* False until the writer has fed the DMA once since the stream started. The first
 * descriptors of a fresh stream are unclaimed by definition, and counting that as
 * a fault is how a counter starts crying wolf. */
static atomic_bool        s_dma_primed;
static volatile uint32_t  s_fill_min = UINT32_MAX;
static volatile uint32_t  s_fill_max;
/* Sum and count of the fills sampled in this window, for the mean. 64-bit because
 * a 10 s window at 100 polls/s of a 24 KiB ring is far inside it but the next
 * person to widen either number should not have to check. */
static volatile uint64_t  s_fill_sum;
static volatile uint32_t  s_fill_n;
/* Guards the window counters above (dry, fill min/max/sum/n): the writer updates
 * them once per chunk and the report task reads and resets them. Not s_lock: the
 * writer holds that across every DMA write, and in slave mode, waiting on the host's
 * clock, it is almost never free -- a try-take failed every report for a whole
 * session (11.10.2026), so the window was never shown. */
static portMUX_TYPE       s_window_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool        s_streaming;   /* an empty ring only matters while playing */
static volatile uint32_t  s_peak;
/*
 * The same figure taken again at the far end of the pipe, on the bytes the DMA is
 * about to clock out.
 *
 * `s_peak` answers one question -- did the decoder produce anything -- and it
 * answers it about a buffer that is still four function calls away from a pin.
 * This one answers whether anything reaches the wire. They can only disagree if
 * something between them is broken, and when they do disagree the difference
 * says which half to look in. `s_wire_head` keeps the first frame of whichever
 * chunk was loudest, because a bit pattern names the fault where a magnitude
 * only reports it.
 */
static volatile uint32_t  s_wire_peak;
static uint8_t            s_wire_head[8];
/* Raised on the decoder task, cleared on the reporting task. Both are read-
 * modify-write and neither is atomic on its own: the clear landing between the
 * other's read and its write-back is simply undone, and the figure can stay
 * pinned to an old transient for many windows. A spinlock costs nothing here --
 * it is taken once per packet, not once per sample. */
static portMUX_TYPE       s_peak_lock = portMUX_INITIALIZER_UNLOCKED;
/* Volatile because both are written by the Bluetooth task and read by the writer
 * task, and the writer reads them inside a loop it never leaves. */
static volatile uint8_t   s_slot_bits = AUDIO_BITS_DEFAULT;   /* on the wire */
static volatile uint8_t   s_src_bits  = 16;                   /* out of the decoder */
static const char        *s_codec_name = "?";

/* These hooks are called only around decode_packet on A2DP_DECODER. The token
 * spans all PCM callbacks from that packet, including after a gate reopens. */
void sat_audio_decode_begin(unsigned depth)
{
    s_decoder_task = xTaskGetCurrentTaskHandle();
    uint32_t old_gate = s_packet_gate, old_epoch = s_packet_epoch;
    s_packet_gate = atomic_load(&s_gate);
    s_packet_epoch = atomic_load(&s_play_epoch);
    s_decode_start_us = esp_timer_get_time();
    s_packet_start_ms = (uint32_t)(s_decode_start_us / 1000);
    s_packet_wait_us = 0;
    s_packet_src_frame = (s_src_bits / 8u) * 2u;
    s_packet_frames = 0;
    if (s_decode_last_us && old_gate == s_packet_gate && old_epoch == s_packet_epoch &&
        !(s_packet_gate & 1u) && s_streaming)
        counter_max(&s_decode_gap_us, (uint32_t)s_decode_start_us - atomic_load(&s_decode_last_us));
    atomic_store(&s_decode_last_us, (uint32_t)s_decode_start_us);
    atomic_store(&s_encoded_depth, depth);
    counter_max(&s_encoded_max, depth);
}
void sat_audio_decode_end(void)
{
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - s_decode_start_us);
    atomic_store(&s_decoder_stack, uxTaskGetStackHighWaterMark(NULL));
    counter_max(&s_decode_max_us, elapsed);
    counter_max(&s_decode_active_max_us, elapsed - s_packet_wait_us);
    if (s_packet_frames != 0u) {
        const uint32_t now = s_packet_frames * 16u;
        const uint32_t mean = atomic_load(&s_frames_per_packet_q4);
        atomic_store(&s_frames_per_packet_q4, mean ? mean - mean / 8u + now / 8u : now);
    }
}
void sat_audio_encoded_event(unsigned depth, unsigned dropped, unsigned gaps)
{
    atomic_store(&s_encoded_depth, depth);
    counter_max(&s_encoded_max, depth);
    atomic_fetch_add(&s_encoded_drops, dropped);
    if (dropped) atomic_store(&s_encoded_drop_us, (uint32_t)esp_timer_get_time());
    atomic_fetch_add(&s_sequence_gaps, gaps);
}

/*
 * Dither, for the one case that needs it: 24-bit aptX HD samples going out on a
 * 16-bit wire.
 *
 * Plain truncation is what the donor did, and it is the worst of the options: it
 * biases every sample the same direction and leaves an error that correlates with
 * the signal, which on quiet passages is audible as a rasp rather than as noise.
 * Rounding halves the error and removes the bias. Triangular dither, one LSB wide,
 * costs a little noise and in exchange makes what is left uncorrelated -- the
 * quantisation stops tracking the music.
 *
 * xorshift32 rather than rand(): a handful of instructions, no lock, no shared
 * state with anything else, and it runs on the Bluetooth task where neither a
 * lock nor a surprise is welcome.
 */
static volatile uint32_t  s_partial_frames;   /* ring handed back a part frame */
/* Bytes of an unfinished frame, carried to the front of the next write. Never
 * more than one frame less one byte, so eight bytes is the whole of it. */
static uint8_t            s_carry[8];
static size_t             s_carry_len;

static uint32_t s_dither_state = 0x2545F491u;

static inline uint32_t audio_xorshift32(void)
{
    uint32_t x = s_dither_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_dither_state = x;
    return x;
}

/* Triangular, spanning plus or minus one LSB of the 16-bit output, expressed in
 * the 32-bit input's units: one output LSB is 65536 of them. */
static inline int32_t audio_tpdf_dither(void)
{
    uint32_t r = audio_xorshift32();
    return (int32_t)(r & 0xFFFFu) - (int32_t)((r >> 16) & 0xFFFFu);
}

static inline int16_t audio_narrow_32_to_16(int32_t v)
{
    /* 64-bit on purpose: v can be within a dither's reach of INT32_MAX, and
     * wrapping there would turn the loudest sample in the track into the
     * quietest. Two extra instructions per sample at 88 kHz is nothing. */
    int64_t t = (int64_t)v + audio_tpdf_dither() + 32768;
    t >>= 16;
    if (t > 32767)  { return 32767; }
    if (t < -32768) { return -32768; }
    return (int16_t)t;
}

/*
 * The clock servo.
 *
 * The phone's crystal sets the pace of the stream and this device's APLL sets
 * the pace of the I2S bus, and the two differ by some tens of ppm. The ring is
 * the only elastic element between them: left alone it traverses its whole
 * range every ~46 minutes at 44.1 kHz/16-bit and every ~11 minutes at
 * 96 kHz/32-bit for a ±50 ppm difference, and ends in a dropped chunk or a
 * run of DMA zeros either way -- a click on a schedule.
 *
 * Once a second the mean fill of the ring over that second is compared with the
 * middle of the ring and MCLK is nudged through i2s_channel_tune_rate() so that
 * the fill drifts back. The error is in milliseconds of audio rather than
 * bytes, so the loop behaves the same at 44.1/16 (a 139 ms ring) as at 96/32
 * (32 ms). AUDIO_SERVO_KP ppm per millisecond gives a 100 s time constant in
 * the linear region; the integrator learns the standing offset so the fill
 * settles on the target rather than a Kp-proportional distance from it, and
 * with Ti = Kp/Ki = 400 s the pair is critically damped against the ring,
 * which is a pure integrator of the rate error. Authority is ±200 ppm, a
 * fiftieth of a percent, with 2 ppm steps so the APLL is not rewritten for
 * noise.
 *
 * Two loops in series, and they do not fight: this one tracks the phone from
 * the ring, the P4's ±500 ppm ASRC tracks this device's BCLK from its own ring,
 * and this one is the slower of the two by design.
 *
 * Only while a stream plays with the gate open. Between tracks the fill means
 * nothing, so the servo freezes and the integrator keeps the offset it has
 * learned; a format change resets everything, because the driver derives a
 * fresh clock and the phone that follows may be a different one.
 *
 * Every step re-derives the APLL live (i2s_common.c:1486, IDF 5.5.5): the sdm
 * coefficients are rewritten and a calibration cycle runs while BCLK is
 * expected to keep going. Whether it really does is a scope measurement, not a
 * code reading -- docs/DEVICE_SPEC.md, "Вихід I2S".
 */
#define AUDIO_SERVO_PERIOD_US   (1000 * 1000)
#define AUDIO_SERVO_TARGET      (AUDIO_RING_BYTES / 2)

/*
 * The reserve, as a slave (1.9). The ring alone is 32 ms at 96 kHz / 32-bit, and the phone's
 * packets arrive with gaps of 50-85 ms (bench logs of 30.09.2026): each gap longer than what
 * the ring held was a rebuffer, 7 to 110 ms of zeros in the music, every few seconds. At
 * 48 kHz the same ring holds two to four times as long, which is why 48 kHz sounded clean.
 * There is no RAM for a bigger ring, but the packets waiting in the A2DP queue hold the same
 * audio seven times denser: while the ring is full the decoder waits and packets queue. So
 * the servo counts both, ring and queue, and holds their sum at 150 ms, of which at most 12
 * packets queue (the queue holds 18 before it drops). 80 ms was not enough: over 10 minutes
 * of music at 96 kHz the phone fell behind by more than 500 ppm for a minute at a time and
 * caught up in bursts, and three of those deficits emptied an 80 ms reserve (30.09.2026).
 * A queued LHDC packet cost about 0.5 KB of heap there. Playback starts at half the reserve,
 * because the decoder may wait on a full ring only 100 ms per packet before it drops PCM;
 * the servo builds the rest.
 */
#define AUDIO_SLAVE_TARGET_MS     150u
#define AUDIO_SLAVE_RESERVE_PKTS  12u

/* Ring bytes plus the queued packets' PCM, in ring bytes. Writer task. */
static uint32_t slave_buffered(uint32_t fill)
{
    const uint32_t frame = (s_slot_bits / 8u) * 2u;
    const uint32_t per_packet = (atomic_load(&s_frames_per_packet_q4) * frame) >> 4;
    return fill + atomic_load(&s_encoded_depth) * per_packet;
}

static uint32_t slave_target(void)
{
    const uint32_t frame = (s_slot_bits / 8u) * 2u;
    const uint32_t per_packet = (atomic_load(&s_frames_per_packet_q4) * frame) >> 4;
    const uint32_t want  = AUDIO_SLAVE_TARGET_MS * (s_rate_hz * frame / 1000u);
    const uint32_t reach = AUDIO_RING_BYTES + AUDIO_SLAVE_RESERVE_PKTS * per_packet;
    return want < reach ? want : reach;
}
#define AUDIO_SERVO_MAX_PPM     200
#define AUDIO_SERVO_KP          10.0f       /* ppm per millisecond of fill error */
#define AUDIO_SERVO_KI          0.025f      /* ppm per millisecond, per second */
#define AUDIO_SERVO_STEP_PPM    2           /* smallest change worth an APLL rewrite */

/* The same loop as a slave (1.9), where its output is the converter's ratio instead of the
 * APLL. A ratio costs nothing to change, so it gets the exact value every second: critically
 * damped at a 100 s time constant, twice as fast as the APLL loop. What it measures moves in
 * whole packets, 15 ms each for LHDC at 96 kHz, so the gain stays low enough that half a
 * packet of noise moves the ratio by ~150 ppm, a quarter of a cent. Its authority is
 * 1000 ppm (1.7 cents, still inaudible), because the phone was seen to fall 500 ppm behind
 * for a minute at a time. */
#define AUDIO_ASRC_KP           20.0f       /* ppm per millisecond of fill error */
#define AUDIO_ASRC_KI           0.1f        /* ppm per millisecond, per second */
#define AUDIO_ASRC_MAX_PPM      1000.0f

static audio_asrc_t s_asrc;             /* writer task only, or with both locks held */
static int32_t      s_asrc_out[(AUDIO_ASRC_BLOCK + 2u) * 2u];
static atomic_uint  s_asrc_us;          /* time spent converting, lifetime */
static atomic_uint  s_asrc_best_cpf = UINT32_MAX;   /* fewest CPU cycles per output frame */
static atomic_uint  s_servo_fill_last;  /* the servo's last one-second mean fill, bytes */

static int      s_servo_ppm;            /* the correction last asked of the driver */
static int32_t  s_servo_delta_hz;       /* what the driver says it applied, MCLK */
static float    s_servo_integral;       /* ppm */
static uint64_t s_servo_fill_acc;       /* bytes, summed over the current second */
static uint32_t s_servo_fill_n;
static int64_t  s_servo_next_us;
static bool     s_servo_off;            /* the driver refused once; do not nag */

/* Writer task only: one reading of the fill, as many times a second as the
 * ring is read. An empty ring during a stream counts as zero. */
static inline void audio_servo_sample(uint32_t fill)
{
    s_servo_fill_acc += fill;
    s_servo_fill_n++;
}

/* With s_lock held: this moves the clock under i2s_channel_write. */
static void audio_servo_apply(int ppm)
{
    const uint32_t begin_us = (uint32_t)esp_timer_get_time();
    const int old_ppm = s_servo_ppm;
    i2s_tuning_info_t info = {0};
    esp_err_t err = i2s_channel_tune_rate(s_tx, NULL, &info);
    if (err != ESP_OK) {
        audio_record_trim(begin_us, (uint32_t)esp_timer_get_time(), old_ppm, ppm, err);
        s_servo_off = true;
        return;
    }
    const int32_t origin = info.curr_mclk_hz - info.delta_mclk_hz;
    const int32_t limit  = (int32_t)(((int64_t)origin * AUDIO_SERVO_MAX_PPM) / 1000000);
    const i2s_tuning_config_t cfg = {
        .tune_mode      = I2S_TUNING_MODE_SET,
        .tune_mclk_val  = origin + (int32_t)(((int64_t)origin * ppm) / 1000000),
        .max_delta_mclk = limit,
        .min_delta_mclk = -limit,
    };
    /* A reference of our own for the duration of the call. The driver drops
     * and retakes its only APLL reference around the re-derivation
     * (i2s_common.c:1487-1489, IDF 5.5.5); with nobody else holding one the
     * count reaches zero, which powers the PLL down under a running stream and
     * trips the assert in periph_rtc_apll_freq_set (clk_ctrl_os.c:111). Held
     * only here and never across a format change: with two holders the driver
     * is not allowed to move the APLL at all. */
    periph_rtc_apll_acquire();
    err = i2s_channel_tune_rate(s_tx, &cfg, &info);
    periph_rtc_apll_release();
    audio_record_trim(begin_us, (uint32_t)esp_timer_get_time(), old_ppm, ppm, err);
    if (err != ESP_OK) {
        atomic_fetch_add(&s_tx_errors, 1);
        s_servo_off = true;
        return;
    }
    atomic_fetch_add(&s_servo_changes, 1);
    atomic_store(&s_servo_last_us, (uint32_t)esp_timer_get_time());
    s_servo_ppm      = ppm;
    s_servo_delta_hz = info.delta_mclk_hz;
}

/* Forget everything learned and put the clock back on its nominal value. From
 * audio_out_configure(), under s_lock with the channel disabled: the driver
 * derives a fresh origin when the rate changes, but a width-only change leaves
 * the old correction standing, and the servo must not start from a lie. */
static void audio_servo_reset(void)
{
    s_servo_ppm = 0;
    s_servo_delta_hz = 0;
    s_servo_integral = 0.0f;
    audio_asrc_set_ppm(&s_asrc, 0.0f);
    audio_asrc_reset(&s_asrc);
    s_servo_fill_acc = 0;
    s_servo_fill_n = 0;
    s_servo_off = false;

    i2s_tuning_info_t info = {0};
    if (s_tx != NULL && i2s_channel_tune_rate(s_tx, NULL, &info) == ESP_OK &&
        info.delta_mclk_hz != 0) {
        const i2s_tuning_config_t back = { .tune_mode = I2S_TUNING_MODE_RESET };
        periph_rtc_apll_acquire();
        i2s_channel_tune_rate(s_tx, &back, NULL);
        periph_rtc_apll_release();
    }
}

/* Writer task, with s_lock held, once a second. */
static void audio_servo_tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (now < s_servo_next_us) {
        return;
    }
    s_servo_next_us = now + AUDIO_SERVO_PERIOD_US;

    const uint32_t n   = s_servo_fill_n;
    const uint64_t acc = s_servo_fill_acc;
    s_servo_fill_n   = 0;
    s_servo_fill_acc = 0;

    if (s_buffering || !s_running || !s_streaming || (atomic_load(&s_gate) & 1u) || n == 0) {
        return;     /* nothing to track; the integrator keeps what it has learned */
    }
    /* A slave has no clock of its own to tune: the host's clocks the link (1.9), and the
     * correction goes to the converter instead, which no diagnostic switch pins. */
    if (!s_slave && (AUDIO_DIAGNOSTIC_FIXED_TRIM || s_servo_off)) {
        return;
    }
    const float kp  = s_slave ? AUDIO_ASRC_KP : AUDIO_SERVO_KP;
    const float ki  = s_slave ? AUDIO_ASRC_KI : AUDIO_SERVO_KI;
    const float lim = s_slave ? AUDIO_ASRC_MAX_PPM : (float)AUDIO_SERVO_MAX_PPM;

    const unsigned frame_bytes = (unsigned)(s_slot_bits / 8) * 2u;
    const float byte_rate = (float)s_rate_hz * (float)frame_bytes;
    const float fill      = (float)(acc / n);
    atomic_store(&s_servo_fill_last, (uint32_t)(acc / n));
    /* Positive: the ring is fuller than wanted, the phone is ahead, and the
     * bus has to run faster to catch up. */
    const float target    = s_slave ? (float)slave_target() : (float)AUDIO_SERVO_TARGET;
    const float err_ms    = (fill - target) * 1000.0f / byte_rate;

    float out = kp * err_ms + s_servo_integral;
    /* Anti-windup: pinned against the limit in the direction of the error, the
     * integrator would only be storing up an overshoot. */
    const bool pinned = (out >=  lim && err_ms > 0.0f) ||
                        (out <= -lim && err_ms < 0.0f);
    if (!pinned) {
        s_servo_integral += ki * err_ms;    /* dt is the one-second period */
        if (s_servo_integral >  lim) { s_servo_integral =  lim; }
        if (s_servo_integral < -lim) { s_servo_integral = -lim; }
        out = kp * err_ms + s_servo_integral;
    }
    if (out >  lim) { out =  lim; }
    if (out < -lim) { out = -lim; }

    const int ppm  = (int)(out >= 0.0f ? out + 0.5f : out - 0.5f);
    if (s_slave) {
        audio_asrc_set_ppm(&s_asrc, out);   /* with s_lock held, like every converter call */
        s_servo_ppm = ppm;
        return;
    }
    const int step = ppm - s_servo_ppm;
    if (step >= AUDIO_SERVO_STEP_PPM || step <= -AUDIO_SERVO_STEP_PPM) {
        audio_servo_apply(ppm);
    }
}

/* Ten seconds, and only while something is playing. A line an hour from an idle
 * device is noise; a line every ten seconds from a stuttering one is the whole
 * diagnosis. */
#define AUDIO_REPORT_US  (10 * 1000 * 1000)

static void audio_report_events(void)
{
    static uint32_t last_rebuffer_overflow, last_trim_overflow;
    /* Bounded drain, including when streaming stops. Never acquire s_lock here.
     * low32 microseconds wrap after 71.6 min: subtract timestamps modulo 2^32.
     * begin: a=pre-poll allocated ring bytes, b=last successful enqueue us,
     * c=empty/startup. Fill predates servo/receive and is not proof of unread
     * data at failure. The enqueue timestamp is sampled when recording begins.
     * end: a=accepted zero frames, b=ring bytes, c=ready/cancelled.
     * trim: a=old ppm, b=requested ppm, c=esp_err_t; begin/end bracket the call. */
    for (unsigned i = 0; i < AUDIO_REBUFFER_EVENTS + AUDIO_TRIM_EVENTS; ++i) {
        audio_event_t event;
        portENTER_CRITICAL(&s_event_lock);
        bool available = audio_event_pop(&s_rebuffer_events, s_rebuffer_history,
                                          AUDIO_REBUFFER_EVENTS, &event);
        if (!available)
            available = audio_event_pop(&s_trim_events, s_trim_history, AUDIO_TRIM_EVENTS, &event);
        portEXIT_CRITICAL(&s_event_lock);
        if (!available) break;
        if (event.kind == AUDIO_EVENT_TRIM) {
            ESP_LOGI(TAG, "trim_event begin_us=%u end_us=%u gate=%u epoch=%u old_ppm=%" PRId32
                     " requested_ppm=%" PRId32 " result=%" PRId32,
                     (unsigned)event.begin_us, (unsigned)event.end_us,
                     (unsigned)event.gate, (unsigned)event.epoch, event.a, event.b, event.c);
        } else if (event.kind == AUDIO_EVENT_REBUFFER_BEGIN) {
            ESP_LOGI(TAG, "rebuffer_begin us=%u gate=%u epoch=%u fill_bytes=%u pcm_enqueue_us=%u reason=%u dma_proxy_pct=%" PRId32,
                     (unsigned)event.begin_us, (unsigned)event.gate, (unsigned)event.epoch,
                     (unsigned)(uint32_t)event.a, (unsigned)(uint32_t)event.b, (unsigned)event.c,
                     event.dma_proxy_pct);
        } else {
            ESP_LOGI(TAG, "rebuffer_end begin_us=%u end_us=%u gate=%u epoch=%u zero_frames=%u fill_bytes=%u reason=%u",
                     (unsigned)event.begin_us, (unsigned)event.end_us,
                     (unsigned)event.gate, (unsigned)event.epoch,
                     (unsigned)(uint32_t)event.a, (unsigned)(uint32_t)event.b, (unsigned)event.c);
        }
    }
    /* Overflow must remain visible even if the stream stopped before draining. */
    portENTER_CRITICAL(&s_event_lock);
    uint32_t rebuffer_overflow = s_rebuffer_events.overflow;
    uint32_t trim_overflow = s_trim_events.overflow;
    portEXIT_CRITICAL(&s_event_lock);
    if (rebuffer_overflow != last_rebuffer_overflow || trim_overflow != last_trim_overflow) {
        ESP_LOGW(TAG, "audio_event_overflow rebuffer=%u trim=%u",
                 (unsigned)rebuffer_overflow, (unsigned)trim_overflow);
        last_rebuffer_overflow = rebuffer_overflow;
        last_trim_overflow = trim_overflow;
    }
}

static void audio_report(void)
{
    static int64_t next_us;
    int64_t now = esp_timer_get_time();
    if (!s_streaming || now < next_us) {
        return;
    }
    next_us = now + AUDIO_REPORT_US;

    audio_out_stats_t st;
    if (audio_out_get_stats(&st)) {
        /* Peak as a fraction of full scale, in tenths of a percent, so that a glance
         * says whether the signal is loud without anyone reaching for a calculator. */
        unsigned peak_permille = (unsigned)((st.peak * 1000u) / 32768u);

        /* Milliseconds of audio standing in the ring, which is the latency this device
         * is adding and the number that says whether the buffer is healthy or merely
         * full. Bytes alone do not, because the same count means different things at
         * different rates and widths. */
        unsigned frame_bytes = (unsigned)(s_slot_bits / 8) * 2u;
        /* The mean, not the peak. Reporting only min and max is what let a ring that
         * spends most of its time near empty read as "40-60 ms" for a whole session
         * (item 10.10): those were high-water marks, and the low one beside them was
         * set by the empty-ring branch itself. */
        unsigned fill_ms = (frame_bytes && s_rate_hz)
                         ? (unsigned)((uint64_t)st.fill_avg * 1000u / (frame_bytes * s_rate_hz))
                         : 0u;

        ESP_LOGI(TAG, "%s %" PRIu32 " Hz %u-bit slots: ring %u/%u/%u of %u (%u ms mean)"
                      ", dry %" PRIu32 ", dma starved %" PRIu32
                      ", dropped %" PRIu32 ", peak %" PRIu32 "/32768 (%u.%u%%), wire %" PRIu32
                      " [%02x %02x %02x %02x %02x %02x %02x %02x], part %" PRIu32 ", free dma %u"
                      ", %s %+d ppm (%+" PRId32 " Hz)",
                 s_codec_name, s_rate_hz, s_slot_bits,
                 (unsigned)st.fill_min, (unsigned)st.fill_avg, (unsigned)st.fill_max,
                 (unsigned)AUDIO_RING_BYTES, fill_ms,
                 st.ring_dry, st.dma_starved, st.dropped_bytes, st.peak,
                 peak_permille / 10, peak_permille % 10, st.wire_peak,
                 st.wire_head[0], st.wire_head[1], st.wire_head[2], st.wire_head[3],
                 st.wire_head[4], st.wire_head[5], st.wire_head[6], st.wire_head[7],
                 s_partial_frames,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 s_slave ? "asrc" : "clock", s_servo_ppm, s_servo_delta_hz);
    } else {
        ESP_LOGI(TAG, "stats snapshot skipped: output not started");
    }
    /* These lifetime counters must remain visible while the writer owns s_lock.
     * Timestamp their acquisition, not the earlier report scheduling check. */
    now = esp_timer_get_time();
    ESP_LOGI(TAG, "uptime_us=%" PRId64 " gate=%u streaming=%u slave=%u fixed_trim=%u "
             "pcm_drop_frames=%u cancel_frames=%u backpressure=%u short=%u txerr=%u txnoclk=%u txcancel_bytes=%u "
             "txevents=%u txgap_us=%u txlast_us=%u qovf_us=%u fmtqovf=%u writer_gap_us=%u rebuffer=%u "
             "decode_gap_us=%u decode_us=%u active_us=%u encoded=%u/%u drop=%u seqgap=%u "
             "trim_changes=%u trim_us=%u internal_min=%u largest=%u writer_stack=%u decoder_stack=%u "
             "asrc_us=%u asrc_cpf=%u servo_ppm=%d servo_fill=%u packet_frames=%u",
             now, atomic_load(&s_gate), (unsigned)s_streaming, (unsigned)s_slave, AUDIO_DIAGNOSTIC_FIXED_TRIM,
             atomic_load(&s_dropped_frames), atomic_load(&s_pcm_cancelled_frames), atomic_load(&s_backpressure),
             atomic_load(&s_short_writes), atomic_load(&s_tx_errors), atomic_load(&s_tx_noclock),
             atomic_load(&s_tx_cancelled_bytes),
             atomic_load(&s_tx_events), atomic_load(&s_tx_gap_us), atomic_load(&s_tx_last_us),
             atomic_load(&s_tx_qovf_us), atomic_load(&s_format_qovf), atomic_load(&s_writer_gap_us), atomic_load(&s_rebuffers),
             atomic_load(&s_decode_gap_us), atomic_load(&s_decode_max_us), atomic_load(&s_decode_active_max_us),
             atomic_load(&s_encoded_depth), atomic_load(&s_encoded_max), atomic_load(&s_encoded_drops), atomic_load(&s_sequence_gaps),
             atomic_load(&s_servo_changes), atomic_load(&s_servo_last_us),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(s_writer_task),
             atomic_load(&s_decoder_stack), atomic_load(&s_asrc_us),
             atomic_exchange(&s_asrc_best_cpf, UINT32_MAX), s_servo_ppm,
             atomic_load(&s_servo_fill_last), atomic_load(&s_frames_per_packet_q4) / 16u);
    ESP_LOGI(TAG, "event_us: encoded_drop=%u pcm_drop=%u tx_error=%u tx_noclock=%u decode_last=%u",
             atomic_load(&s_encoded_drop_us), atomic_load(&s_pcm_drop_us),
             atomic_load(&s_tx_error_us), atomic_load(&s_tx_noclock_us), atomic_load(&s_decode_last_us));
    audio_event_state_t events;
    uint32_t rebuffer_overflow, trim_overflow;
    portENTER_CRITICAL(&s_event_lock);
    events = s_events;
    rebuffer_overflow = s_rebuffer_events.overflow;
    trim_overflow = s_trim_events.overflow;
    portEXIT_CRITICAL(&s_event_lock);
    ESP_LOGI(TAG, "silence: frames=%u writes=%u last_us=%u gate=%u epoch=%u cancel_frames=%u "
             "pcm_enqueue_us=%u rebuffer_begin_us=%u rebuffer_end_us=%u active=%u event_overflow=%u/%u",
             (unsigned)events.zero_frames, (unsigned)events.zero_writes, (unsigned)events.zero_last_us,
             (unsigned)events.zero_gate, (unsigned)events.zero_epoch, (unsigned)events.zero_cancel_frames,
             (unsigned)events.pcm_last_us, (unsigned)events.rebuffer_begin_us,
             (unsigned)events.rebuffer_end_us, (unsigned)events.active,
             (unsigned)rebuffer_overflow, (unsigned)trim_overflow);
    ESP_LOGI(TAG, "refill: empty_poll=%u wait_recovered=%u wait_timeout=%u wait_cancel=%u maxwait_us=%u",
             atomic_load(&s_empty_polls), atomic_load(&s_wait_recovered), atomic_load(&s_wait_timeout),
             atomic_load(&s_wait_cancel), atomic_load(&s_wait_max_us));
}

static void audio_report_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        audio_report_events();
        audio_report();
    }
}

/*
 * Peak of a block of wire-format bytes, on the same 0..32768 scale as the source
 * meter so that the two numbers can be compared without arithmetic.
 *
 * Assembled byte by byte on purpose. The ring hands back a pointer to wherever
 * the data happens to sit, and the carry logic can advance it by one to seven
 * bytes; a 32-bit load from an odd address is not slow on this core, it is an
 * exception. Little endian, matching what the conversion above writes and what
 * the I2S peripheral expects.
 */
static uint32_t audio_wire_peak(const uint8_t *p, size_t len)
{
    const size_t step = (s_slot_bits == 32) ? 4u : 2u;
    uint32_t peak = 0;

    for (size_t i = 0; i + step <= len; i += step) {
        int32_t v;
        if (step == 4) {
            v = (int32_t)((uint32_t)p[i]            | ((uint32_t)p[i + 1] << 8) |
                          ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24));
        } else {
            /* Shifted up so both widths land on one scale. */
            v = (int32_t)(int16_t)((uint16_t)p[i] | ((uint16_t)p[i + 1] << 8));
            v = (int32_t)((uint32_t)v << 16);
        }
        uint32_t mag = (uint32_t)(v < 0 ? -(int64_t)v : (int64_t)v);
        mag >>= 16;
        if (mag > peak) { peak = mag; }
    }
    return peak;
}

/* Descriptor-return and queue-overflow events are software service evidence,
 * not proof of a physical BCLK interruption. No logging or buffer writes in ISR.
 * ESP32 IDF 5.5.5 i2s_common.c keeps auto_clear_after_cb enabled with on_sent. */
static bool audio_dma_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle; (void)event; (void)ctx;
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t last = atomic_exchange(&s_tx_last_us, now);
    atomic_fetch_add(&s_tx_events, 1);
    if (last) counter_max(&s_tx_gap_us, now - last);
    return false;
}
static bool audio_dma_starved(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle; (void)event; (void)ctx;
    atomic_store(&s_tx_qovf_us, (uint32_t)esp_timer_get_time());
    if (atomic_load(&s_gate) & 1u) atomic_fetch_add(&s_format_qovf, 1);
    else if (s_streaming && s_dma_primed) { s_dma_starved++; }
    return false;
}

typedef struct { uint32_t gate, epoch; bool silence; } transfer_context_t;
typedef struct { uint32_t gate, epoch; size_t borrowed_len; } refill_context_t;

static bool refill_current(void *ctx)
{
    refill_context_t *c = ctx;
    return c->gate == atomic_load(&s_gate) && c->epoch == atomic_load(&s_play_epoch) &&
           !(c->gate & 1u) && s_streaming && s_running;
}
static uint32_t refill_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)esp_timer_get_time();
}
static void *refill_receive(void *ctx, size_t *len, unsigned ticks, size_t max_bytes)
{
    refill_context_t *c = ctx;
    void *item = xRingbufferReceiveUpTo(s_ring, len, (TickType_t)ticks, max_bytes);
    c->borrowed_len = item ? *len : 0;
    return item;
}
static void refill_return_item(void *ctx, void *item)
{
    refill_context_t *c = ctx;
    /* A generation changed during receive: the helper owns and returns this
     * stale borrowed item, so it will not be counted by the next ring flush. */
    atomic_fetch_add(&s_pcm_cancelled_frames, c->borrowed_len / ((s_slot_bits / 8u) * 2u));
    vRingbufferReturnItem(s_ring, item);
}
static audio_refill_result_t audio_wait_refill(uint32_t gate, uint32_t epoch)
{
    /* Called only after a steady nonblocking poll returned NULL, with s_lock
     * still held. Exactly one RTOS tick, not a millisecond and not a retry loop.
     * This measures a receive recovery; it does not prove DMA avoided a gap. */
    atomic_fetch_add(&s_empty_polls, 1);
    refill_context_t ctx = { gate, epoch, 0 };
    const audio_refill_ops_t ops = { &ctx, refill_current, refill_now, refill_receive, refill_return_item };
    audio_refill_result_t result = audio_refill_once(&ops, AUDIO_CHUNK_BYTES);
    counter_max(&s_wait_max_us, result.elapsed_us);
    if (result.status == AUDIO_REFILL_RECOVERED) atomic_fetch_add(&s_wait_recovered, 1);
    else if (result.status == AUDIO_REFILL_TIMEOUT) atomic_fetch_add(&s_wait_timeout, 1);
    else atomic_fetch_add(&s_wait_cancel, 1);
    return result;
}
static uint32_t transfer_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}
static bool transfer_current(void *ctx)
{
    transfer_context_t *c = ctx;
    return c->gate == atomic_load(&s_gate) && c->epoch == atomic_load(&s_play_epoch) && (c->silence || (!(c->gate & 1u) && s_streaming));
}
static bool tx_send(void *ctx, const uint8_t *data, size_t len, size_t *sent)
{
    (void)ctx;
    esp_err_t err = i2s_channel_write(s_tx, data, len, sent, AUDIO_WRITE_TIMEOUT_MS);
    if (*sent < len) atomic_fetch_add(&s_short_writes, 1);
    return err == ESP_OK || err == ESP_ERR_TIMEOUT;
}
static size_t i2s_write_locked(const void *data, size_t len, bool silence)
{
    transfer_context_t ctx = { s_writer_gate, s_writer_epoch, true };
    audio_transfer_ops_t ops = { &ctx, transfer_current, transfer_now, tx_send };
    const uint32_t dma_events = atomic_load(&s_tx_events);
    size_t accepted;
    audio_transfer_result_t result = audio_transfer(&ops, data, len, transfer_now(NULL),
                                                    AUDIO_TRANSFER_BUDGET_MS, &accepted);
    if (result != AUDIO_TRANSFER_OK) {
        atomic_fetch_add(&s_tx_cancelled_bytes, len - accepted);
        /* IDF normally accepts frame-aligned DMA prefixes. If a future driver
         * cancels mid-frame, reset its cursor before submitting new PCM. */
        if (accepted % ((s_slot_bits / 8u) * 2u)) s_tx_recover = true;
        if (result == AUDIO_TRANSFER_TIMEOUT && s_slave &&
            atomic_load(&s_tx_events) == dma_events) {
            /* Not one descriptor finished in the whole budget: a slave with no BCLK.
             * The host clocks only at an announced rate (PROTOCOL.md 3.5), so this is
             * every boot until a phone negotiates a codec. Counted apart from faults,
             * and silence is not recovered: DMA holds zeros, and resetting it ten
             * times a second fixed nothing (52 resets in 5 s on 11.10.2026). PCM still
             * is, so that none of it plays when the clock comes back. */
            atomic_fetch_add(&s_tx_noclock, 1);
            atomic_store(&s_tx_noclock_us, (uint32_t)esp_timer_get_time());
            if (!silence) {
                atomic_fetch_add(&s_play_epoch, 1u);
                s_tx_recover = true;
            }
        } else if (result != AUDIO_TRANSFER_CANCELLED) {
            atomic_fetch_add(&s_tx_errors, 1);
            atomic_store(&s_tx_error_us, (uint32_t)esp_timer_get_time());
            /* Explicit recovery: invalidate this packet/tail, clear DMA, retry
             * the same negotiated format. No old sample may follow a new one. */
            atomic_fetch_add(&s_play_epoch, 1u);
            s_tx_recover = true;
        }
    }
    return accepted;
}

static void i2s_write_silence_locked(const void *data, size_t len)
{
    const uint32_t gate = s_writer_gate, epoch = s_writer_epoch;
    const bool streaming_open = s_streaming && !(gate & 1u) &&
                                gate == atomic_load(&s_gate) && epoch == atomic_load(&s_play_epoch);
    const unsigned frame = (s_slot_bits / 8u) * 2u;
    size_t accepted = i2s_write_locked(data, len, true);
    const bool cancelled = gate != atomic_load(&s_gate) || epoch != atomic_load(&s_play_epoch);
    uint32_t now = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_event_lock);
    audio_event_zeros(&s_events, now, gate, epoch, frame, accepted, streaming_open, cancelled);
    portEXIT_CRITICAL(&s_event_lock);
}

/* Whole frames of ring PCM to the pins. Straight through while this device makes the clock;
 * through the converter while a host does (1.9), because then the samples are all that is
 * left to follow the phone with. Writer task, s_lock held. */
static void pcm_write_locked(const uint8_t *wire, size_t len)
{
    if (!s_slave || s_slot_bits != 32u) {
        i2s_write_locked(wire, len, false);
        return;
    }
    size_t frames = len / 8u;
    uint32_t spent = 0;
    while (frames != 0u) {
        size_t used = 0;
        const uint32_t t0 = (uint32_t)esp_timer_get_time();
        const uint32_t c0 = esp_cpu_get_cycle_count();
        const size_t made = audio_asrc_run(&s_asrc, wire, frames, s_asrc_out,
                                           AUDIO_ASRC_BLOCK + 2u, &used);
        const uint32_t cycles = esp_cpu_get_cycle_count() - c0;
        spent += (uint32_t)esp_timer_get_time() - t0;
        /* The fewest cycles a frame took this window: the cost without whatever preempted. */
        if (made >= 32u) {
            const uint32_t cpf = cycles / (uint32_t)made;
            if (cpf < atomic_load(&s_asrc_best_cpf)) atomic_store(&s_asrc_best_cpf, cpf);
        }
        if (made != 0u) i2s_write_locked(s_asrc_out, made * 8u, false);
        wire += used * 8u;
        frames -= used;
        if (used == 0u && made == 0u) break;
    }
    atomic_fetch_add(&s_asrc_us, spent);
}

#if AUDIO_TEST_TONE_HZ
#include <math.h>

#define AUDIO_TONE_TABLE  256u      /* power of two, so the phase masks instead of dividing */

static int16_t  s_tone[AUDIO_TONE_TABLE];
static uint32_t s_tone_phase;       /* Q16; the high half indexes the table */

static void audio_tone_init(void)
{
    const double amp = 32767.0 * pow(10.0, (double)AUDIO_TEST_TONE_DBFS / 20.0);
    for (unsigned i = 0; i < AUDIO_TONE_TABLE; i++) {
        s_tone[i] = (int16_t)lrint(amp * sin(2.0 * M_PI * (double)i / (double)AUDIO_TONE_TABLE));
    }
}

/*
 * One block in whatever the wire currently takes, both slots carrying the same
 * sample. A swapped pair is then inaudible and a mis-set slot width is not, which
 * is the only distinction this test is for.
 */
static size_t audio_tone_fill(uint8_t *buf, size_t cap)
{
    const unsigned bits   = s_slot_bits;
    const size_t   frame  = (size_t)(bits / 8) * 2u;
    const size_t   frames = cap / frame;
    const uint32_t step   = (uint32_t)(((uint64_t)AUDIO_TEST_TONE_HZ * AUDIO_TONE_TABLE << 16)
                                       / (s_rate_hz ? s_rate_hz : AUDIO_RATE_DEFAULT));
    size_t off = 0;

    for (size_t i = 0; i < frames; i++) {
        int16_t v = s_tone[(s_tone_phase >> 16) & (AUDIO_TONE_TABLE - 1u)];
        s_tone_phase += step;
        for (int ch = 0; ch < 2; ch++) {
            if (bits == 32) {
                uint32_t w = (uint32_t)v << 16;     /* left aligned, as the real path leaves it */
                buf[off++] = (uint8_t)(w >>  0);
                buf[off++] = (uint8_t)(w >>  8);
                buf[off++] = (uint8_t)(w >> 16);
                buf[off++] = (uint8_t)(w >> 24);
            } else {
                buf[off++] = (uint8_t)((uint16_t)v >> 0);
                buf[off++] = (uint8_t)((uint16_t)v >> 8);
            }
        }
    }
    return off;
}
#endif /* AUDIO_TEST_TONE_HZ */

/*
 * The writer gives s_lock at the end of a pass and takes it again a few dozen
 * instructions later. A mutex give hands nothing over: it only readies the waiter,
 * which must win the take again. bt_av sits below the writer and is woken on the
 * other core, so in that race it loses nearly every time and a format transaction
 * could wait for as long as no interrupt happened to land in the gap. So control
 * callers announce themselves, and the writer stays off the lock until they have
 * had it. They wait in a format transaction or a role change, with the gate closed,
 * so the writer has only zeros to give and DMA plays zeros on its own meanwhile. */
static void ctrl_take_lock(void)
{
    atomic_fetch_add(&s_ctrl_waiting, 1u);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    atomic_fetch_sub(&s_ctrl_waiting, 1u);
}

/* Writer task: the end of every pass. */
static void writer_unlock(void)
{
    xSemaphoreGive(s_lock);
    while (atomic_load(&s_ctrl_waiting)) vTaskDelay(1);
}

static void audio_out_task(void *arg)
{
    (void)arg;

#if AUDIO_TEST_TONE_HZ
    /* Never returns: with the tone on, nothing else reaches the pins. */
    static uint8_t tone_buf[AUDIO_CHUNK_BYTES];
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!(atomic_load(&s_gate) & 1u)) {
            size_t n = audio_tone_fill(tone_buf, sizeof(tone_buf));
            s_writer_gate = atomic_load(&s_gate);
            s_writer_epoch = atomic_load(&s_play_epoch);
            i2s_write_locked(tone_buf, n, false);
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(1);
    }
#endif

    static const uint8_t silence[AUDIO_CHUNK_BYTES];
    bool rebuffer = true;
    int64_t reserve_since_us = 0;
    uint32_t writer_gate = atomic_load(&s_gate) & ~1u;
    uint32_t writer_epoch = atomic_load(&s_play_epoch);
    int64_t last_service = 0;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        if (last_service && s_streaming && !(atomic_load(&s_gate) & 1u))
            counter_max(&s_writer_gap_us, now - last_service);
        last_service = now;
        uint32_t gate = atomic_load(&s_gate);
        uint32_t epoch = atomic_load(&s_play_epoch);
        s_writer_gate = gate;
        s_writer_epoch = epoch;
        if ((gate & 1u) || !s_streaming || writer_gate != (gate & ~1u) || writer_epoch != epoch)
            audio_record_rebuffer(gate, epoch, 0, AUDIO_REBUFFER_CANCELLED);
        if (writer_gate != (gate & ~1u) || writer_epoch != epoch) {
            /* Never invert the control task's lock order by waiting here. */
            if (xSemaphoreTake(s_producer_lock, 0) != pdTRUE) {
                if (s_running) i2s_write_silence_locked(silence, sizeof silence);
                writer_unlock();
                continue;
            }
            flush_ring();
            writer_gate = gate & ~1u;
            writer_epoch = epoch;
            rebuffer = true;
            xSemaphoreGive(s_producer_lock);
        }
        if (s_tx_recover) {
            esp_err_t err = i2s_channel_disable(s_tx);
            s_running = false;
            if (err == ESP_OK) err = clear_dma();
            if (err == ESP_OK) err = i2s_channel_enable(s_tx);
            s_running = (err == ESP_OK);
            s_tx_recover = false;
            if (err != ESP_OK) { s_fault = true; audio_out_gate(); }
        }
        uint32_t fill_now = AUDIO_RING_BYTES - xRingbufferGetCurFreeSize(s_ring);
        size_t prefill = audio_prefill_bytes(s_rate_hz, (s_slot_bits / 8u) * 2u, AUDIO_RING_BYTES);
        bool go = fill_now >= prefill;
        if (s_slave) {
            /* The ring must hold the old threshold, and on top of it the reserve (ring plus
             * queue) must reach half its target -- or 250 ms must pass. The ring condition
             * is not optional: queued packets are audio only if the decoder keeps up, and
             * when it did not, a full queue restarted playback on an empty ring forty times
             * a second (30.09.2026). */
            const bool reserve = slave_buffered(fill_now) >= slave_target() / 2u;
            if (!go) reserve_since_us = 0;
            else if (reserve_since_us == 0) reserve_since_us = now;
            go = go && (reserve || now - reserve_since_us >= 250000);
        }
        if (rebuffer && go) {
            audio_record_rebuffer(gate, epoch, fill_now, AUDIO_REBUFFER_READY);
            rebuffer = false;
            reserve_since_us = 0;
        }
        s_buffering = rebuffer;
        audio_servo_tick();
        size_t len = 0;
        uint8_t *item = NULL;
        if (!(atomic_load(&s_gate) & 1u) && s_streaming && s_running && !rebuffer) {
            item = xRingbufferReceiveUpTo(s_ring, &len, 0, AUDIO_CHUNK_BYTES);
            if (item == NULL) {
                audio_refill_result_t refill = audio_wait_refill(gate, epoch);
                if (refill.status == AUDIO_REFILL_CANCELLED) {
                    /* The helper already returned any item it borrowed. Go
                     * through generation/stream handling before touching PCM. */
                    writer_unlock();
                    continue;
                }
                item = refill.item;
                len = refill.len;
            }
        }
        if (item == NULL) {
            if (s_streaming && !(atomic_load(&s_gate) & 1u)) {
                audio_record_rebuffer(gate, epoch, fill_now,
                                      rebuffer ? AUDIO_REBUFFER_STARTUP : AUDIO_REBUFFER_EMPTY);
                if (!rebuffer) {
                    atomic_fetch_add(&s_rebuffers, 1);
                    /* What the converter still holds is from before the gap. */
                    audio_asrc_reset(&s_asrc);
                }
                rebuffer = true;
                portENTER_CRITICAL(&s_window_lock);
                s_ring_dry++;
                s_fill_min = 0;
                s_fill_n++;
                portEXIT_CRITICAL(&s_window_lock);
                audio_servo_sample(0);
            }
            /* Keep servicing DMA at its own pace; no sleep/refeed loop and no
             * clock stop during a routine underrun or FORMAT_READY wait. */
            if (s_running) i2s_write_silence_locked(silence, sizeof silence);
            writer_unlock();
            if (!s_running) vTaskDelay(1); /* driver fault, not normal refill */
            continue;
        }

        /* Peak is measured in audio_out_write(), where the sample width is known.
         * By here the ring holds wire format and this task just moves bytes.
         *
         * Whole frames only. The ring is a byte buffer and hands back whatever is
         * contiguous, which need not end on a frame boundary; writing a partial
         * frame shifts every sample after it by a byte or two and the channels
         * never line up again. At 16-bit slots that is a swapped pair, audible but
         * self-correcting. At 32-bit slots it is noise that stays. The leftover
         * goes back to the front of the next write rather than to the bin. */
        const size_t frame = (size_t)(s_slot_bits / 8) * 2u;
        size_t off = 0;

        /* Finish the frame left over from last time, if there is one. */
        if (s_carry_len != 0) {
            size_t need = frame - s_carry_len;
            if (len >= need) {
                memcpy(s_carry + s_carry_len, item, need);
                pcm_write_locked(s_carry, frame);
                s_carry_len = 0;
                off = need;
            } else {
                memcpy(s_carry + s_carry_len, item, len);
                s_carry_len += len;
                off = len;
            }
        }

        size_t avail = len - off;
        size_t whole = avail - (avail % frame);
        size_t rem   = avail - whole;
        if (rem != 0) {
            /* Keep the tail for the next pass. Dropping it would shift every
             * sample after it, which is the whole point of this exercise. */
            memcpy(s_carry, (const uint8_t *)item + off + whole, rem);
            s_carry_len = rem;
            s_partial_frames++;
        }
        len = whole;

        /* Sampled after the read, so it is what was left behind. */
        size_t free_bytes = xRingbufferGetCurFreeSize(s_ring);
        uint32_t fill = (uint32_t)(AUDIO_RING_BYTES - free_bytes);
        portENTER_CRITICAL(&s_window_lock);
        if (fill < s_fill_min) { s_fill_min = fill; }
        if (fill > s_fill_max) { s_fill_max = fill; }
        s_fill_sum += fill;
        s_fill_n++;
        portEXIT_CRITICAL(&s_window_lock);
        audio_servo_sample(s_slave ? slave_buffered(fill) : fill);

        if (len != 0) {
            const uint8_t *wire = (const uint8_t *)item + off;
            uint32_t wpeak = audio_wire_peak(wire, len);
            portENTER_CRITICAL(&s_peak_lock);
            if (wpeak >= s_wire_peak) {
                s_wire_peak = wpeak;
                memcpy(s_wire_head, wire, (len < sizeof(s_wire_head)) ? len : sizeof(s_wire_head));
            }
            portEXIT_CRITICAL(&s_peak_lock);
            pcm_write_locked(wire, len);
            /* The DMA now holds something this stream put there, so an overflow
             * of its descriptor queue from here on is a real starve and not the
             * empty pool every stream begins with. */
            s_dma_primed = true;
        }
        vRingbufferReturnItem(s_ring, item);
        writer_unlock();
    }
}

/*
 * Protocol 1.9: the channel in either role.
 *
 * MASTER (hosts before 1.9): this device makes BCLK and WS from the APLL, and the clock
 * servo absorbs the phone's drift by tuning that APLL (DEVICE_SPEC, "Вихід I2S").
 *
 * SLAVE (1.9 hosts): the host makes BCLK and WS from the PLL that also clocks its DAC and
 * reads DATA on its own clock -- measured on 30.09.2026, the host as a SLAVE received
 * ~95 856 of the 96 000 frames a second these pins carried. The slave samples the host's
 * bit clock with an internal clock of bclk x bclk_div, which the 160 MHz PLL must exceed
 * by 1.99x (i2s_std.c); IDF's floor is 8, and the most that fits gives the finest look at
 * the host's clock and the least lag on DOUT ("increase this field when data sent by slave
 * lag behind", IDF i2s_std.h). Always 32-bit slots: that is what a 1.9 host clocks.
 */
static i2s_std_clk_config_t slave_clock(uint32_t rate)
{
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.clk_src = I2S_CLK_SRC_DEFAULT;                  /* the 160 MHz PLL on this chip */
    const uint64_t bclk = (uint64_t)rate * 64u;         /* 32-bit stereo */
    uint32_t div = (uint32_t)((160000000ull * 100u - 1u) / (bclk * 199u));
    if (div < 8u) div = 8u;
    if (div > 63u) div = 63u;                           /* the divider register's reach */
    clk.bclk_div = div;
    return clk;
}

static i2s_std_clk_config_t master_clock(uint32_t rate)
{
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    /* APLL, not the 160 MHz PLL: 44.1 kHz lands far closer, and the clock servo
     * absorbs the phone's drift by tuning it (DEVICE_SPEC, "Вихід I2S"). */
    clk.clk_src = I2S_CLK_SRC_APLL;
    return clk;
}

/* A slave drives DATA and nothing else. i2s_del_channel only forgets its reservation of
 * the pads (i2s_output_gpio_revoke): after a master they stay outputs on that master's
 * clocks, and a slave's init enables their inputs without taking the outputs away. */
static void release_clock_pads(void)
{
    gpio_set_direction(BOARD_I2S_BCLK_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(BOARD_I2S_WS_GPIO, GPIO_MODE_INPUT);
}

/* Pad drive, measured on 30.09.2026 with the host clocking this link and this device's
 * pulse counter on WS: this cable is loaded hard enough that drive 0-1 moves no line at
 * all, and at the default 2 the bit clock couples into its neighbour for ~10 false edges
 * a frame; at 3 the neighbour arrives clean. So every line that runs beside the bit clock
 * and is ours to drive goes out at 3: DATA always, WS too when we are the master. The bit
 * clock itself stays at 2, the least that arrives, so it couples least into the others. */
static void set_pad_drive(bool slave)
{
    gpio_set_drive_capability(BOARD_I2S_DOUT_GPIO, GPIO_DRIVE_CAP_3);
    if (!slave) {
        gpio_set_drive_capability(BOARD_I2S_WS_GPIO, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability(BOARD_I2S_BCLK_GPIO, GPIO_DRIVE_CAP_2);
    }
}

/* With both locks held, or before the writer exists. On failure s_tx is NULL. */
static esp_err_t i2s_open(bool slave)
{
    if (slave) release_clock_pads();
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                            slave ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER);
    /* Send zeros rather than the last buffer again when the producer falls behind:
     * a repeated buffer is an audible buzz, silence is not. */
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) { s_tx = NULL; return err; }

    /* Put back if this fails: the width is what the host was told, and no channel came up
     * to carry another. */
    const uint8_t bits_before = s_slot_bits;
    if (slave) s_slot_bits = 32;
    /* A variable, not an expression: the IDF macro pastes its argument unparenthesised. */
    const i2s_data_bit_width_t width = s_slot_bits == 32 ? I2S_DATA_BIT_WIDTH_32BIT
                                                         : I2S_DATA_BIT_WIDTH_16BIT;
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_DEFAULT),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,        /* the host does not get an MCLK and does not need one */
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws   = BOARD_I2S_WS_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg = slave ? slave_clock(s_rate_hz) : master_clock(s_rate_hz);

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) set_pad_drive(slave);
    /* Before enable: the driver refuses callbacks on a running channel. They live
     * in the handle and survive the disable/reconfig/enable that a format change
     * does, so one registration per channel is enough. */
    const i2s_event_callbacks_t cbs = { .on_send_q_ovf = audio_dma_starved, .on_sent = audio_dma_sent };
    if (err == ESP_OK) err = i2s_channel_register_event_callback(s_tx, &cbs, NULL);
    if (err == ESP_OK) err = clear_dma();
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = NULL;
        s_running = false;
        s_slot_bits = bits_before;
        return err;
    }
    s_slave = slave;
    s_running = true;
    return ESP_OK;
}

esp_err_t audio_out_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_producer_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_producer_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_ring = xRingbufferCreate(AUDIO_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (s_ring == NULL) {
        ESP_LOGE(TAG, "no memory for the %u byte ring", (unsigned)AUDIO_RING_BYTES);
        return ESP_ERR_NO_MEM;
    }

    /* A slave until a host says otherwise: a slave drives only DATA, so whatever the
     * host turns out to be, nothing on the wire has two drivers (protocol 1.9). */
    ESP_RETURN_ON_ERROR(i2s_open(true), TAG, "i2s_open");

#if AUDIO_TEST_TONE_HZ
    audio_tone_init();
#endif

    /* Core 0, away from the decoder. The A2DP decode task is pinned to core 1 and decodes
     * both LHDC channels there (LHDC_DEC_DUAL_CORE is 0); the writer, unpinned, settled on
     * core 1 too once the converter made it a float user. With a heavier LHDC stream on
     * 30.09.2026 core 1 ran at 100 % -- the task watchdog named A2DP_DECODER and audio_out
     * on CPU 1 while CPU 0 sat in its idle hook -- the decoder fell behind, the queue filled
     * and dropped, and the ring ran dry forty times a second. */
    if (xTaskCreatePinnedToCore(audio_out_task, "audio_out", AUDIO_TASK_STACK, NULL,
                                AUDIO_TASK_PRIO, &s_writer_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(audio_report_task, "audio_stats", 3072, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "%s out: BCLK %d, WS %d, DOUT %d, %" PRIu32 " Hz, %d-bit stereo",
             s_slave ? "slave (the host clocks)" : "master",
             BOARD_I2S_BCLK_GPIO, BOARD_I2S_WS_GPIO, BOARD_I2S_DOUT_GPIO,
             s_rate_hz, (int)s_slot_bits);
#if AUDIO_TEST_TONE_HZ
    /* At ERROR because this build plays no music at all, and discovering that by
     * ear an hour later is an hour nobody gets back. */
    ESP_LOGE(TAG, "TEST TONE BUILD: %d Hz at %d dBFS, Bluetooth audio goes nowhere",
             AUDIO_TEST_TONE_HZ, AUDIO_TEST_TONE_DBFS);
#endif
    return ESP_OK;
}

static bool pcm_send(void *ctx, const uint8_t *data, size_t len, size_t *sent)
{
    (void)ctx;
    size_t chunk = len > AUDIO_CHUNK_BYTES ? AUDIO_CHUNK_BYTES : len;
    int64_t start = esp_timer_get_time();
    TickType_t slice = pdMS_TO_TICKS(AUDIO_WRITE_TIMEOUT_MS);
    bool ok = xRingbufferSend(s_ring, data, chunk, slice ? slice : 1) == pdTRUE;
    if (ok) {
        uint32_t now = (uint32_t)esp_timer_get_time();
        portENTER_CRITICAL(&s_event_lock);
        s_events.pcm_last_us = now;
        portEXIT_CRITICAL(&s_event_lock);
    }
    s_packet_wait_us += (uint32_t)(esp_timer_get_time() - start);
    *sent = ok ? chunk : 0;
    if (!ok) atomic_fetch_add(&s_backpressure, 1);
    return true;
}
static void audio_ring_put(const void *data, size_t len)
{
    transfer_context_t ctx = { s_packet_gate, s_packet_epoch, false };
    audio_transfer_ops_t ops = { &ctx, transfer_current, transfer_now, pcm_send };
    size_t accepted;
    audio_transfer_result_t result = audio_transfer(&ops, data, len, s_packet_start_ms,
                                                    AUDIO_TRANSFER_BUDGET_MS, &accepted);
    unsigned frame = (s_slot_bits / 8u) * 2u;
    if (result == AUDIO_TRANSFER_CANCELLED) {
        atomic_fetch_add(&s_pcm_cancelled_frames, (len - accepted) / frame);
    } else if (result != AUDIO_TRANSFER_OK) {
        atomic_store(&s_pcm_drop_us, (uint32_t)esp_timer_get_time());
        s_dropped += len - accepted;
        atomic_fetch_add(&s_dropped_frames, (len - accepted) / frame);
    }
}

static void audio_write_packet(const uint8_t *pcm, size_t len)
{
    if (s_ring == NULL || pcm == NULL || len == 0) {
        return;
    }

    /* Peak is measured here, on what the decoder produced, and reported on a
     * 0..32768 scale whatever the width -- so that the number in the log means
     * the same thing before and after anyone changes the wire. */
    uint32_t peak = 0;
    if (s_src_bits == 32) {
        const int32_t *s = (const int32_t *)(const void *)pcm;
        for (size_t i = 0; i < len / sizeof(int32_t); i++) {
            int32_t v = s[i];
            uint32_t mag = (uint32_t)(v < 0 ? -(int64_t)v : (int64_t)v);
            mag >>= 16;
            if (mag > peak) { peak = mag; }
        }
    } else {
        const int16_t *s = (const int16_t *)(const void *)pcm;
        for (size_t i = 0; i < len / sizeof(int16_t); i++) {
            int32_t v = s[i];
            uint32_t mag = (uint32_t)(v < 0 ? -v : v);
            if (mag > peak) { peak = mag; }
        }
    }
    /* Merge rather than overwrite, under the lock, so a concurrent reset is
     * either before this or after it and never lost inside it. */
    portENTER_CRITICAL(&s_peak_lock);
    if (peak > s_peak) { s_peak = peak; }
    portEXIT_CRITICAL(&s_peak_lock);

    if (s_src_bits == s_slot_bits) {
        audio_ring_put(pcm, len);           /* the common case: nothing to do */
        return;
    }

    if (s_src_bits == 32 && s_slot_bits == 16) {
        const int32_t *src = (const int32_t *)(const void *)pcm;
        size_t n = len / sizeof(int32_t);
        int16_t conv[AUDIO_CONV_BYTES / sizeof(int16_t)];
        const size_t batch = (sizeof(conv) / sizeof(conv[0])) & ~(size_t)1;
        for (size_t i = 0; i < n; i += batch) {
            size_t take = (n - i < batch) ? (n - i) : batch;
            for (size_t j = 0; j < take; j++) {
                conv[j] = audio_narrow_32_to_16(src[i + j]);
            }
            audio_ring_put(conv, take * sizeof(int16_t));
        }
        return;
    }

    if (s_src_bits == 16 && s_slot_bits == 32) {
        const int16_t *src = (const int16_t *)(const void *)pcm;
        size_t n = len / sizeof(int16_t);
        int32_t conv[AUDIO_CONV_BYTES / sizeof(int32_t)];
        /* Even, so a batch is always a whole number of stereo frames: an odd one
         * would hand the writer half a frame and shift everything after it. */
        const size_t batch = (sizeof(conv) / sizeof(conv[0])) & ~(size_t)1;
        for (size_t i = 0; i < n; i += batch) {
            size_t take = (n - i < batch) ? (n - i) : batch;
            for (size_t j = 0; j < take; j++) {
                conv[j] = (int32_t)((uint32_t)(uint16_t)src[i + j] << 16);    /* left aligned, low bits zero */
            }
            audio_ring_put(conv, take * sizeof(int32_t));
        }
        return;
    }

    /* Unreachable: both widths are set by this file and only ever 16 or 32. */
    ESP_LOGE(TAG, "no conversion from %u-bit samples to %u-bit slots",
             s_src_bits, s_slot_bits);
}

/* Nonblocking callback-side gate. A token names this exact closed interval. */
uint32_t audio_out_gate(void)
{
    unsigned old = atomic_load(&s_gate), next;
    do { next = (old + 2u) | 1u; }
    while (!atomic_compare_exchange_weak(&s_gate, &old, next));
    return next;
}

void audio_out_write(const uint8_t *pcm, size_t len)
{
    /* Only the decoder worker may backpressure. Stack/control callbacks never
     * wait here. Packet hooks are in patch 0008, not an application callback. */
    if (!s_producer_lock || xTaskGetCurrentTaskHandle() != s_decoder_task) return;
    if (s_packet_src_frame) s_packet_frames += len / s_packet_src_frame;
    transfer_context_t ctx = { s_packet_gate, s_packet_epoch, false };
    if (!transfer_current(&ctx) || xSemaphoreTake(s_producer_lock, 0) != pdTRUE) {
        atomic_fetch_add(&s_pcm_cancelled_frames, len / s_packet_src_frame);
        return;
    }
    if (transfer_current(&ctx)) audio_write_packet(pcm, len);
    else atomic_fetch_add(&s_pcm_cancelled_frames, len / s_packet_src_frame);
    xSemaphoreGive(s_producer_lock);
}

/* IDF 5.5.5 preload visits every descriptor while READY. Disabling resets its
 * cursor; clearing only the application ring would leave old audio in DMA. */
static esp_err_t clear_dma(void)
{
    static const uint8_t zeros[1024];
    size_t loaded;
    do {
        esp_err_t err = i2s_channel_preload_data(s_tx, zeros, sizeof zeros, &loaded);
        if (err != ESP_OK) return err;
    } while (loaded == sizeof zeros);
    return ESP_OK;
}

esp_err_t audio_out_begin_format(uint32_t *token)
{
    *token = audio_out_gate();
    if (s_fault) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_producer_lock, portMAX_DELAY); // finish any old decoder packet
    ctrl_take_lock();                               // finish and return any old ring item
    flush_ring();
    esp_err_t err = ESP_OK;
    if (s_running) {
        err = i2s_channel_disable(s_tx);
        if (err == ESP_OK) s_running = false;
    }
    if (err == ESP_OK) err = clear_dma();
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx); // old-format clocks, zeros only
        s_running = (err == ESP_OK);
    }
    if (err != ESP_OK) s_fault = true;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_producer_lock);
    return err;
}

esp_err_t audio_out_configure(uint32_t rate, uint8_t bits, uint8_t source_bits)
{
    if (!rate || (bits != 16 && bits != 32) || (source_bits != 16 && source_bits != 32))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_producer_lock, portMAX_DELAY);
    ctrl_take_lock();
    /* A 1.9 host clocks the link in 32-bit slots only. Read under the locks, since
     * audio_out_set_role runs on the link task. */
    if (s_slave) bits = 32;
    esp_err_t err = (atomic_load(&s_gate) & 1u) ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && s_running) {
        err = i2s_channel_disable(s_tx);
        if (err == ESP_OK) s_running = false;
    }
    if (err == ESP_OK && bits != s_slot_bits) {
        i2s_data_bit_width_t width = bits == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;
        i2s_std_slot_config_t slots = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO);
        err = i2s_channel_reconfig_std_slot(s_tx, &slots);
        if (err == ESP_OK) s_slot_bits = bits;
    }
    if (err == ESP_OK && rate != s_rate_hz) {
        i2s_std_clk_config_t clock = s_slave ? slave_clock(rate) : master_clock(rate);
        err = i2s_channel_reconfig_std_clock(s_tx, &clock);
        if (err == ESP_OK) s_rate_hz = rate;
    }
    if (err == ESP_OK) audio_servo_reset();   // a new format is a new phone as far as the clock knows
    if (err == ESP_OK) err = clear_dma();
    if (err == ESP_OK) {
        s_src_bits = source_bits;
        err = i2s_channel_enable(s_tx);
        s_running = (err == ESP_OK);
    }
    // On error stay gated and never enable possibly missing DMA descriptors.
    if (err != ESP_OK) s_fault = true;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_producer_lock);
    return err;
}

bool audio_out_resume(uint32_t token)
{
    unsigned expected = token;
    return s_running && atomic_compare_exchange_strong(&s_gate, &expected, token & ~1u);
}

esp_err_t audio_out_set_role(bool slave)
{
    if (s_lock == NULL || s_producer_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (s_tx != NULL && slave == s_slave) return ESP_OK;
    /* PROTOCOL.md 6.1: a role change can change the slot width, and none is played
     * unannounced. Gate first, which also voids the pending resume token: the reopened
     * channel carries zeros until bt_av runs a new format transaction for this role. */
    (void)audio_out_gate();
    xSemaphoreTake(s_producer_lock, portMAX_DELAY); // finish any decoder packet
    ctrl_take_lock();                               // and any writer pass
    flush_ring();
    if (s_tx != NULL) {
        if (s_running) i2s_channel_disable(s_tx);
        s_running = false;
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    esp_err_t err = i2s_open(slave);
    if (err != ESP_OK) {
        /* Neither role came up: stay silent and driving nothing rather than half open. */
        s_fault = true;
        s_open_fault = true;
        ESP_LOGE(TAG, "I2S %s would not open: %s", slave ? "slave" : "master", esp_err_to_name(err));
    } else {
        if (s_open_fault) {
            /* Including what begin_format raised meanwhile against the missing channel. */
            s_fault = false;
            s_open_fault = false;
        }
        audio_servo_reset();    /* the old channel's correction is not this one's */
        ESP_LOGW(TAG, "I2S is now the %s: %" PRIu32 " Hz, %d-bit slots",
                 slave ? "SLAVE (the host clocks the link)" : "MASTER (legacy host)",
                 s_rate_hz, (int)s_slot_bits);
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_producer_lock);
    return err;
}

bool audio_out_is_slave(void)
{
    return s_tx != NULL && s_slave;
}

uint8_t audio_out_slot_bits(void)
{
    return s_slot_bits;
}

uint32_t audio_out_rate_hz(void)
{
    return s_rate_hz;
}

uint32_t audio_out_dropped_bytes(void)
{
    return s_dropped;
}

static void flush_ring(void)
{
    if (s_ring == NULL) {
        return;
    }
    size_t len = 0;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_ring, &len, 0, AUDIO_CHUNK_BYTES)) != NULL) {
        atomic_fetch_add(&s_pcm_cancelled_frames, len / ((s_slot_bits / 8u) * 2u));
        vRingbufferReturnItem(s_ring, item);
    }
    /* And the half frame held back from the old format, which belongs to it, and what the
     * converter still holds. */
    s_carry_len = 0;
    audio_asrc_reset(&s_asrc);
}

void audio_out_set_codec_name(const char *name)
{
    s_codec_name = (name != NULL) ? name : "?";
}

void audio_out_set_streaming(bool streaming)
{
    /* Cleared on both edges, and before the flag the ISR gates on: a stream begins
     * with a DMA pool nobody has filled yet, and counting that as starvation is
     * how a counter starts crying wolf. The writer sets it again on its first
     * write, which is the moment the DMA first holds this stream's audio. */
    s_dma_primed = false;
    atomic_fetch_add(&s_play_epoch, 1u); /* preserve correlated FORMAT_READY token */
    s_streaming  = streaming;
}

bool audio_out_get_stats(audio_out_stats_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return false;
    }
    out->dropped_bytes = s_dropped;
    out->dma_starved   = s_dma_starved;
    /* The window counters restart; dma_starved does not, because zero is the only
     * good value it has and a running total is what anyone would check. */
    portENTER_CRITICAL(&s_window_lock);
    out->ring_dry      = s_ring_dry;
    out->fill_min      = (s_fill_min == UINT32_MAX) ? 0 : s_fill_min;
    out->fill_max      = s_fill_max;
    const uint32_t n   = s_fill_n;
    out->fill_avg      = n ? (uint32_t)(s_fill_sum / n) : 0;
    s_ring_dry = 0;
    s_fill_min = UINT32_MAX;
    s_fill_max = 0;
    s_fill_sum = 0;
    s_fill_n   = 0;
    portEXIT_CRITICAL(&s_window_lock);
    portENTER_CRITICAL(&s_peak_lock);
    out->peak      = s_peak;
    out->wire_peak = s_wire_peak;
    memcpy(out->wire_head, s_wire_head, sizeof(out->wire_head));
    s_peak      = 0;
    s_wire_peak = 0;
    portEXIT_CRITICAL(&s_peak_lock);
    return true;
}
