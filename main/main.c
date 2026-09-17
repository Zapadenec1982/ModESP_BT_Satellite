/*
 * modesp-bt-satellite — a Bluetooth A2DP receiver that hands audio to a host over
 * I2S and everything else over UART. See docs/DEVICE_SPEC.md.
 *
 * Stage 1 is the bare receiver: a phone pairs, SBC plays, and the samples reach the
 * I2S pins. The control link, the port expander and the extra codecs come later.
 */

#include "audio_out.h"
#include "bt_av.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "sat";

/* What the phone shows in its Bluetooth list. */
#define DEVICE_NAME "modesp satellite"

void app_main(void)
{
    /* Bluedroid keeps its pairing keys here, so this comes first. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* The output comes up before the radio: once a phone connects, samples can
     * start arriving immediately and there has to be somewhere to put them. */
    ESP_ERROR_CHECK(audio_out_init());
    ESP_ERROR_CHECK(bt_av_start(DEVICE_NAME));

    ESP_LOGI(TAG, "up");
}
