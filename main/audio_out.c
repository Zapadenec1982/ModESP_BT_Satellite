#include "audio_out.h"

#include <inttypes.h>
#include <string.h>
#include <stdatomic.h>

#include "board.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
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

/* About 136 ms of 44.1 kHz stereo. Deep enough to ride out a busy moment in the
 * Bluetooth task, shallow enough that latency stays sane. */
#define AUDIO_RING_BYTES        (24 * 1024)
#define AUDIO_CHUNK_BYTES       1024
#define AUDIO_WRITE_TIMEOUT_MS  20

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
static atomic_uint       s_gate = 1; /* bit 0 closed; upper bits invalidate old resumes */
static bool              s_running;
static bool              s_fault;
static void flush_ring(void);
static uint32_t           s_rate_hz = AUDIO_RATE_DEFAULT;
static volatile uint32_t  s_dropped;
static volatile uint32_t  s_underruns;
static volatile uint32_t  s_fill_min = UINT32_MAX;
static volatile uint32_t  s_fill_max;
static volatile bool      s_streaming;   /* an empty ring only matters while playing */
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
#define AUDIO_SERVO_MAX_PPM     200
#define AUDIO_SERVO_KP          10.0f       /* ppm per millisecond of fill error */
#define AUDIO_SERVO_KI          0.025f      /* ppm per millisecond, per second */
#define AUDIO_SERVO_STEP_PPM    2           /* smallest change worth an APLL rewrite */

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
    i2s_tuning_info_t info = {0};
    if (i2s_channel_tune_rate(s_tx, NULL, &info) != ESP_OK) {
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
    const esp_err_t err = i2s_channel_tune_rate(s_tx, &cfg, &info);
    periph_rtc_apll_release();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clock servo: %+d ppm refused: %s; servo off until the next format",
                 ppm, esp_err_to_name(err));
        s_servo_off = true;
        return;
    }
    if (s_servo_ppm == 0 && ppm != 0) {
        ESP_LOGI(TAG, "clock servo engaged: MCLK %" PRId32 " Hz nominal", origin);
    }
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

    if (s_servo_off || !s_running || !s_streaming || (atomic_load(&s_gate) & 1u) || n == 0) {
        return;     /* nothing to track; the integrator keeps what it has learned */
    }

    const unsigned frame_bytes = (unsigned)(s_slot_bits / 8) * 2u;
    const float byte_rate = (float)s_rate_hz * (float)frame_bytes;
    const float fill      = (float)(acc / n);
    /* Positive: the ring is fuller than wanted, the phone is ahead, and the
     * bus has to run faster to catch up. */
    const float err_ms    = (fill - (float)AUDIO_SERVO_TARGET) * 1000.0f / byte_rate;

    float out = AUDIO_SERVO_KP * err_ms + s_servo_integral;
    /* Anti-windup: pinned against the limit in the direction of the error, the
     * integrator would only be storing up an overshoot. */
    const bool pinned = (out >=  AUDIO_SERVO_MAX_PPM && err_ms > 0.0f) ||
                        (out <= -AUDIO_SERVO_MAX_PPM && err_ms < 0.0f);
    if (!pinned) {
        s_servo_integral += AUDIO_SERVO_KI * err_ms;    /* dt is the one-second period */
        if (s_servo_integral >  AUDIO_SERVO_MAX_PPM) { s_servo_integral =  AUDIO_SERVO_MAX_PPM; }
        if (s_servo_integral < -AUDIO_SERVO_MAX_PPM) { s_servo_integral = -AUDIO_SERVO_MAX_PPM; }
        out = AUDIO_SERVO_KP * err_ms + s_servo_integral;
    }
    if (out >  AUDIO_SERVO_MAX_PPM) { out =  AUDIO_SERVO_MAX_PPM; }
    if (out < -AUDIO_SERVO_MAX_PPM) { out = -AUDIO_SERVO_MAX_PPM; }

    const int ppm  = (int)(out >= 0.0f ? out + 0.5f : out - 0.5f);
    const int step = ppm - s_servo_ppm;
    if (step >= AUDIO_SERVO_STEP_PPM || step <= -AUDIO_SERVO_STEP_PPM) {
        audio_servo_apply(ppm);
    }
}

