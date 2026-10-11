/*
 * modesp-bt-satellite — a Bluetooth A2DP receiver that hands audio to a host over
 * I2S and everything else over UART. See docs/DEVICE_SPEC.md and docs/PROTOCOL.md.
 *
 * Order matters here. The output comes up first because samples can start
 * arriving the moment a phone connects; the link comes up before the radio so
 * that the first BT_STATE has somewhere to go; the expander comes up last
 * because its GPIO_MAP must follow the HELLO.
 */

#include "audio_out.h"
#include "bt_av.h"
#include "clock_probe.h"
#include "expander.h"
#include "link.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "satlink/proto.h"

static const char *TAG = "sat";

/* What the phone shows in its Bluetooth list. */
#define DEVICE_NAME "modesp satellite"

static void on_hello(const satlink_hello_t *hello)
{
    (void)hello;
#if !CONFIG_SAT_LINK_ONLY
    /*
     * A host that turns up after the phone did should not have to wait for the
     * next change to learn what is going on — and must not be left to measure the
     * sample rate off the stream, which is exactly what happens when STREAM_FMT
     * is announced once and missed once.
     *
     * Rate limited rather than done once, though. The host repeats its HELLO
     * every couple of seconds, and answering every one with a full set of state
     * put a needless message a second on a link that also carries audio
     * telemetry. A limit rather than a one-shot because a host that restarts
     * greets us again and has every right to be told everything a second time.
     */
    /*
     * The format goes out on every greeting, rate limit or not.
     *
     * PROTOCOL.md 1.2 lets the host refuse to play until it has been told the
     * format, which is the right thing for it to do -- a guessed slot width is
     * noise, not silence, and nobody can tell it from a bad codec by ear. A host
     * that restarts must not then wait ten seconds to learn it. The message is six
     * bytes; the state dump behind it is not, and that keeps its limit.
     */
    /* A host that was slow to greet us left the device narrow until a reboot.
     * 1.2 allows the width to change whenever, so long as it is announced, so a
     * late greeting is now enough to widen. Does nothing when already correct. */
    bt_av_apply_host_caps();
    bt_av_report_stream_fmt();
    /* Protocol 1.8 asks for BT_CONFIG and BOND_LIST after every greeting, and the
     * pairing mode follows the host that greeted last: not rate limited. */
    bt_av_host_hello();
#endif

    /* PROTOCOL.md 3.4: GPIO_MAP after every host HELLO, always, in a link-only
     * build too. It went out once at boot, which a host that greeted later never
     * saw. Not rate limited: it is 1 + 3n bytes and the host reads it as the
     * answer to "what lines does this thing have". */
    expander_send_map();

#if !CONFIG_SAT_LINK_ONLY
    static int64_t next_announce_us;
    int64_t now = esp_timer_get_time();
    if (now < next_announce_us) {
        return;
    }
    next_announce_us = now + (10 * 1000 * 1000);

    bt_av_report_state();
#endif
}

#if CONFIG_SAT_LINK_ONLY
/* Nothing to forward these to. Answered rather than ignored, so the host can see
 * that its commands arrive and are understood. */
static void on_transport(uint8_t cmd)  { ESP_LOGI(TAG, "transport %u, no radio in this build", cmd); }
static void on_volume(uint8_t volume)  { ESP_LOGI(TAG, "volume %u, no radio in this build", volume); }
static void on_radio(bool on, bool discoverable, bool disconnect)
{
    ESP_LOGI(TAG, "radio %u/%u/%u, no radio in this build", on, discoverable, disconnect);
}
#else
static void on_transport(uint8_t cmd)  { bt_av_transport(cmd); }
static void on_volume(uint8_t volume)  { bt_av_set_volume(volume); }
static void on_radio(bool on, bool discoverable, bool disconnect)
{
    bt_av_set_radio(on, discoverable, disconnect);
}
#endif

static uint32_t own_caps(void);

/* Protocol 1.9, before our answering HELLO goes out: a host that clocks the link gets a
 * slave, any other host gets the master it has always had. Then the answer says which. */
static uint32_t on_host_caps(uint32_t host_caps)
{
#if !CONFIG_SAT_LINK_ONLY
    const bool slave = (host_caps & SATLINK_HOST_CAP_I2S_MASTER) != 0;
    if (audio_out_set_role(slave) != ESP_OK)
        ESP_LOGE(TAG, "I2S could not become the %s", slave ? "slave" : "master");
#else
    (void)host_caps;
#endif
    return own_caps();
}

static void on_gpio_cfg(const satlink_gpio_cfg_t *cfg) { expander_apply_cfg(cfg); }
static void on_gpio_out(uint32_t values, uint32_t mask) { expander_set_outputs(values, mask); }
static void on_adc_read(uint8_t index)     { expander_read_adc(index); }

/* What this build can actually do. The host believes the HELLO, so a bit claimed
 * here has to be a bit honoured later. */
static uint32_t own_caps(void)
{
    uint32_t caps = expander_caps();
#if !CONFIG_SAT_LINK_ONLY
    /* 1.9: claimed only while true, because the host drives BCLK and WS on it. */
    if (audio_out_is_slave()) caps |= SATLINK_CAP_I2S_SLAVE;
#endif
#if !CONFIG_SAT_LINK_ONLY
    caps |= SATLINK_CAP_A2DP_SINK | SATLINK_CAP_AVRCP_META |
            SATLINK_CAP_ABS_VOLUME | SATLINK_CAP_COVER_ART |
            /* Alongside COVER_ART and never without it: the host's gate requires
             * both, and this build answers COVER_RETRY only for a track whose cover
             * it was sending in the first place. */
            SATLINK_CAP_COVER_RETRY |
            /* Protocol 1.8: settings, bonds and host-confirmed pairing. */
            SATLINK_CAP_BT_SETTINGS;
#endif
    return caps;
}

void app_main(void)
{
    /* Bluedroid keeps its pairing keys here, so this comes first. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

#if !CONFIG_SAT_LINK_ONLY
    ESP_ERROR_CHECK(audio_out_init());
#endif

    const link_handlers_t handlers = {
        .on_hello     = on_hello,
#if !CONFIG_SAT_LINK_ONLY
        .on_format_ready = bt_av_format_ready,
        .on_cover_retry  = bt_av_cover_retry,
        .on_pair_reply    = bt_av_pair_reply,
        .on_bt_config_set = bt_av_config_set,
        .on_bond_remove   = bt_av_bond_remove,
        .on_bt_refresh    = bt_av_bt_refresh,
#endif
        .on_transport = on_transport,
        .on_volume    = on_volume,
        .on_radio     = on_radio,
        .on_gpio_cfg  = on_gpio_cfg,
        .on_gpio_out  = on_gpio_out,
        .on_adc_read  = on_adc_read,
        .on_host_caps = on_host_caps,
    };
    /* The expander's state before the link that delivers GPIO_CFG to it. */
    ESP_ERROR_CHECK(expander_init());
    ESP_ERROR_CHECK(link_start(&handlers, own_caps()));
    ESP_ERROR_CHECK(expander_start());

    /* After both peripherals have claimed their pins. The probe only listens on
     * the I2S clock pads and never drives them, so it is safe in either role. */
    ESP_ERROR_CHECK(clock_probe_start());

#if CONFIG_SAT_LINK_ONLY
    ESP_LOGW(TAG, "link only: no Bluetooth, no I2S, nothing but the wire");
#else
    ESP_ERROR_CHECK(bt_av_start(DEVICE_NAME));
#endif

    ESP_LOGI(TAG, "up");
}
