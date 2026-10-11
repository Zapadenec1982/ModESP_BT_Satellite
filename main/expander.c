#include "expander.h"
#include "expander_policy.h"

#include <inttypes.h>
#include <string.h>

#include "link.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sat.gpio";

#define EXP_TASK_STACK     2560
#define EXP_TASK_PRIO      (configMAX_PRIORITIES - 8)

typedef struct {
    gpio_num_t    gpio;
    uint8_t       flags;        /* SATLINK_PIN_CAN_* */
    adc_unit_t    adc_unit;
    adc_channel_t adc_channel;
} exp_pin_t;

/*
 * The lines this board lends out, in host index order.
 *
 * Empty by the customer's decision of 18.09.2026: there is nothing to connect
 * yet, and GPIO_MAP announces the set at runtime, so filling this in later costs
 * one row and no protocol change.
 *
 * What is available on the DevKitC once there is a use for it, from
 * docs/DEVICE_SPEC.md: 14, 18, 19, 21, 22, 23, 25, 32 and 33 either way, and 34,
 * 35, 36, 39 as inputs only — those four have no internal pulls, which is exactly
 * what the HAS_PULL flag is for.
 *
 * Example of a filled row, for whoever adds the first one:
 *   { GPIO_NUM_14, SATLINK_PIN_CAN_INPUT | SATLINK_PIN_CAN_OUTPUT |
 *                  SATLINK_PIN_CAN_ADC   | SATLINK_PIN_HAS_PULL,
 *     ADC_UNIT_2, ADC_CHANNEL_6 },
 */
static const exp_pin_t s_pins[] = {
    /* intentionally empty */
};

/* A literal, so that the preprocessor can see it, kept honest by the assertion
 * below. Whoever adds the first line to the table above will be told to change
 * this number too, at compile time, which is the right moment to be told. */
#define EXP_PIN_COUNT  0

_Static_assert(EXP_PIN_COUNT == sizeof(s_pins) / sizeof(s_pins[0]),
               "EXP_PIN_COUNT does not match s_pins");

#if EXP_PIN_COUNT == 0
/* With no lines, every bounds check below compares against zero and the compiler
 * rightly says the answer is a foregone conclusion. The code is written for the
 * table that will exist rather than the one that does, so the warnings are
 * suppressed here and this whole block disappears with the first entry. */
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif

static uint32_t s_input_mask;       /* lines currently configured as inputs */
static uint32_t s_output_mask;
static uint32_t s_analog_mask;      /* lines in mode 6 with their channel set up */
static uint32_t s_reported;         /* last levels the host was told about */
static uint32_t s_candidate;        /* levels seen but not yet stable */
static uint8_t  s_stable[SATLINK_GPIO_MAX];

static uint16_t s_adc_period[SATLINK_GPIO_MAX];   /* poll ticks, 0 = on request only */
static uint16_t s_adc_countdown[SATLINK_GPIO_MAX];
static adc_oneshot_unit_handle_t s_adc[2];
static adc_cali_handle_t s_cali[2];             /* NULL: no calibration, linear mv */

/* What expander_init found wrong with the ADC units. It runs before link_start
 * installs the LOG bridge and nothing before that is kept, so it only records;
 * expander_start, after the bridge, is the one that says so. */
static bool s_adc_unit_failed[2];
static bool s_adc_cali_missing[2];

/* Everything above is shared between the link's RX task (GPIO_CFG, GPIO_OUT,
 * ADC_READ) and the poller, on either core. A mutex rather than a critical
 * section, because GPIO, the ADC and the link are called with it held. It also
 * serialises the ADC: adc_oneshot_read on a unit someone else is reading fails
 * instead of waiting. */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static gpio_mode_t mode_to_gpio(uint8_t mode)
{
    switch (mode) {
    case SATLINK_PIN_INPUT:
    case SATLINK_PIN_INPUT_PU:
    case SATLINK_PIN_INPUT_PD:   return GPIO_MODE_INPUT;
    case SATLINK_PIN_OUTPUT:     return GPIO_MODE_OUTPUT;
    case SATLINK_PIN_OUTPUT_OD:  return GPIO_MODE_OUTPUT_OD;
    default:                     return GPIO_MODE_DISABLE;
    }
}

