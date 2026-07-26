#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "config_store.h"

typedef struct {
    bool is_fallback;
    int refresh_minutes;
    char mode_id[65];
} backend_render_info_t;

esp_err_t backend_render(
    inksight_config_t *config,
    bool next_mode,
    float battery_voltage,
    int wifi_rssi,
    uint8_t *frame,
    backend_render_info_t *info
);

esp_err_t backend_decode_bmp(
    const uint8_t *data,
    size_t length,
    uint8_t *frame
);

/**
 * @brief Fetch focus_listening and always_active flags from server config.
 *
 * Calls GET /api/config/{mac}. On success sets *focus_listening and
 * *always_active to the values from the server.
 */
esp_err_t backend_fetch_focus_config(
    inksight_config_t *config,
    bool *focus_listening,
    bool *always_active
);

/**
 * @brief Poll for a pending alert BMP.
 *
 * Calls GET /api/device/{mac}/alert-bmp. If an alert exists (HTTP 200),
 * decodes the BMP into frame and returns ESP_OK. If no alert (HTTP 204),
 * returns ESP_ERR_NOT_FOUND.
 */
esp_err_t backend_fetch_alert_bmp(
    inksight_config_t *config,
    uint8_t *frame
);
