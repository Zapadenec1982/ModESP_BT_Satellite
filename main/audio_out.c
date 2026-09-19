#include "audio_out.h"

#include <inttypes.h>
#include <string.h>

#include "board.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
#define AUDIO_WRITE_TIMEOUT_MS  200

#define AUDIO_TASK_STACK        3072
#define AUDIO_TASK_PRIO         (configMAX_PRIORITIES - 3)

static i2s_chan_handle_t  s_tx;
static RingbufHandle_t    s_ring;
static SemaphoreHandle_t  s_lock;       /* guards the channel against retune-while-writing */
static uint32_t           s_rate_hz = AUDIO_RATE_DEFAULT;
static volatile uint32_t  s_dropped;
static volatile uint32_t  s_underruns;
static volatile uint32_t  s_fill_min = UINT32_MAX;
static volatile uint32_t  s_fill_max;
static volatile bool      s_streaming;   /* an empty ring only matters while playing */
static volatile uint32_t  s_peak;
static uint8_t            s_slot_bits = AUDIO_BITS_DEFAULT;   /* on the wire */
static uint8_t            s_src_bits  = 16;                   /* out of the decoder */

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

    ESP_LOGI(TAG, "ring %u..%u of %u bytes, underruns %" PRIu32 ", dropped %" PRIu32
                  ", peak %" PRIu32 "/32768 (%u.%u%% of full scale), free dma %u",
             (unsigned)st.fill_min, (unsigned)st.fill_max, (unsigned)AUDIO_RING_BYTES,
             st.underruns, st.dropped_bytes, st.peak,
             peak_permille / 10, peak_permille % 10,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}

static void audio_out_task(void *arg)
{
    (void)arg;

    for (;;) {
        audio_report();
        size_t len = 0;
        uint8_t *item = xRingbufferReceiveUpTo(s_ring, &len, pdMS_TO_TICKS(100), AUDIO_CHUNK_BYTES);
        if (item == NULL) {
            /* Nothing to send. Between tracks that is silence and correct; during
             * a stream it is a hole in the music, because the DMA fills it with
             * zeros and says nothing. */
            if (s_streaming) {
                s_underruns++;
                s_fill_min = 0;
            }
            continue;
        }

        /* Peak is measured in audio_out_write(), where the sample width is known.
         * By here the ring holds wire format and this task just moves bytes. */

        /* Sampled after the read, so it is what was left behind. */
        size_t free_bytes = xRingbufferGetCurFreeSize(s_ring);
        uint32_t fill = (uint32_t)(AUDIO_RING_BYTES - free_bytes);
        if (fill < s_fill_min) { s_fill_min = fill; }
        if (fill > s_fill_max) { s_fill_max = fill; }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, item, len, &written, pdMS_TO_TICKS(AUDIO_WRITE_TIMEOUT_MS));
        xSemaphoreGive(s_lock);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "write failed: %s (%u of %u bytes)", esp_err_to_name(err),
                     (unsigned)written, (unsigned)len);
        }
        vRingbufferReturnItem(s_ring, item);
    }
}