void expander_apply_cfg(const satlink_gpio_cfg_t *cfg)
{
    uint32_t newly_input = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (uint8_t i = 0; i < cfg->count; i++) {
        uint8_t index   = cfg->pins[i].index;
        uint8_t mode    = cfg->pins[i].mode;
        uint8_t initial = cfg->pins[i].initial;

        if (index >= EXP_PIN_COUNT) {
            ESP_LOGW(TAG, "host configured index %u; this board offers %u lines",
                     index, (unsigned)EXP_PIN_COUNT);
            continue;
        }
        const exp_pin_t *pin = &s_pins[index];

        /* Asking for something the silicon cannot do is answered with the nearest
         * legal thing plus a complaint, because the protocol has no request and
         * response pairs to say no with. */
        exp_mode_fix_t fix;
        const uint8_t asked = mode;
        mode = exp_legal_mode(asked, pin->flags, &fix);
        switch (fix) {
        case EXP_MODE_NO_PULL:
            ESP_LOGW(TAG, "index %u (GPIO%d) has no internal pull; using a plain input",
                     index, pin->gpio);
            break;
        case EXP_MODE_INPUT_ONLY:
            ESP_LOGW(TAG, "index %u (GPIO%d) is input only; using a plain input",
                     index, pin->gpio);
            break;
        case EXP_MODE_NO_ADC:
            ESP_LOGW(TAG, "index %u (GPIO%d) has no ADC; using a plain input", index, pin->gpio);
            break;
        case EXP_MODE_UNKNOWN:
            ESP_LOGW(TAG, "index %u (GPIO%d): mode %u is not defined; line disabled",
                     index, pin->gpio, asked);
            break;
        case EXP_MODE_AS_ASKED:
            break;
        }

        s_input_mask  &= ~(1u << index);
        s_output_mask &= ~(1u << index);
        s_analog_mask &= ~(1u << index);
        s_adc_period[index] = 0;

        if (mode == SATLINK_PIN_ANALOG) {
            adc_oneshot_chan_cfg_t chan = {
                .atten = ADC_ATTEN_DB_12,           /* full scale, roughly 0..3.1 V */
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            if (s_adc[pin->adc_unit] != NULL &&
                adc_oneshot_config_channel(s_adc[pin->adc_unit], pin->adc_channel, &chan) == ESP_OK) {
                s_analog_mask |= (1u << index);
                /* Hundreds of milliseconds on the wire, poll ticks here. */
                s_adc_period[index] = exp_adc_period_ticks(initial);
                s_adc_countdown[index] = s_adc_period[index];
            } else {
                ESP_LOGE(TAG, "index %u: ADC setup failed", index);
            }
            continue;
        }

        gpio_config_t io = {
            .pin_bit_mask = 1ULL << pin->gpio,
            .mode = mode_to_gpio(mode),
            .pull_up_en   = (mode == SATLINK_PIN_INPUT_PU) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = (mode == SATLINK_PIN_INPUT_PD) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (mode == SATLINK_PIN_OUTPUT || mode == SATLINK_PIN_OUTPUT_OD) {
            /* Outputs come up in what the host asked for and not a moment before:
             * until GPIO_CFG arrives every line is an input. The output latch does
             * not depend on the driver being on, so it is written first and the pad
             * starts driving the host's level instead of a stale one. */
            gpio_set_level(pin->gpio, initial ? 1 : 0);
        }
        if (gpio_config(&io) != ESP_OK) {
            ESP_LOGE(TAG, "index %u (GPIO%d): configuration refused", index, pin->gpio);
            continue;
        }

        if (mode == SATLINK_PIN_OUTPUT || mode == SATLINK_PIN_OUTPUT_OD) {
            s_output_mask |= (1u << index);
        } else if (exp_mode_is_input(mode)) {
            s_input_mask |= (1u << index);
            newly_input |= (1u << index);
        }
    }

    if (newly_input != 0) {
        /* The host has no other way to learn a level that never changes. Without
         * this, a button nobody pressed does not exist for it. */
        uint32_t values = 0;
        for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
            if ((newly_input & (1u << i)) && gpio_get_level(s_pins[i].gpio)) {
                values |= (1u << i);
            }
        }
        exp_debounce_adopt(newly_input, values, &s_candidate, s_stable, &s_reported);
        link_send_gpio_in(s_reported, newly_input);
    }
    xSemaphoreGive(s_lock);
}

void expander_set_outputs(uint32_t values, uint32_t mask)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
        uint32_t bit = 1u << i;
        if ((mask & bit) == 0 || (s_output_mask & bit) == 0) {
            continue;
        }
        gpio_set_level(s_pins[i].gpio, (values & bit) ? 1 : 0);
    }
    xSemaphoreGive(s_lock);
}

