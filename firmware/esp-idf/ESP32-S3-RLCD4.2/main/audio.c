#include "audio.h"
#include <esp_log.h>
static const char *TAG = "audio";
esp_err_t audio_init(void) { ESP_LOGW(TAG, "stub: audio not available"); return ESP_FAIL; }
esp_err_t audio_playback_start(int s) { return ESP_FAIL; }
void audio_playback_stop(void) {}
int audio_write(const int16_t *d, int l) { return 0; }
esp_err_t audio_record_start(int s) { return ESP_FAIL; }
void audio_record_stop(void) {}
int audio_read(int16_t *d, int l) { return 0; }
void audio_set_volume(int v) {}
void audio_beep(int f, int d) {}
