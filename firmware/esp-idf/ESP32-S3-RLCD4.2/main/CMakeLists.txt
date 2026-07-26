idf_component_register(
    SRCS
        "app_main.c"
        "audio.c"
        "backend.c"
        "battery.c"
        "codec_board.c"
        "codec_init.c"
        "config_store.c"
        "dummy_codec.c"
        "portal.c"
        "st7305.c"
        "ui.c"
        "voice_chat.c"
        "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_adc
        esp_event
        esp_http_client
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        json
        mbedtls
        nvs_flash
        esp_codec_dev
        esp_driver_i2c
        esp_driver_i2s
        tcp_transport
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