/* Ten seconds, and only while something is playing. A line an hour from an idle
 * device is noise; a line every ten seconds from a stuttering one is the whole
 * diagnosis. */
#define AUDIO_REPORT_US  (10 * 1000 * 1000)

static void audio_report(void)
{
    static int64_t next_us;
    int64_t now = esp_timer_get_time();
    if (!s_streaming || now < next_us) {
        return;
    }
    next_us = now + AUDIO_REPORT_US;

    audio_out_stats_t st;
    audio_out_get_stats(&st);
    /* Peak as a fraction of full scale, in tenths of a percent, so that a glance
     * says whether the signal is loud without anyone reaching for a calculator. */
    unsigned peak_permille = (unsigned)((st.peak * 1000u) / 32768u);

    /* Milliseconds of audio standing in the ring, which is the latency this device
     * is adding and the number that says whether the buffer is healthy or merely
     * full. Bytes alone do not, because the same count means different things at
     * different rates and widths. */
    unsigned frame_bytes = (unsigned)(s_slot_bits / 8) * 2u;
    unsigned fill_ms = (frame_bytes && s_rate_hz)
                     ? (unsigned)((uint64_t)st.fill_max * 1000u / (frame_bytes * s_rate_hz))
                     : 0u;

    ESP_LOGI(TAG, "%s %" PRIu32 " Hz %u-bit slots: ring %u..%u of %u (%u ms), underruns %" PRIu32
                  ", dropped %" PRIu32 ", peak %" PRIu32 "/32768 (%u.%u%%), wire %" PRIu32
                  " [%02x %02x %02x %02x %02x %02x %02x %02x], part %" PRIu32 ", free dma %u"
                  ", clock %+d ppm (%+" PRId32 " Hz)",
             s_codec_name, s_rate_hz, s_slot_bits,
             (unsigned)st.fill_min, (unsigned)st.fill_max, (unsigned)AUDIO_RING_BYTES, fill_ms,
             st.underruns, st.dropped_bytes, st.peak,
             peak_permille / 10, peak_permille % 10, st.wire_peak,
             st.wire_head[0], st.wire_head[1], st.wire_head[2], st.wire_head[3],
             st.wire_head[4], st.wire_head[5], st.wire_head[6], st.wire_head[7],
             s_partial_frames,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             s_servo_ppm, s_servo_delta_hz);
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

static void i2s_write_locked(const void *data, size_t len)
{
    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx, data, len, &written, AUDIO_WRITE_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write failed: %s (%u of %u bytes)", esp_err_to_name(err),
                 (unsigned)written, (unsigned)len);
    }
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
            i2s_write_locked(tone_buf, n);
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(1);
    }
