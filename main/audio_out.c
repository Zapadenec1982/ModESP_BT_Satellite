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
#define AUDIO_BITS              I2S_DATA_BIT_WIDTH_16BIT

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

        /* The level we are actually handing over, before anything downstream sees
         * it. Sixteen-bit two's complement, so full scale is 32768. */
        const int16_t *samples = (const int16_t *)(const void *)item;
        size_t n_samples = len / sizeof(int16_t);
        uint32_t peak = s_peak;
        for (size_t i = 0; i < n_samples; i++) {
            int32_t v = samples[i];
            uint32_t mag = (uint32_t)(v < 0 ? -v : v);
            if (mag > peak) { peak = mag; }
        }
        s_peak = peak;

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
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(AUDIO_BITS, I2S_SLOT_MODE_STEREO),
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
             s_rate_hz, (int)AUDIO_BITS);
    return ESP_OK;
}

void audio_out_write(const uint8_t *pcm, size_t len)
{
    if (s_ring == NULL || pcm == NULL || len == 0) {
        return;
    }

    /* Zero timeout on purpose. This runs in the Bluetooth stack's task, and rule 1
     * of the spec is that the device never blocks. */
    if (xRingbufferSend(s_ring, pcm, len, 0) != pdTRUE) {
        uint32_t before = s_dropped;
        s_dropped += len;
        /* One line per kilobyte lost, not one per packet. */
        if ((before >> 10) != (s_dropped >> 10)) {
            ESP_LOGW(TAG, "ring full, dropped %" PRIu32 " bytes so far", s_dropped);
        }
    }
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

uint32_t audio_out_dropped_bytes(void)
{
    return s_dropped;
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
