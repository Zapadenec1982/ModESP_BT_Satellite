#include "expander.h"
#include "expander_policy.h"

#include <inttypes.h>
#include <string.h>

#include "link.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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
static uint32_t s_reported;         /* last levels the host was told about */
static uint32_t s_candidate;        /* levels seen but not yet stable */
static uint8_t  s_stable[SATLINK_GPIO_MAX];

static uint16_t s_adc_period[SATLINK_GPIO_MAX];   /* poll ticks, 0 = on request only */
static uint16_t s_adc_countdown[SATLINK_GPIO_MAX];
static adc_oneshot_unit_handle_t s_adc[2];

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
        s_adc_period[index] = 0;

        if (mode == SATLINK_PIN_ANALOG) {
            adc_oneshot_chan_cfg_t chan = {
                .atten = ADC_ATTEN_DB_12,           /* full scale, roughly 0..3.1 V */
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            if (s_adc[pin->adc_unit] != NULL &&
                adc_oneshot_config_channel(s_adc[pin->adc_unit], pin->adc_channel, &chan) == ESP_OK) {
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
        if (gpio_config(&io) != ESP_OK) {
            ESP_LOGE(TAG, "index %u (GPIO%d): configuration refused", index, pin->gpio);
            continue;
        }

        if (mode == SATLINK_PIN_OUTPUT || mode == SATLINK_PIN_OUTPUT_OD) {
            /* Outputs come up in what the host asked for and not a moment before:
             * until GPIO_CFG arrives every line is an input. */
            gpio_set_level(pin->gpio, initial ? 1 : 0);
            s_output_mask |= (1u << index);
        } else if (exp_mode_is_input(mode)) {
            s_input_mask |= (1u << index);
            newly_input |= (1u << index);
            s_stable[index] = EXP_STABLE_SAMPLES;    /* take the first read as settled */
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
        s_reported = (s_reported & ~newly_input) | values;
        s_candidate = s_reported;
        link_send_gpio_in(s_reported, newly_input);
    }
}

void expander_set_outputs(uint32_t values, uint32_t mask)
{
    for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
        uint32_t bit = 1u << i;
        if ((mask & bit) == 0 || (s_output_mask & bit) == 0) {
            continue;
        }
        gpio_set_level(s_pins[i].gpio, (values & bit) ? 1 : 0);
    }
}

static void adc_sample_and_send(uint8_t index)
{
    const exp_pin_t *pin = &s_pins[index];
    if (s_adc[pin->adc_unit] == NULL) {
        return;
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc[pin->adc_unit], pin->adc_channel, &raw) != ESP_OK) {
        return;
    }
    /* Uncalibrated for now: 12 bits over roughly 3.1 V at this attenuation. The
     * curve-fitting calibration is a later concern; the raw value is the honest
     * one and it travels alongside. */
    uint32_t mv = ((uint32_t)raw * 3100u) / 4095u;
    link_send_adc_value(index, (uint16_t)raw, (uint16_t)mv);
}

void expander_read_adc(uint8_t index)
{
    if (index >= EXP_PIN_COUNT) {
        ESP_LOGW(TAG, "host asked to read index %u; this board offers %u lines",
                 index, (unsigned)EXP_PIN_COUNT);
        return;
    }
    adc_sample_and_send(index);
}

static void expander_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(EXP_POLL_MS));

        uint32_t changed = 0;
        for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
            uint32_t bit = 1u << i;
            if ((s_input_mask & bit) == 0) {
                continue;
            }
            uint32_t level = gpio_get_level(s_pins[i].gpio) ? bit : 0;

            if (level == (s_candidate & bit)) {
                if (s_stable[i] < EXP_STABLE_SAMPLES) {
                    s_stable[i]++;
                }
            } else {
                s_candidate = (s_candidate & ~bit) | level;
                s_stable[i] = 0;
            }

            if (s_stable[i] >= EXP_STABLE_SAMPLES && (s_reported & bit) != level) {
                s_reported = (s_reported & ~bit) | level;
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

esp_err_t expander_start(void)
{
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
            ESP_LOGE(TAG, "ADC unit %d would not start", unit + 1);
            s_adc[unit] = NULL;
        }
    }

    /* Straight after our own HELLO, always, even when there is nothing in it: an
     * empty map is the answer to "what have you got", not the absence of one.
     * Every host HELLO gets it again, from main.c. */
    expander_send_map();

    if (EXP_PIN_COUNT == 0) {
        ESP_LOGI(TAG, "no lines offered yet; GPIO_MAP sent empty, poller not started");
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(
        xTaskCreate(expander_task, "expander", EXP_TASK_STACK, NULL, EXP_TASK_PRIO, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "no memory for the expander task");

    ESP_LOGI(TAG, "%u lines, %d ms debounce", (unsigned)EXP_PIN_COUNT, EXP_DEBOUNCE_MS);
    return ESP_OK;
}
