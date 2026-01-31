#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "esp_err.h"
#include "audio_pipeline.h"
#include "audio_element.h"

// Typy źródeł audio
typedef enum {
    AUDIO_SOURCE_NONE = 0,
    AUDIO_SOURCE_HTTP,      // Radio internetowe
    AUDIO_SOURCE_SDCARD,    // Pliki z karty SD
    AUDIO_SOURCE_BLUETOOTH, // Bluetooth A2DP
    AUDIO_SOURCE_AUX,       // Wejście AUX
} audio_source_t;

// Stan odtwarzacza
typedef enum {
    PLAYER_STATE_IDLE = 0,
    PLAYER_STATE_BUFFERING,  // Pre-buffering before playback
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_ERROR,
} player_state_t;

// Struktura stanu odtwarzacza
typedef struct {
    player_state_t state;
    audio_source_t source;
    int volume;
    bool muted;
    char current_url[512];
    char current_title[128];
    char current_artist[128];
} player_status_t;

// Callback dla zmiany stanu
typedef void (*player_state_callback_t)(player_status_t *status);

// Inicjalizacja i deinicjalizacja
esp_err_t audio_player_init(void);
esp_err_t audio_player_deinit(void);

// Sterowanie odtwarzaniem
esp_err_t audio_player_play_url(const char *url);
esp_err_t audio_player_play_sdcard(const char *filepath);
esp_err_t audio_player_play_next_station(void);  // Play next station from list
esp_err_t audio_player_stop(void);
esp_err_t audio_player_pause(void);
esp_err_t audio_player_resume(void);

// Sterowanie głośnością
esp_err_t audio_player_set_volume(int volume);
int audio_player_get_volume(void);
esp_err_t audio_player_mute(bool mute);

// Stan odtwarzacza
player_status_t *audio_player_get_status(void);
void audio_player_register_callback(player_state_callback_t callback);

// Buffer monitoring
int audio_player_get_buffer_level(void);  // Returns 0-100%

// Equalizer control
esp_err_t audio_player_set_eq_band(int band, int gain_db);
esp_err_t audio_player_set_eq_all_bands(const int *gains_db);
audio_element_handle_t audio_player_get_equalizer(void);

#endif // AUDIO_PLAYER_H