#endif

    for (;;) {
        audio_report();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        audio_servo_tick();
        if (atomic_load(&s_gate) & 1u) {
            xSemaphoreGive(s_lock);
            vTaskDelay(1);
            continue;
        }
        size_t len = 0;
        uint8_t *item = xRingbufferReceiveUpTo(s_ring, &len, 0, AUDIO_CHUNK_BYTES);
        if (item == NULL) {
            /* Nothing to send. Between tracks that is silence and correct; during
             * a stream it is a hole in the music, because the DMA fills it with
             * zeros and says nothing. */
            // Preserve the existing 100 ms starvation diagnostic cadence.
            static TickType_t last_empty;
            const TickType_t now = xTaskGetTickCount();
            if (s_streaming && now - last_empty >= pdMS_TO_TICKS(100)) {
                s_underruns++;
                s_fill_min = 0;
                last_empty = now;
            }
            if (s_streaming) {
                audio_servo_sample(0);
            }
            xSemaphoreGive(s_lock);
            vTaskDelay(1);
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
                i2s_write_locked(s_carry, frame);
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
        if (fill < s_fill_min) { s_fill_min = fill; }
        if (fill > s_fill_max) { s_fill_max = fill; }
        audio_servo_sample(fill);

        if (len != 0) {
            const uint8_t *wire = (const uint8_t *)item + off;
            uint32_t wpeak = audio_wire_peak(wire, len);
            portENTER_CRITICAL(&s_peak_lock);
            if (wpeak >= s_wire_peak) {
                s_wire_peak = wpeak;
                memcpy(s_wire_head, wire, (len < sizeof(s_wire_head)) ? len : sizeof(s_wire_head));
            }
            portEXIT_CRITICAL(&s_peak_lock);
            i2s_write_locked(wire, len);
        }
        vRingbufferReturnItem(s_ring, item);
        xSemaphoreGive(s_lock);
    }
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

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Send zeros rather than the last buffer again when the producer falls behind:
     * a repeated buffer is an audible buzz, silence is not. */
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_DEFAULT),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
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
    /* APLL, not the 160 MHz PLL: 44.1 kHz lands far closer, and the clock servo
     * above absorbs the phone's drift by tuning it (DEVICE_SPEC, "Вихід I2S"). */
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init_std_mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "channel_enable");
    s_running = true;

#if AUDIO_TEST_TONE_HZ
    audio_tone_init();
#endif

    if (xTaskCreate(audio_out_task, "audio_out", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "master out: BCLK %d, WS %d, DOUT %d, %" PRIu32 " Hz, %d-bit stereo",
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

/* Zero timeout on purpose, everywhere below. This runs in the Bluetooth stack's
 * task, and rule 1 of the spec is that the device never blocks. */
static void audio_ring_put(const void *data, size_t len)
{
    if (xRingbufferSend(s_ring, data, len, 0) == pdTRUE) {
        return;
    }
    uint32_t before = s_dropped;
    s_dropped += len;
    /* One line per kilobyte lost, not one per packet. */
    if ((before >> 10) != (s_dropped >> 10)) {
        ESP_LOGW(TAG, "ring full, dropped %" PRIu32 " bytes so far", s_dropped);
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
    if (!s_producer_lock || (atomic_load(&s_gate) & 1u)) return;
    if (xSemaphoreTake(s_producer_lock, 0) != pdTRUE) return;
    if (!(atomic_load(&s_gate) & 1u)) audio_write_packet(pcm, len);
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
    xSemaphoreTake(s_lock, portMAX_DELAY);          // finish and return any old ring item
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
    xSemaphoreTake(s_lock, portMAX_DELAY);
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
        i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        clock.clk_src = I2S_CLK_SRC_APLL;
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
        vRingbufferReturnItem(s_ring, item);
    }
    /* And the half frame held back from the old format, which belongs to it. */
    s_carry_len = 0;
}

void audio_out_set_codec_name(const char *name)
{
    s_codec_name = (name != NULL) ? name : "?";
}

void audio_out_set_streaming(bool streaming)
{
    s_streaming = streaming;
}

void audio_out_get_stats(audio_out_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->dropped_bytes = s_dropped;
    out->underruns     = s_underruns;
    out->fill_min      = (s_fill_min == UINT32_MAX) ? 0 : s_fill_min;
    out->fill_max      = s_fill_max;
    portENTER_CRITICAL(&s_peak_lock);
    out->peak      = s_peak;
    out->wire_peak = s_wire_peak;
    memcpy(out->wire_head, s_wire_head, sizeof(out->wire_head));
    s_peak      = 0;
    s_wire_peak = 0;
    portEXIT_CRITICAL(&s_peak_lock);
    s_fill_min = UINT32_MAX;
    s_fill_max = 0;
}
