#include "backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "board_config.h"
#include "config_store.h"
#include "wifi_manager.h"

typedef struct {
    uint8_t *body;
    size_t body_length;
    size_t body_capacity;
    bool allocation_failed;
    char fallback_header[8];
    char refresh_header[16];
    char mode_id_header[65];
} http_response_t;

static const char *TAG = "backend";

static void copy_header_value(
    char *destination,
    size_t destination_size,
    const char *value,
    int value_length
) {
    if (destination_size == 0) {
        return;
    }
    size_t length = value_length > 0 ? (size_t)value_length : 0;
    if (length >= destination_size) {
        length = destination_size - 1;
    }
    memcpy(destination, value, length);
    destination[length] = '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *event) {
    http_response_t *response = event->user_data;
    if (response == NULL) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key != NULL &&
        event->header_value != NULL) {
        if (strcasecmp(event->header_key, "X-Content-Fallback") == 0) {
            copy_header_value(
                response->fallback_header,
                sizeof(response->fallback_header),
                event->header_value,
                (int)strlen(event->header_value)
            );
        } else if (strcasecmp(
                       event->header_key,
                       "X-Refresh-Minutes"
                   ) == 0) {
            copy_header_value(
                response->refresh_header,
                sizeof(response->refresh_header),
                event->header_value,
                (int)strlen(event->header_value)
            );
        } else if (strcasecmp(event->header_key, "X-Mode-Id") == 0) {
            copy_header_value(
                response->mode_id_header,
                sizeof(response->mode_id_header),
                event->header_value,
                (int)strlen(event->header_value)
            );
        }
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA ||
        event->data == NULL ||
        event->data_len <= 0) {
        return ESP_OK;
    }

    size_t required = response->body_length + (size_t)event->data_len;
    if (required > INKSIGHT_HTTP_MAX_BODY) {
        response->allocation_failed = true;
        return ESP_ERR_NO_MEM;
    }
    if (required > response->body_capacity) {
        size_t next_capacity =
            response->body_capacity > 0 ? response->body_capacity : 32768;
        while (next_capacity < required) {
            next_capacity *= 2;
        }
        if (next_capacity > INKSIGHT_HTTP_MAX_BODY) {
            next_capacity = INKSIGHT_HTTP_MAX_BODY;
        }
        uint8_t *resized = realloc(response->body, next_capacity);
        if (resized == NULL) {
            response->allocation_failed = true;
            return ESP_ERR_NO_MEM;
        }
        response->body = resized;
        response->body_capacity = next_capacity;
    }

    memcpy(
        response->body + response->body_length,
        event->data,
        (size_t)event->data_len
    );
    response->body_length += (size_t)event->data_len;
    return ESP_OK;
}

static void response_release(http_response_t *response) {
    if (response == NULL) {
        return;
    }
    free(response->body);
    memset(response, 0, sizeof(*response));
}

static esp_err_t perform_request(
    const char *url,
    esp_http_client_method_t method,
    const char *device_token,
    const char *post_body,
    http_response_t *response,
    int *status_code
) {
    if (url == NULL || response == NULL || status_code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(response, 0, sizeof(*response));
    *status_code = 0;

    esp_http_client_config_t client_config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = INKSIGHT_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = false,
        .max_redirection_count = 4,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client =
        esp_http_client_init(&client_config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Connection", "close");
    if (device_token != NULL && device_token[0] != '\0') {
        esp_http_client_set_header(client, "X-Device-Token", device_token);
    }
    if (post_body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(
            client,
            post_body,
            (int)strlen(post_body)
        );
    }

    esp_err_t err = esp_http_client_perform(client);
    *status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (response->allocation_failed) {
        response_release(response);
        return ESP_ERR_NO_MEM;
    }
    return err;
}

static esp_err_t ensure_device_token(inksight_config_t *config) {
    if (config->device_token[0] != '\0') {
        return ESP_OK;
    }

    char mac[18];
    wifi_manager_mac_string(mac, sizeof(mac));
    char url[320];
    int result = snprintf(
        url,
        sizeof(url),
        "%s/api/device/%s/token",
        config->server,
        mac
    );
    if (result < 0 || (size_t)result >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        http_response_t response;
        int status_code = 0;
        esp_err_t err = perform_request(
            url,
            HTTP_METHOD_POST,
            NULL,
            "{}",
            &response,
            &status_code
        );
        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Token request failed: %s",
                esp_err_to_name(err)
            );
            response_release(&response);
            continue;
        }
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGW(TAG, "Token endpoint returned HTTP %d", status_code);
            response_release(&response);
            continue;
        }

        cJSON *root = cJSON_ParseWithLength(
            (const char *)response.body,
            response.body_length
        );
        const cJSON *token = root != NULL
            ? cJSON_GetObjectItemCaseSensitive(root, "token")
            : NULL;
        if (!cJSON_IsString(token) || token->valuestring == NULL ||
            token->valuestring[0] == '\0' ||
            strlen(token->valuestring) >= sizeof(config->device_token)) {
            ESP_LOGW(TAG, "Token response did not contain a usable token");
            cJSON_Delete(root);
            response_release(&response);
            continue;
        }

        strlcpy(
            config->device_token,
            token->valuestring,
            sizeof(config->device_token)
        );
        cJSON_Delete(root);
        response_release(&response);

        ESP_RETURN_ON_ERROR(
            config_store_save_device_token(config->device_token),
            TAG,
            "Could not persist device token"
        );
        ESP_LOGI(TAG, "Device token obtained");
        return ESP_OK;
    }
    return ESP_FAIL;
}