/* Called with s_lock held. */
static void adc_sample_and_send(uint8_t index)
{
    const exp_pin_t *pin = &s_pins[index];
    if (!exp_adc_readable(pin->flags, (s_analog_mask >> index) & 1u) ||
        s_adc[pin->adc_unit] == NULL) {
        return;
    }
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc[pin->adc_unit], pin->adc_channel, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "index %u: ADC read failed (%s)", index, esp_err_to_name(err));
        return;
    }
    /* PROTOCOL.md 11 promises millivolts calibrated on the device. Without a
     * calibration scheme, the nominal 12 bits over roughly 3.1 V at this
     * attenuation; expander_start has said so in LOG. */
    int mv = -1;
    if (s_cali[pin->adc_unit] == NULL ||
        adc_cali_raw_to_voltage(s_cali[pin->adc_unit], raw, &mv) != ESP_OK || mv < 0) {
        mv = (int)(((uint32_t)raw * 3100u) / 4095u);
    }
    link_send_adc_value(index, (uint16_t)raw, (uint16_t)mv);
}

void expander_read_adc(uint8_t index)
{
    if (index >= EXP_PIN_COUNT) {
        ESP_LOGW(TAG, "host asked to read index %u; this board offers %u lines",
                 index, (unsigned)EXP_PIN_COUNT);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Not set up on first use instead: configuring the channel hands the pad to
     * the ADC and would take a line the host made digital away from it. */
    if (!exp_adc_readable(s_pins[index].flags, (s_analog_mask >> index) & 1u)) {
        ESP_LOGW(TAG, "host asked to read index %u (GPIO%d), which is not an analog input; "
                 "GPIO_CFG mode 6 first", index, s_pins[index].gpio);
    } else {
        adc_sample_and_send(index);
    }
    xSemaphoreGive(s_lock);
}

/* The poll period. Not vTaskDelay: below one FreeRTOS tick (10 ms at the 100 Hz
 * this build runs) it rounds to 0, which only yields, and the debounce and the
 * ADC period would count loop passes instead of 5 ms. */
static void poll_tick(void *arg)
{
    xTaskNotifyGive((TaskHandle_t)arg);
}

static void expander_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* One pass under the lock, the send included: a GPIO_CFG on the other
         * core must neither undo a level this pass reports nor queue a GPIO_IN
         * built from the levels before it. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t changed = 0;
        for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
            uint32_t bit = 1u << i;
            if ((s_input_mask & bit) == 0) {
                continue;
            }
            uint32_t level = gpio_get_level(s_pins[i].gpio) ? bit : 0;
            if (exp_debounce_step(level, bit, &s_candidate, &s_stable[i], &s_reported)) {
                changed |= bit;
            }
        }

        if (changed != 0) {
            /* By event, never by polling: that is what keeps a button under the
             * 30 ms the spec asks for. */
            link_send_gpio_in(s_reported, changed);
        }

        for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
            if (exp_adc_tick(&s_adc_countdown[i], s_adc_period[i])) {
                adc_sample_and_send(i);
            }
        }
        xSemaphoreGive(s_lock);
    }
}

