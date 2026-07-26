set(CODEC_DEV_DIR "${CMAKE_CURRENT_LIST_DIR}/../components/esp_codec_dev")

idf_component_register(
    SRCS
        "app_main.c"
        "audio.c"
        "backend.c"
        "battery.c"
        "config_store.c"
        "portal.c"
        "st7305.c"
        "ui.c"
        "voice_chat.c"
        "wifi_manager.c"
        "${CODEC_DEV_DIR}/esp_codec_dev.c"
        "${CODEC_DEV_DIR}/esp_codec_dev_vol.c"
        "${CODEC_DEV_DIR}/esp_codec_dev_if.c"
        "${CODEC_DEV_DIR}/audio_codec_sw_vol.c"
        "${CODEC_DEV_DIR}/platform/audio_codec_gpio.c"
        "${CODEC_DEV_DIR}/platform/audio_codec_ctrl_i2c.c"
        "${CODEC_DEV_DIR}/platform/audio_codec_data_i2s.c"
        "${CODEC_DEV_DIR}/platform/audio_codec_ctrl_spi.c"
        "${CODEC_DEV_DIR}/platform/esp_codec_dev_os.c"
        "${CODEC_DEV_DIR}/device/es8311/es8311.c"
        "${CODEC_DEV_DIR}/device/es7210/es7210.c"
    INCLUDE_DIRS "."
        "${CODEC_DEV_DIR}/include"
        "${CODEC_DEV_DIR}/interface"
        "${CODEC_DEV_DIR}/device/include"
        "${CODEC_DEV_DIR}/device/priv_include"
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
        tcp_transport
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