static uint16_t read_u16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int32_t read_i32(const uint8_t *bytes) {
    return (int32_t)read_u32(bytes);
}

static bool rgb_is_black(uint8_t red, uint8_t green, uint8_t blue) {
    return (int)red + (int)green + (int)blue < 384;
}

static void frame_set_black(uint8_t *frame, int x, int y) {
    frame[y * INKSIGHT_ROW_BYTES + x / 8] &=
        (uint8_t)~(0x80U >> (x % 8));
}

esp_err_t backend_decode_bmp(
    const uint8_t *data,
    size_t length,
    uint8_t *frame
) {
    if (data == NULL || frame == NULL || length < 54 ||
        data[0] != 'B' || data[1] != 'M') {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pixel_offset = read_u32(data + 10);
    uint32_t dib_size = read_u32(data + 14);
    int32_t width = read_i32(data + 18);
    int32_t signed_height = read_i32(data + 22);
    uint16_t planes = read_u16(data + 26);
    uint16_t bits_per_pixel = read_u16(data + 28);
    uint32_t compression = read_u32(data + 30);

    bool supported_height =
        signed_height == INKSIGHT_HEIGHT ||
        signed_height == -INKSIGHT_HEIGHT;
    if (dib_size < 40 ||
        (size_t)dib_size > length - 14U ||
        width != INKSIGHT_WIDTH ||
        !supported_height ||
        planes != 1 ||
        compression != 0 ||
        (bits_per_pixel != 1 &&
         bits_per_pixel != 8 &&
         bits_per_pixel != 24 &&
         bits_per_pixel != 32)) {
        ESP_LOGW(
            TAG,
            "Unsupported BMP: %ldx%ld bpp=%u compression=%lu",
            (long)width,
            (long)signed_height,
            bits_per_pixel,
            (unsigned long)compression
        );
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t row_stride =
        (((size_t)width * bits_per_pixel + 31U) / 32U) * 4U;
    size_t palette_offset = 14U + (size_t)dib_size;
    if ((size_t)pixel_offset < palette_offset ||
        pixel_offset >= length ||
        row_stride > length ||
        (size_t)INKSIGHT_HEIGHT >
            (length - pixel_offset) / row_stride) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *palette = data + palette_offset;
    bool has_palette =
        bits_per_pixel <= 8 &&
        palette_offset < pixel_offset &&
        pixel_offset - palette_offset >=
            (bits_per_pixel == 1 ? 8U : 4U);

    memset(frame, 0xFF, INKSIGHT_FRAME_BYTES);
    bool top_down = signed_height < 0;

    for (int source_y = 0; source_y < INKSIGHT_HEIGHT; source_y++) {
        int destination_y =
            top_down ? source_y : INKSIGHT_HEIGHT - 1 - source_y;
        const uint8_t *row =
            data + pixel_offset + (size_t)source_y * row_stride;

        for (int x = 0; x < INKSIGHT_WIDTH; x++) {
            bool black = false;
            if (bits_per_pixel == 1) {
                uint8_t palette_index =
                    (row[x / 8] >> (7 - (x % 8))) & 1U;
                if (has_palette) {
                    const uint8_t *color = palette + palette_index * 4U;
                    black = rgb_is_black(color[2], color[1], color[0]);
                } else {
                    black = palette_index == 0;
                }
            } else if (bits_per_pixel == 8) {
                uint8_t palette_index = row[x];
                if (has_palette &&
                    palette_offset + (size_t)palette_index * 4U + 3U <
                        pixel_offset) {
                    const uint8_t *color =
                        palette + (size_t)palette_index * 4U;
                    black = rgb_is_black(color[2], color[1], color[0]);
                } else {
                    black = palette_index < 128;
                }
            } else {
                int bytes_per_pixel = bits_per_pixel / 8;
                const uint8_t *color = row + x * bytes_per_pixel;
                black = rgb_is_black(color[2], color[1], color[0]);
            }

            if (black) {
                frame_set_black(frame, x, destination_y);
            }
        }
    }

    ESP_LOGI(
        TAG,
        "Decoded %ldx%ld %u-bpp BMP",
        (long)width,
        (long)(signed_height < 0 ? -signed_height : signed_height),
        bits_per_pixel
    );
    return ESP_OK;
}

esp_err_t backend_render(
    inksight_config_t *config,
    bool next_mode,
    float battery_voltage,
    int wifi_rssi,
    uint8_t *frame,
    backend_render_info_t *info
) {
    if (config == NULL || frame == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));

    ESP_RETURN_ON_ERROR(
        ensure_device_token(config),
        TAG,
        "Device token unavailable"
    );

    char mac[18];
    wifi_manager_mac_string(mac, sizeof(mac));
    char url[768];
    int result = snprintf(
        url,
        sizeof(url),
        "%s/api/render?v=%.2f&mac=%s&rssi=%d&refresh_min=%d"
        "&w=%d&h=%d&bpp=1&colors=2%s",
        config->server,
        battery_voltage,
        mac,
        wifi_rssi,
        config->sleep_minutes,
        INKSIGHT_WIDTH,
        INKSIGHT_HEIGHT,
        next_mode ? "&next=1" : ""
    );
    if (result < 0 || (size_t)result >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        http_response_t response;
        int status_code = 0;
        ESP_LOGI(TAG, "GET %s", url);
        esp_err_t err = perform_request(
            url,
            HTTP_METHOD_GET,
            config->device_token,
            NULL,
            &response,
            &status_code
        );
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Render request failed: %s", esp_err_to_name(err));
            response_release(&response);
            return err;
        }

        if (status_code == 401 && attempt == 0) {
            response_release(&response);
            config->device_token[0] = '\0';
            config_store_clear_device_token();
            ESP_RETURN_ON_ERROR(
                ensure_device_token(config),
                TAG,
                "Token refresh failed"
            );
            continue;
        }
        if (status_code != 200) {
            ESP_LOGW(
                TAG,
                "Render endpoint returned HTTP %d (%u bytes)",
                status_code,
                (unsigned)response.body_length
            );
            response_release(&response);
            return ESP_FAIL;
        }

        info->is_fallback =
            strcmp(response.fallback_header, "1") == 0 ||
            strcasecmp(response.fallback_header, "true") == 0;
        strlcpy(
            info->mode_id,
            response.mode_id_header,
            sizeof(info->mode_id)
        );

        char *refresh_end = NULL;
        long refresh_minutes = strtol(
            response.refresh_header,
            &refresh_end,
            10
        );
        if (refresh_end != response.refresh_header &&
            *refresh_end == '\0' &&
            refresh_minutes >= INKSIGHT_MIN_SLEEP_MINUTES &&
            refresh_minutes <= INKSIGHT_MAX_SLEEP_MINUTES) {
            info->refresh_minutes = (int)refresh_minutes;
            if (config->sleep_minutes != info->refresh_minutes) {
                config->sleep_minutes = info->refresh_minutes;
                config_store_save_sleep_minutes(info->refresh_minutes);
            }
        }

        err = backend_decode_bmp(
            response.body,
            response.body_length,
            frame
        );
        response_release(&response);
        return err;
    }
    return ESP_FAIL;
}

