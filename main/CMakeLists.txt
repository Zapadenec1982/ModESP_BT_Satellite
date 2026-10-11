idf_component_register(
    SRCS
        "main.c"
        "bt_av.c"
        "audio_out.c"
        "audio_asrc.c"
        "link.c"
        "expander.c"
        "clock_probe.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        esp_app_format
        esp_adc
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_pcnt
        esp_driver_uart
        esp_ringbuf
        esp_timer
        nvs_flash
        satlink
)

option(AUDIO_DIAGNOSTIC_FIXED_TRIM "Bench: disable satellite live APLL trim" OFF)
if(AUDIO_DIAGNOSTIC_FIXED_TRIM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AUDIO_DIAGNOSTIC_FIXED_TRIM=1)
endif()
