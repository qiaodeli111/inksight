#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "backend.h"
#include "battery.h"
#include "board_config.h"
#include "config_store.h"
#include "portal.h"
#include "st7305.h"
#include "ui.h"
#include "audio.h"
#include "voice_chat.h"
#include "wifi_manager.h"

#include "esp_timer.h"

static const char *TAG = "inksight";
static uint8_t s_frame[INKSIGHT_FRAME_BYTES];
static inksight_config_t s_config;

static const uint32_t ALERT_POLL_MS = 10000;    // Poll for alerts every 10s
static const uint32_t FOCUS_RECHECK_MS = 60000;  // Re-check focus flag every 60s
static const uint32_t LONG_PRESS_MS = 1500;      // Button held this long → provisioning
static const uint32_t TICK_MS = 50;               // Main loop tick for responsive buttons

static void initialize_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void initialize_key(void) {
    gpio_config_t key_config = {
        .pin_bit_mask = (1ULL << INKSIGHT_KEY_GPIO) |
                        (1ULL << INKSIGHT_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&key_config));

    // GPIO 0 may be held from deep sleep; release it
    gpio_hold_dis(INKSIGHT_BOOT_GPIO);
}

static bool key_held_for(uint32_t duration_ms) {
    if (gpio_get_level(INKSIGHT_KEY_GPIO) != 0) {
        return false;
    }
    uint32_t elapsed = 0;
    while (elapsed < duration_ms) {
        if (gpio_get_level(INKSIGHT_KEY_GPIO) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        elapsed += 20;
    }
    return true;
}

/*
 * Check if the device button is being pressed *right now* (no debounce).
 * Used to break out of the focus listening loop.
 */
static bool key_pressed_now(void) {
    return gpio_get_level(INKSIGHT_KEY_GPIO) == 0;
}

static void run_provisioning(void) {
    char ap_name[33];
    ESP_ERROR_CHECK(
        wifi_manager_start_provisioning_ap(ap_name, sizeof(ap_name))
    );
    ui_draw_setup(s_frame, ap_name);
    ESP_ERROR_CHECK(st7305_display(s_frame));
    ESP_ERROR_CHECK(portal_start(&s_config, ap_name));

    ESP_LOGI(TAG, "Provisioning active; connect to %s", ap_name);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void wait_for_key_release(void) {
    uint32_t waited_ms = 0;
    while (gpio_get_level(INKSIGHT_KEY_GPIO) == 0 && waited_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
}

static bool s_focus_listening = false;
static bool s_always_active = false;

static void enter_deep_sleep(int sleep_minutes, bool force) {
    if (!force && (s_focus_listening || s_always_active)) {
        if (s_focus_listening) {
            ESP_LOGI(TAG, "Focus listening enabled, skipping deep sleep");
        } else {
            ESP_LOGI(TAG, "Always active enabled, skipping deep sleep");
        }
        return;
    }

    if (sleep_minutes < INKSIGHT_MIN_SLEEP_MINUTES ||
        sleep_minutes > INKSIGHT_MAX_SLEEP_MINUTES) {
        sleep_minutes = INKSIGHT_DEFAULT_SLEEP_MINUTES;
    }

    wifi_manager_stop();
    wait_for_key_release();

    ESP_ERROR_CHECK(
        esp_sleep_enable_timer_wakeup(
            (uint64_t)sleep_minutes * 60ULL * 1000000ULL
        )
    );
    ESP_ERROR_CHECK(
        esp_sleep_enable_ext1_wakeup_io(
            1ULL << INKSIGHT_KEY_GPIO,
            ESP_EXT1_WAKEUP_ANY_LOW
        )
    );
    gpio_pullup_en(INKSIGHT_KEY_GPIO);
    gpio_pulldown_dis(INKSIGHT_KEY_GPIO);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    st7305_prepare_for_deep_sleep();
    ESP_LOGI(
        TAG,
        "Deep sleep for %d minutes; RLCD low-power scan remains active",
        sleep_minutes
    );
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

void app_main(void) {
    ESP_LOGI(
        TAG,
        "InkSight ESP-IDF %s for Waveshare ESP32-S3-RLCD-4.2",
        INKSIGHT_IDF_VERSION
    );
    ESP_LOGI(
        TAG,
        "Free heap=%u, PSRAM=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );

    initialize_nvs();
    initialize_key();
    ESP_ERROR_CHECK(config_store_load(&s_config));
    ESP_ERROR_CHECK(st7305_init());
    ESP_ERROR_CHECK(wifi_manager_init());

    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    bool key_wakeup = wake_cause == ESP_SLEEP_WAKEUP_EXT1;
    bool force_provisioning =
        key_held_for(key_wakeup ? 1500 : 500);
    bool next_mode = key_wakeup && !force_provisioning;

    if (force_provisioning || !config_store_is_ready(&s_config)) {
        run_provisioning();
    }

    if (!wifi_manager_connect(&s_config)) {
        ESP_LOGW(TAG, "Saved Wi-Fi unavailable; opening provisioning");
        run_provisioning();
    }

    // Audio init deferred to BOOT button press (voice_chat_start calls audio_init)
    ESP_LOGI(TAG, "Audio deferred to BOOT button");

    // Configure voice chat (needs device token from config)
    {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        voice_chat_set_config(
            s_config.server[0] ? s_config.server : "https://www.inksight.site",
            s_config.device_token,
            mac_str);
    }

    float battery_voltage = battery_read_voltage();
    ESP_LOGI(
        TAG,
        "Main stack free before backend=%u bytes",
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );
    backend_render_info_t render_info;
    esp_err_t render_error = backend_render(
        &s_config,
        next_mode,
        battery_voltage,
        wifi_manager_rssi(),
        s_frame,
        &render_info
    );
    ESP_LOGI(
        TAG,
        "Main stack minimum free after backend=%u bytes",
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );

    if (render_error == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Rendered mode=%s fallback=%s refresh=%d",
            render_info.mode_id[0] != '\0' ? render_info.mode_id : "(unknown)",
            render_info.is_fallback ? "yes" : "no",
            render_info.refresh_minutes
        );
        ESP_ERROR_CHECK(st7305_display(s_frame));
    } else {
        ESP_LOGE(
            TAG,
            "Render failed: %s",
            esp_err_to_name(render_error)
        );
        char error_line[32];
        snprintf(
            error_line,
            sizeof(error_line),
            "ERROR %s",
            esp_err_to_name(render_error)
        );
        ui_draw_status(
            s_frame,
            "FETCH FAILED",
            "CHECK WIFI SERVER",
            error_line,
            "HOLD KEY FOR SETUP"
        );
        ESP_ERROR_CHECK(st7305_display(s_frame));
    }

    // ── Fetch focus config flags from server ──
    bool focus_listening = false;
    bool always_active = false;
    esp_err_t focus_err = backend_fetch_focus_config(
        &s_config, &focus_listening, &always_active
    );
    if (focus_err != ESP_OK) {
        focus_listening = false;
        always_active = false;
    }
    s_focus_listening = focus_listening;
    s_always_active = always_active;

    if (!focus_listening && !always_active) {
        enter_deep_sleep(s_config.sleep_minutes, false);
        // Never returns; esp_deep_sleep_start() resets the chip.
        return;
    }

    // ── Focus listening / always active loop ──
    ESP_LOGI(TAG, "Entering active loop (focus=%s active=%s)",
             (focus_listening ? "yes" : "no"),
             (always_active ? "yes" : "no"));

    // Allocate backup frame from PSRAM for alert overlay
    uint8_t *backup_frame = heap_caps_malloc(
        INKSIGHT_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (backup_frame == NULL) {
        // Fall back to internal RAM
        backup_frame = heap_caps_malloc(
            INKSIGHT_FRAME_BYTES, MALLOC_CAP_8BIT
        );
    }
    bool have_backup = false;
    bool alert_visible = false;
    uint64_t last_alert_poll_us = 0;
    uint64_t last_focus_check_us = esp_timer_get_time();

    while (s_focus_listening || s_always_active) {
        uint64_t now_us = esp_timer_get_time();

        // ── Periodically re-check focus config from server ──
        if (focus_listening &&
            (now_us - last_focus_check_us) >= (FOCUS_RECHECK_MS * 1000ULL)) {
            backend_fetch_focus_config(
                &s_config, &focus_listening, &always_active
            );
            s_focus_listening = focus_listening;
            s_always_active = always_active;
            last_focus_check_us = now_us;
        }

        // ── Poll for alerts (only when no alert is currently shown) ──
        if (!alert_visible && focus_listening &&
            (now_us - last_alert_poll_us) >= (ALERT_POLL_MS * 1000ULL)) {
            last_alert_poll_us = now_us;
            esp_err_t alert_err = backend_fetch_alert_bmp(&s_config, s_frame);
            if (alert_err == ESP_OK) {
                // Save current content, show alert
                if (!have_backup && backup_frame != NULL) {
                    memcpy(backup_frame, s_frame, INKSIGHT_FRAME_BYTES);
                    have_backup = true;
                }
                ESP_LOGI(TAG, "Alert received, displaying");
                st7305_display(s_frame);
                audio_beep(2000, 300);  // Beep for alert
                alert_visible = true;
            }
        }

        // ── Button handling ──
        // KEY (GPIO 18) pressed?
        if (gpio_get_level(INKSIGHT_KEY_GPIO) == 0) {
            if (alert_visible) {
                // ── Dismiss alert immediately ──
                ESP_LOGI(TAG, "Alert dismissed by button press");
                if (have_backup && backup_frame != NULL) {
                    memcpy(s_frame, backup_frame, INKSIGHT_FRAME_BYTES);
                    st7305_display(s_frame);
                    have_backup = false;
                }
                alert_visible = false;
                // Wait for release so the press isn't mistaken for "next mode"
                while (gpio_get_level(INKSIGHT_KEY_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
                }
                vTaskDelay(pdMS_TO_TICKS(TICK_MS)); // debounce
            } else {
                // ── Detect short vs long press for next mode / provisioning ──
                uint32_t hold_ms = 0;
                bool released = false;
                while (hold_ms < LONG_PRESS_MS) {
                    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
                    hold_ms += TICK_MS;
                    if (gpio_get_level(INKSIGHT_KEY_GPIO) != 0) {
                        released = true;
                        break;
                    }
                }

                if (!released) {
                    // Long press → enter provisioning directly
                    ESP_LOGI(TAG, "Key held %ums, entering provisioning", hold_ms);
                    if (backup_frame != NULL) free(backup_frame);
                    wifi_manager_stop();
                    run_provisioning();
                    // Never returns; provisioning runs forever.
                } else {
                    // Short press → next mode
                    ESP_LOGI(TAG, "Short press, fetching next mode");
                    vTaskDelay(pdMS_TO_TICKS(TICK_MS)); // debounce

                    // Save current content as backup
                    if (backup_frame != NULL) {
                        memcpy(backup_frame, s_frame, INKSIGHT_FRAME_BYTES);
                    }

                    // Ensure WiFi is connected
                    if (!wifi_manager_is_connected()) {
                        wifi_manager_connect(&s_config);
                    }

                    if (wifi_manager_is_connected()) {
                        float v = battery_read_voltage();
                        backend_render_info_t ri;
                        esp_err_t err = backend_render(
                            &s_config, true, v,
                            wifi_manager_rssi(),
                            s_frame, &ri
                        );
                        if (err == ESP_OK) {
                            ESP_LOGI(TAG, "Next mode: %s",
                                     ri.mode_id[0] ? ri.mode_id : "(unknown)");
                            st7305_display(s_frame);
                        } else {
                            ESP_LOGW(TAG, "Next mode render failed, restoring");
                            if (backup_frame != NULL) {
                                memcpy(s_frame, backup_frame, INKSIGHT_FRAME_BYTES);
                                st7305_display(s_frame);
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "WiFi unavailable for next mode");
                        if (backup_frame != NULL) {
                            memcpy(s_frame, backup_frame, INKSIGHT_FRAME_BYTES);
                            st7305_display(s_frame);
                        }
                    }

                    // Reset backup flag (we consumed it or overwrote content)
                    have_backup = false;
                }
            }
        }

        // ── BOOT button (GPIO 0) → toggle AI voice chat ──
        if (gpio_get_level(INKSIGHT_BOOT_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // debounce
            if (gpio_get_level(INKSIGHT_BOOT_GPIO) == 0) {
                ESP_LOGI(TAG, "BOOT button pressed, toggling voice chat");
                if (voice_chat_is_active()) {
                    voice_chat_stop();
                } else {
                    voice_chat_start();
                }
                // Wait for release
                while (gpio_get_level(INKSIGHT_BOOT_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
                }
            }
        }

        // ── Voice chat loop (stream mic → ws → speaker) ──
        voice_chat_loop();

        // Small delay for responsive button handling
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }

    if (backup_frame != NULL) {
        free(backup_frame);
    }

    enter_deep_sleep(s_config.sleep_minutes, false);
}
