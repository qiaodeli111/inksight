#include "voice_chat.h"
#include "audio.h"
#include "board_config.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_transport.h>
#include <esp_transport_ws.h>
#include <esp_transport_ssl.h>
#include <cJSON.h>
#include <string.h>

static const char *TAG = "voice_chat";

#define SAMPLE_RATE 24000
#define CHUNK_MS 100
#define CHUNK_SAMPLES (SAMPLE_RATE * CHUNK_MS / 1000)

static esp_transport_handle_t s_ws = NULL;
static bool s_active = false;
static char s_mac[18];
static char s_token[256];

void voice_chat_set_config(const char *server, const char *token, const char *mac) {
    (void)server;
    strncpy(s_token, token, sizeof(s_token) - 1);
    strncpy(s_mac, mac, sizeof(s_mac) - 1);
}

void voice_chat_stop(void) {
    if (!s_active) return;
    s_active = false;
    audio_record_stop();
    audio_playback_stop();
    if (s_ws) {
        esp_transport_close(s_ws);
        esp_transport_destroy(s_ws);
        s_ws = NULL;
    }
    ESP_LOGI(TAG, "Voice chat stopped");
}

esp_err_t voice_chat_start(void) {
    if (s_active) voice_chat_stop();

    esp_err_t err = audio_init();
    if (err != ESP_OK) return err;

    // Create SSL transport
    esp_transport_handle_t ssl = esp_transport_ssl_init();
    if (!ssl) return ESP_FAIL;

    // Wrap with WebSocket
    s_ws = esp_transport_ws_init(ssl);
    if (!s_ws) {
        esp_transport_destroy(ssl);
        return ESP_FAIL;
    }

    // Set WebSocket path & headers
    char path[512];
    snprintf(path, sizeof(path), "/api/device/%s/voice/ws?token=%s", s_mac, s_token);
    esp_transport_ws_set_path(s_ws, path);

    // Connect (will do TLS + WS upgrade)
    if (esp_transport_connect(s_ws, "www.inksight.site", 443, 10000) < 0) {
        ESP_LOGE(TAG, "WS connect failed");
        voice_chat_stop();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WebSocket connected");

    // Send session.start
    cJSON *start = cJSON_CreateObject();
    cJSON_AddStringToObject(start, "type", "session.start");
    cJSON_AddNumberToObject(start, "sample_rate", SAMPLE_RATE);
    cJSON_AddNumberToObject(start, "w", 400);
    cJSON_AddNumberToObject(start, "h", 300);
    cJSON_AddBoolToObject(start, "include_image", false);
    cJSON_AddNumberToObject(start, "protocol", 2);
    cJSON_AddStringToObject(start, "audio_codec", "pcm");
    char *json = cJSON_PrintUnformatted(start);
    cJSON_Delete(start);

    int len = strlen(json);
    esp_transport_ws_send_raw(s_ws, WS_TRANSPORT_OPCODES_TEXT,
                               json, len, 5000);
    ESP_LOGI(TAG, "Sent: %s", json);
    free(json);

    // Start audio
    audio_record_start(SAMPLE_RATE);
    audio_playback_start(SAMPLE_RATE);
    s_active = true;
    ESP_LOGI(TAG, "Voice chat started");
    return ESP_OK;
}

bool voice_chat_is_active(void) { return s_active; }

void voice_chat_loop(void) {
    if (!s_active || !s_ws) return;

    int16_t buf[CHUNK_SAMPLES];
    int samples = audio_read(buf, CHUNK_SAMPLES);
    if (samples > 0) {
        esp_transport_ws_send_raw(s_ws, WS_TRANSPORT_OPCODES_BINARY,
                                  (const char *)buf,
                                  samples * (int)sizeof(int16_t), 100);
    }

    // Check for incoming audio
    char inbuf[4096];
    int inlen = esp_transport_read(s_ws, inbuf, sizeof(inbuf), 100);
    if (inlen > 0) {
        int opcode = esp_transport_ws_get_read_opcode(s_ws);
        if ((opcode & 0x0F) == WS_TRANSPORT_OPCODES_BINARY) {
            audio_write((const int16_t *)inbuf, inlen / (int)sizeof(int16_t));
        }
        if ((opcode & 0x0F) == WS_TRANSPORT_OPCODES_TEXT) {
            inbuf[inlen] = 0;
            cJSON *root = cJSON_Parse((const char *)inbuf);
            if (root) {
                cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
                if (type && cJSON_IsString(type) &&
                    strcmp(type->valuestring, "session.ended") == 0) {
                    voice_chat_stop();
                }
                cJSON_Delete(root);
            }
        }
    }
}
