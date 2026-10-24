idf_component_register(
    SRCS
        "main.c"
        "bt_av.c"
        "audio_out.c"
        "audio_asrc.c"
        "link.c"
        "clock_probe.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        esp_app_format
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_pcnt
        esp_driver_uart
        esp_ringbuf
        esp_timer
        nvs_flash
        satlink
)

# The LHDC V5 decoder is fetched, not carried (lhdc.pin). Without it the bt component
# fails much later on a missing lhdc_dec.h; say what is missing instead.
if(CONFIG_BT_A2DP_LHDCV5_DECODER)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(NOT "lhdcv5_decoder" IN_LIST build_components)
        message(FATAL_ERROR "CONFIG_BT_A2DP_LHDCV5_DECODER is set, but the LHDC V5 decoder "
            "is not in components/lhdcv5_decoder. Fetch it with tools/setup.sh --with-lhdc "
            "(setup.ps1 -WithLhdc), or build without sdkconfig.lhdc.")
    endif()
endif()

option(AUDIO_DIAGNOSTIC_FIXED_TRIM "Bench: disable satellite live APLL trim" OFF)
if(AUDIO_DIAGNOSTIC_FIXED_TRIM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AUDIO_DIAGNOSTIC_FIXED_TRIM=1)
endif()
