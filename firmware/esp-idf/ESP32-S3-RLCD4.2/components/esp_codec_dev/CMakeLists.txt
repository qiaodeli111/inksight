set(COMPONENT_ADD_INCLUDEDIRS include interface device/include device/priv_include)
set(COMPONENT_SRCS
  esp_codec_dev.c
  esp_codec_dev_vol.c
  esp_codec_dev_if.c
  audio_codec_sw_vol.c
  platform/audio_codec_gpio.c
  platform/audio_codec_ctrl_i2c.c
  platform/audio_codec_data_i2s.c
  platform/audio_codec_ctrl_spi.c
  platform/esp_codec_dev_os.c
  device/es8311/es8311.c
  device/es7210/es7210.c
)
idf_component_register(SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS ${COMPONENT_ADD_INCLUDEDIRS}
    PRIV_REQUIRES freertos esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi)
