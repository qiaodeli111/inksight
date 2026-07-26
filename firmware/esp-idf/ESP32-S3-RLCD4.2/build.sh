#!/bin/bash
# Build and flash Inksight ESP-IDF firmware for Waveshare ESP32-S3 RLCD 4.2"
# Source: https://github.com/wickenzh/inksight

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ESP-IDF paths
export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf"
export IDF_TOOLS_PATH="$HOME/.espressif"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.14_env"
export ESP_ROM_ELF_DIR="$HOME/.espressif/tools/esp-rom-elfs/"

# Toolchain paths
XTENSA_DIR="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin"
ESP_CLANG_DIR="$HOME/.espressif/tools/esp-clang/19.1.5-20250317/esp-clang/bin"
NINJA_DIR="$HOME/.espressif/tools/ninja/1.12.1"

export PATH="$IDF_PATH/tools:$IDF_PYTHON_ENV_PATH/bin:$XTENSA_DIR:$ESP_CLANG_DIR:$NINJA_DIR:$PATH"

ACTION="${1:-build}"

case "$ACTION" in
  build)
    idf.py build
    ;;
  flash)
    idf.py -p "${ESP_PORT:-/dev/cu.usbserial-0001}" flash
    ;;
  monitor)
    idf.py -p "${ESP_PORT:-/dev/cu.usbserial-0001}" monitor
    ;;
  flash-monitor)
    idf.py -p "${ESP_PORT:-/dev/cu.usbserial-0001}" flash monitor
    ;;
  menuconfig)
    idf.py menuconfig
    ;;
  clean)
    idf.py fullclean
    ;;
  *)
    echo "用法: $0 [build|flash|monitor|flash-monitor|menuconfig|clean]"
    echo "  export ESP_PORT=/dev/cu.usbserial-XXXXX  # 指定串口, 默认 /dev/cu.usbserial-0001"
    exit 1
    ;;
esac