esp_err_t backend_fetch_focus_config(
    inksight_config_t *config,
    bool *focus_listening,
    bool *always_active
) {
    if (config == NULL || focus_listening == NULL || always_active == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *focus_listening = false;
    *always_active = false;

    ESP_RETURN_ON_ERROR(
        ensure_device_token(config),
        TAG,
        "Device token unavailable for focus config check"
    );

    char mac[18];
    wifi_manager_mac_string(mac, sizeof(mac));
    char url[512];
    int result = snprintf(
        url,
        sizeof(url),
        "%s/api/config/%s",
        config->server,
        mac
    );
    if (result < 0 || (size_t)result >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        http_response_t response;
        int status_code = 0;
        esp_err_t err = perform_request(
            url,
            HTTP_METHOD_GET,
            config->device_token,
            NULL,
            &response,
            &status_code
        );
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Config request failed: %s", esp_err_to_name(err));
            response_release(&response);
            return err;
        }

        if (status_code == 401 && attempt == 0) {
            response_release(&response);
            config->device_token[0] = '\0';
            config_store_clear_device_token();
            ESP_RETURN_ON_ERROR(
                ensure_device_token(config),
                TAG,
                "Token refresh failed"
            );
            continue;
        }
        if (status_code != 200) {
            ESP_LOGW(
                TAG,
                "Config endpoint returned HTTP %d",
                status_code
            );
            response_release(&response);
            return ESP_FAIL;
        }

        // Null-terminate for JSON parsing
        if (response.body_length >= response.body_capacity) {
            response_release(&response);
            return ESP_ERR_NO_MEM;
        }
        response.body[response.body_length] = '\0';

        cJSON *root = cJSON_ParseWithLength(
            (const char *)response.body,
            response.body_length
        );
        if (root == NULL) {
            ESP_LOGW(TAG, "Config response is not valid JSON");
            response_release(&response);
            return ESP_ERR_INVALID_ARG;
        }

        const cJSON *focus_item = cJSON_GetObjectItemCaseSensitive(
            root, "is_focus_listening"
        );
        if (cJSON_IsBool(focus_item)) {
            *focus_listening = cJSON_IsTrue(focus_item);
        }

        const cJSON *active_item = cJSON_GetObjectItemCaseSensitive(
            root, "is_always_active"
        );
        if (cJSON_IsBool(active_item)) {
            *always_active = cJSON_IsTrue(active_item);
        }

        ESP_LOGI(
            TAG,
            "Focus config: focus_listening=%s always_active=%s",
            *focus_listening ? "true" : "false",
            *always_active ? "true" : "false"
        );

        cJSON_Delete(root);
        response_release(&response);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t backend_fetch_alert_bmp(
    inksight_config_t *config,
    uint8_t *frame
) {
    if (config == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(
        ensure_device_token(config),
        TAG,
        "Device token unavailable for alert check"
    );

    char mac[18];
    wifi_manager_mac_string(mac, sizeof(mac));
    char url[512];
    int result = snprintf(
        url,
        sizeof(url),
        "%s/api/device/%s/alert-bmp?w=%d&h=%d",
        config->server,
        mac,
        INKSIGHT_WIDTH,
        INKSIGHT_HEIGHT
    );
    if (result < 0 || (size_t)result >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        http_response_t response;
        int status_code = 0;
        esp_err_t err = perform_request(
            url,
            HTTP_METHOD_GET,
            config->device_token,
            NULL,
            &response,
            &status_code
        );
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Alert check failed: %s", esp_err_to_name(err));
            response_release(&response);
            return err;
        }

        if (status_code == 204) {
            // No alert pending
            response_release(&response);
            return ESP_ERR_NOT_FOUND;
        }

        if (status_code == 401 && attempt == 0) {
            response_release(&response);
            config->device_token[0] = '\0';
            config_store_clear_device_token();
            ESP_RETURN_ON_ERROR(
                ensure_device_token(config),
                TAG,
                "Token refresh failed"
            );
            continue;
        }

        if (status_code != 200) {
            ESP_LOGW(
                TAG,
                "Alert-bmp endpoint returned HTTP %d (%u bytes)",
                status_code,
                (unsigned)response.body_length
            );
            response_release(&response);
            return ESP_FAIL;
        }

        err = backend_decode_bmp(
            response.body,
            response.body_length,
            frame
        );
        response_release(&response);
        return err;
    }
    return ESP_FAIL;
}