esp_err_t audio_out_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
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
    /* APLL, not the 160 MHz PLL: 44.1 kHz lands far closer, and absorbing the
     * phone's drift later means tuning this clock (DEVICE_SPEC, "Вихід I2S"). */
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init_std_mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "channel_enable");

    if (xTaskCreate(audio_out_task, "audio_out", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "master out: BCLK %d, WS %d, DOUT %d, %" PRIu32 " Hz, %d-bit stereo",
             BOARD_I2S_BCLK_GPIO, BOARD_I2S_WS_GPIO, BOARD_I2S_DOUT_GPIO,
             s_rate_hz, (int)s_slot_bits);
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

void audio_out_write(const uint8_t *pcm, size_t len)
{
    if (s_ring == NULL || pcm == NULL || len == 0) {
        return;
    }

    /* Peak is measured here, on what the decoder produced, and reported on a
     * 0..32768 scale whatever the width -- so that the number in the log means
     * the same thing before and after anyone changes the wire. */
    uint32_t peak = s_peak;
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
    s_peak = peak;

    if (s_src_bits == s_slot_bits) {
        audio_ring_put(pcm, len);           /* the common case: nothing to do */
        return;
    }

    if (s_src_bits == 32 && s_slot_bits == 16) {
        const int32_t *src = (const int32_t *)(const void *)pcm;
        size_t n = len / sizeof(int32_t);
        int16_t conv[AUDIO_CONV_BYTES / sizeof(int16_t)];
        const size_t batch = sizeof(conv) / sizeof(conv[0]);
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
        const size_t batch = sizeof(conv) / sizeof(conv[0]);
        for (size_t i = 0; i < n; i += batch) {
            size_t take = (n - i < batch) ? (n - i) : batch;
            for (size_t j = 0; j < take; j++) {
                conv[j] = (int32_t)src[i + j] << 16;    /* left aligned, low bits zero */
            }
            audio_ring_put(conv, take * sizeof(int32_t));
        }
        return;
    }

    /* Unreachable: both widths are set by this file and only ever 16 or 32. */
    ESP_LOGE(TAG, "no conversion from %u-bit samples to %u-bit slots",
             s_src_bits, s_slot_bits);
}

esp_err_t audio_out_set_rate(uint32_t rate_hz)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rate_hz == s_rate_hz) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "retune %" PRIu32 " -> %" PRIu32 " Hz", s_rate_hz, rate_hz);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = i2s_channel_disable(s_tx);
    if (err == ESP_OK) {
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate_hz);
        clk_cfg.clk_src = I2S_CLK_SRC_APLL;
        err = i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
        if (err == ESP_OK) {
            s_rate_hz = rate_hz;
        }
        esp_err_t en = i2s_channel_enable(s_tx);
        if (err == ESP_OK) {
            err = en;
        }
    }

    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "retune failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t audio_out_set_slot_bits(uint8_t bits)
{
    if (bits != 16 && bits != 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bits == s_slot_bits) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "slot width %u -> %u bits (BCLK %u per frame)",
             s_slot_bits, bits, bits * 2u);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = i2s_channel_disable(s_tx);
    if (err == ESP_OK) {
        /* Through a variable, not inline: the macro uses its argument several
         * times, one of them in a boolean context, and a ternary there trips
         * -Werror=int-in-bool-context. */
        i2s_data_bit_width_t width = (bits == 32) ? I2S_DATA_BIT_WIDTH_32BIT
                                                  : I2S_DATA_BIT_WIDTH_16BIT;
        i2s_std_slot_config_t slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO);
        err = i2s_channel_reconfig_std_slot(s_tx, &slot_cfg);
        if (err == ESP_OK) {
            s_slot_bits = bits;
        }
        esp_err_t en = i2s_channel_enable(s_tx);
        if (err == ESP_OK) {
            err = en;
        }
    }

    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "slot reconfigure failed: %s", esp_err_to_name(err));
    }
    return err;
}

uint8_t audio_out_slot_bits(void)
{
    return s_slot_bits;
}

void audio_out_set_source_bits(uint8_t bits)
{
    if (bits != 16 && bits != 32) {
        ESP_LOGE(TAG, "decoder claims %u-bit samples, which is not a thing here", bits);
        return;
    }
    s_src_bits = bits;
}

uint32_t audio_out_dropped_bytes(void)
{
    return s_dropped;
}

void audio_out_flush(void)
{
    if (s_ring == NULL) {
        return;
    }
    size_t len = 0;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_ring, &len, 0, AUDIO_CHUNK_BYTES)) != NULL) {
        vRingbufferReturnItem(s_ring, item);
    }
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
    out->peak          = s_peak;
    s_fill_min = UINT32_MAX;
    s_fill_max = 0;
    s_peak = 0;
}
