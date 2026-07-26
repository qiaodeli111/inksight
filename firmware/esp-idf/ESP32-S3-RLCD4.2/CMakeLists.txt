cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.1.3")
set(EXTRA_COMPONENT_DIRS components/esp_codec_dev)
project(inksight_waveshare_rlcd)
