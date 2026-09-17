#include "audio_out.h"

#include <inttypes.h>
#include <string.h>

#include "board.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
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

static void audio_out_task(void *arg)
{
    (void)arg;

    for (;;) {
        size_t len = 0;
        uint8_t *item = xRingbufferReceiveUpTo(s_ring, &len, pdMS_TO_TICKS(100), AUDIO_CHUNK_BYTES);
        if (item == NULL) {
            continue;   /* nothing playing; the channel keeps clocking out silence */
        }

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