uint32_t expander_caps(void)
{
    uint32_t caps = SATLINK_CAP_EXPANDER;
    for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
        if (s_pins[i].flags & SATLINK_PIN_CAN_ADC) {
            caps |= SATLINK_CAP_ADC;
            break;
        }
    }
    return caps;
}

/* Built from the table every time rather than kept: the host can greet before
 * expander_start has run, and the table is all there is to know. */
void expander_send_map(void)
{
    satlink_gpio_map_t map = { .count = (uint8_t)EXP_PIN_COUNT };
    for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
        map.pins[i].index = i;
        map.pins[i].gpio  = (uint8_t)s_pins[i].gpio;
        map.pins[i].flags = s_pins[i].flags;
    }
    link_send_gpio_map(&map);
}

esp_err_t expander_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    bool needs_adc[2] = { false, false };

    for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
        if (s_pins[i].flags & SATLINK_PIN_CAN_ADC) {
            needs_adc[s_pins[i].adc_unit] = true;
        }
    }

    for (int unit = 0; unit < 2; unit++) {
        if (!needs_adc[unit]) {
            continue;
        }
        adc_oneshot_unit_init_cfg_t cfg = { .unit_id = (adc_unit_t)unit };
        if (adc_oneshot_new_unit(&cfg, &s_adc[unit]) != ESP_OK) {
            s_adc_unit_failed[unit] = true;
            s_adc[unit] = NULL;
            continue;
        }

        /* Line fitting, the scheme IDF has for the ESP32, from the eFuse two-point
         * or Vref values. default_vref is the 1100 mV the IDF calibration guide
         * gives as the design reference, used only on a chip with neither. */
        adc_cali_line_fitting_config_t cali = {
            .unit_id = (adc_unit_t)unit,
            .atten = ADC_ATTEN_DB_12,                   /* what mode 6 configures */
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .default_vref = 1100,
        };
        if (adc_cali_create_scheme_line_fitting(&cali, &s_cali[unit]) != ESP_OK) {
            s_adc_cali_missing[unit] = true;
            s_cali[unit] = NULL;
        }
    }
    return ESP_OK;
}

esp_err_t expander_start(void)
{
    /* Straight after our own HELLO, always, even when there is nothing in it: an
     * empty map is the answer to "what have you got", not the absence of one.
     * Every host HELLO gets it again, from main.c. */
    expander_send_map();

    /* expander_init's findings, now that LOG is there to carry them. */
    for (int unit = 0; unit < 2; unit++) {
        if (s_adc_unit_failed[unit]) {
            ESP_LOGE(TAG, "ADC unit %d would not start", unit + 1);
        } else if (s_adc_cali_missing[unit]) {
            ESP_LOGW(TAG, "ADC unit %d: no calibration; mv will be a linear estimate", unit + 1);
        }
    }

    if (EXP_PIN_COUNT == 0) {
        ESP_LOGI(TAG, "no lines offered yet; GPIO_MAP sent empty, poller not started");
        return ESP_OK;
    }

    TaskHandle_t task = NULL;
    ESP_RETURN_ON_FALSE(
        xTaskCreate(expander_task, "expander", EXP_TASK_STACK, NULL, EXP_TASK_PRIO, &task) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "no memory for the expander task");

    const esp_timer_create_args_t poll_args = {
        .callback = poll_tick,
        .arg = task,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "exp_poll",
    };
    esp_timer_handle_t poll_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&poll_args, &poll_timer), TAG, "poll timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(poll_timer, EXP_POLL_MS * 1000), TAG,
                        "poll timer start");

    ESP_LOGI(TAG, "%u lines, %d ms debounce", (unsigned)EXP_PIN_COUNT, EXP_DEBOUNCE_MS);
    return ESP_OK;
}
