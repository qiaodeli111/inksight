#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t audio_init(void);
esp_err_t audio_playback_start(int s);
void audio_playback_stop(void);
int audio_write(const int16_t *d, int l);
esp_err_t audio_record_start(int s);
void audio_record_stop(void);
int audio_read(int16_t *d, int l);
void audio_set_volume(int v);
void audio_beep(int freq_hz, int duration_ms);
#ifdef __cplusplus
}
#endif
