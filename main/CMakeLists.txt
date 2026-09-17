idf_component_register(
    SRCS
        "main.c"
        "bt_av.c"
        "audio_out.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        esp_driver_i2s
        esp_ringbuf
        nvs_flash
)
