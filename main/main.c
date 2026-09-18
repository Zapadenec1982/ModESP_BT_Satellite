/*
 * modesp-bt-satellite — a Bluetooth A2DP receiver that hands audio to a host over
 * I2S and everything else over UART. See docs/DEVICE_SPEC.md and docs/PROTOCOL.md.
 *
 * Order matters here. The output comes up first because samples can start
 * arriving the moment a phone connects; the link comes up before the radio so
 * that the first BT_STATE has somewhere to go; the expander comes up last
 * because its GPIO_MAP must follow the HELLO.
 */

#include <string.h>

#include "audio_out.h"
#include "bt_av.h"
#include "expander.h"
#include "link.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "satlink/proto.h"

static const char *TAG = "sat";

/* What the phone shows in its Bluetooth list. */
#define DEVICE_NAME "modesp satellite"

static void on_hello(const satlink_hello_t *hello)
{
    (void)hello;
    /* A host that turns up after the phone did should not have to wait for the
     * next change to learn what is going on. */
    bt_av_report_state();
}

static void on_transport(uint8_t cmd)      { bt_av_transport(cmd); }
static void on_volume(uint8_t volume)      { bt_av_set_volume(volume); }

static void on_radio(bool on, bool discoverable, bool disconnect)
{
    bt_av_set_radio(on, discoverable, disconnect);
}

static void on_gpio_cfg(const satlink_gpio_cfg_t *cfg) { expander_apply_cfg(cfg); }
static void on_gpio_out(uint32_t values, uint32_t mask) { expander_set_outputs(values, mask); }
static void on_adc_read(uint8_t index)     { expander_read_adc(index); }

void app_main(void)
{
    /* Bluedroid keeps its pairing keys here, so this comes first. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(audio_out_init());

    const link_handlers_t handlers = {
        .on_hello     = on_hello,
        .on_transport = on_transport,
        .on_volume    = on_volume,
        .on_radio     = on_radio,
        .on_gpio_cfg  = on_gpio_cfg,
        .on_gpio_out  = on_gpio_out,
        .on_adc_read  = on_adc_read,
    };
    ESP_ERROR_CHECK(link_start(&handlers));
    ESP_ERROR_CHECK(expander_start());

    ESP_ERROR_CHECK(bt_av_start(DEVICE_NAME));

    ESP_LOGI(TAG, "up");
}
