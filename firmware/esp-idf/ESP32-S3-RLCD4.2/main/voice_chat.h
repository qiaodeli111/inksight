#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start AI voice conversation
 * Connects WebSocket, starts recording, plays response
 */
esp_err_t voice_chat_start(void);

/**
 * Stop voice conversation
 */
void voice_chat_stop(void);

/**
 * Check if voice chat is active
 */
bool voice_chat_is_active(void);

/**
 * Set connection config (server, token, mac)
 */
void voice_chat_set_config(const char *server, const char *token, const char *mac);

/**
 * Voice chat task - call periodically from main loop
 */
void voice_chat_loop(void);

#ifdef __cplusplus
}
#endif
