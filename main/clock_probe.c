/*
 * Measures the I2S bit clock on this board, with nothing attached to it.
 *
 * The host asked a question that matters to its receiver: does BCLK keep running
 * when there is no stream, or does it stop? Reading the driver code gives an
 * answer; counting the edges gives the answer. The pulse counter can be fed from
 * the pin's own output path, so this costs no wire and no second instrument.
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
#define PROBE_WINDOW_MS    200
#define PROBE_PERIOD_MS    5000

static pcnt_unit_handle_t s_unit;

static void probe_task(void *arg)
{
    (void)arg;

    for (;;) {
        pcnt_unit_clear_count(s_unit);
        int64_t t0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(PROBE_WINDOW_MS));
        int count = 0;
        pcnt_unit_get_count(s_unit, &count);
        int64_t elapsed_us = esp_timer_get_time() - t0;

        uint32_t hz = (elapsed_us > 0)
            ? (uint32_t)(((int64_t)count * 1000000) / elapsed_us)
            : 0;

        if (hz == 0) {
            ESP_LOGW(TAG, "BCLK on GPIO%d is stopped", BOARD_I2S_BCLK_GPIO);
        } else {
            /* Two channels of sixteen bits is thirty-two bit clocks per frame, so
             * the frame rate falls straight out of the edge count. */
            ESP_LOGI(TAG, "BCLK %" PRIu32 " Hz on GPIO%d, i.e. %" PRIu32 " frames/s",
                     hz, BOARD_I2S_BCLK_GPIO, hz / 32);
        }

        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS - PROBE_WINDOW_MS));
    }
}

esp_err_t clock_probe_start(void)
{
    pcnt_unit_config_t unit_cfg = {
        .high_limit = PROBE_HIGH_LIMIT,
        .low_limit  = -1,                 /* the driver requires one below zero */
        .flags.accum_count = 1,           /* keep counting past the 16-bit wrap */
    };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &s_unit), TAG, "pcnt_new_unit");

    /* accum_count only accumulates at values the unit is watching. */
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(s_unit, PROBE_HIGH_LIMIT), TAG, "watch high");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(s_unit, -1), TAG, "watch low");

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num  = BOARD_I2S_BCLK_GPIO,
        .level_gpio_num = -1,             /* nothing gates the count */
        .flags.io_loop_back = 1,          /* read the pin's own output back in */
    };
    pcnt_channel_handle_t chan = NULL;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_unit, &chan_cfg, &chan), TAG, "pcnt_new_channel");

    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(chan,
                            PCNT_CHANNEL_EDGE_ACTION_INCREASE,   /* rising */
                            PCNT_CHANNEL_EDGE_ACTION_HOLD),      /* falling: ignore */
                        TAG, "edge action");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(chan,
                            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                            PCNT_CHANNEL_LEVEL_ACTION_KEEP),
                        TAG, "level action");

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_unit), TAG, "enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_unit), TAG, "clear");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(s_unit), TAG, "start");

    ESP_RETURN_ON_FALSE(
        xTaskCreate(probe_task, "clk_probe", 2560, NULL, 2, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "no memory for the probe task");

    ESP_LOGW(TAG, "clock probe running on GPIO%d; this is a diagnostic build",
             BOARD_I2S_BCLK_GPIO);
    return ESP_OK;
}

#else  /* CONFIG_SAT_CLOCK_PROBE */

esp_err_t clock_probe_start(void)
{
    return ESP_OK;
}

#endif
