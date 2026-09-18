/*
 * Counts edges on this board's own output pins, with nothing attached to them.
 *
 * Two questions were asked of this firmware that a datasheet cannot answer. Does
 * BCLK keep running when there is no stream? And is the UART transmitter actually
 * moving the pad, or only the peripheral? The pulse counter can be fed from a
 * pin's own output path, so both cost no wire and no second instrument.
 *
 * Off by default. It is a diagnostic, not a feature.
 */

#include "clock_probe.h"

#include <inttypes.h>

#include "board.h"

#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_SAT_CLOCK_PROBE

static const char *TAG = "sat.clk";

/* The counter is 16-bit; accum_count carries the overflows so the total is still
 * readable. A window of 200 ms at 1.4 MHz is a few hundred thousand edges, which
 * is plenty of resolution and still quick. */
#define PROBE_HIGH_LIMIT   30000

/* Two seconds, because the UART is the slow one here: it sends a PING once a
 * second, so a shorter window could land between two and report a dead pin that
 * is merely idle. */
#define PROBE_WINDOW_MS    2000
#define PROBE_PERIOD_MS    6000

/* Four pins, two of them controls. WS must read exactly the frame rate and BCLK
 * thirty-two times that; if either is wrong the method is wrong and nothing else
 * measured here means anything. */
typedef struct {
    const char        *what;
    int                gpio;
    pcnt_unit_handle_t unit;
} probe_t;

static probe_t s_probes[] = {
    { "BCLK    ", BOARD_I2S_BCLK_GPIO, NULL },
    { "WS      ", BOARD_I2S_WS_GPIO,   NULL },
    { "UART TX ", BOARD_LINK_TX_GPIO,  NULL },
    { "UART RX ", BOARD_LINK_RX_GPIO,  NULL },
};

#define PROBE_COUNT (sizeof(s_probes) / sizeof(s_probes[0]))

static esp_err_t probe_pin(int gpio, pcnt_unit_handle_t *out_unit)
{
    pcnt_unit_config_t unit_cfg = {
        .high_limit = PROBE_HIGH_LIMIT,
        .low_limit  = -1,                 /* the driver requires one below zero */
        .flags.accum_count = 1,           /* keep counting past the 16-bit wrap */
    };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, out_unit), TAG, "pcnt_new_unit");

    /* accum_count only accumulates at values the unit is watching. */
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(*out_unit, PROBE_HIGH_LIMIT), TAG, "watch high");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(*out_unit, -1), TAG, "watch low");

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num  = gpio,
        .level_gpio_num = -1,             /* nothing gates the count */
        .flags.io_loop_back = 1,          /* read the pin's own output back in */
    };
    pcnt_channel_handle_t chan = NULL;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(*out_unit, &chan_cfg, &chan), TAG, "pcnt_new_channel");

    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(chan,
                            PCNT_CHANNEL_EDGE_ACTION_INCREASE,   /* rising */
                            PCNT_CHANNEL_EDGE_ACTION_HOLD),      /* falling: ignore */
                        TAG, "edge action");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(chan,
                            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                            PCNT_CHANNEL_LEVEL_ACTION_KEEP),
                        TAG, "level action");

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(*out_unit), TAG, "enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(*out_unit), TAG, "clear");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(*out_unit), TAG, "start");
    return ESP_OK;
}

static void probe_task(void *arg)
{
    (void)arg;

    for (;;) {
        for (size_t i = 0; i < PROBE_COUNT; i++) {
            pcnt_unit_clear_count(s_probes[i].unit);
        }
        int64_t t0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(PROBE_WINDOW_MS));

        int counts[PROBE_COUNT];
        for (size_t i = 0; i < PROBE_COUNT; i++) {
            counts[i] = 0;
            pcnt_unit_get_count(s_probes[i].unit, &counts[i]);
        }
        int64_t elapsed_us = esp_timer_get_time() - t0;

        for (size_t i = 0; i < PROBE_COUNT; i++) {
            uint32_t hz = (elapsed_us > 0)
                ? (uint32_t)(((int64_t)counts[i] * 1000000) / elapsed_us)
                : 0;
            ESP_LOGI(TAG, "%s GPIO%-2d : %8d edges in %d ms = %8" PRIu32 " Hz",
                     s_probes[i].what, s_probes[i].gpio, counts[i], PROBE_WINDOW_MS, hz);
        }

        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS - PROBE_WINDOW_MS));
    }
}

esp_err_t clock_probe_start(void)
{
    for (size_t i = 0; i < PROBE_COUNT; i++) {
        ESP_RETURN_ON_ERROR(probe_pin(s_probes[i].gpio, &s_probes[i].unit),
                            TAG, "probe on GPIO%d", s_probes[i].gpio);
    }

    ESP_RETURN_ON_FALSE(
        xTaskCreate(probe_task, "clk_probe", 3072, NULL, 2, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "no memory for the probe task");

    ESP_LOGW(TAG, "counting edges on four pins; this is a diagnostic build");
    return ESP_OK;
}

#else  /* CONFIG_SAT_CLOCK_PROBE */

esp_err_t clock_probe_start(void)
{
    return ESP_OK;
}

#endif
