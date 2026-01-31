#ifndef SPOTIFY_API_H
#define SPOTIFY_API_H

#include "esp_err.h"

// Stan autoryzacji Spotify
typedef enum {
    SPOTIFY_STATE_NOT_AUTHORIZED = 0,
    SPOTIFY_STATE_AUTHORIZING,
    SPOTIFY_STATE_AUTHORIZED,
    SPOTIFY_STATE_ERROR,
} spotify_auth_state_t;

// Stan odtwarzania Spotify
typedef struct {
    bool is_playing;
    char track_name[128];
    char artist_name[128];
    char album_name[128];
    char album_art_url[256];
    int duration_ms;
    int progress_ms;
    int volume_percent;
    char device_id[64];
} spotify_playback_state_t;

// Callback dla zmiany stanu
typedef void (*spotify_state_callback_t)(spotify_playback_state_t *state);

// Inicjalizacja
esp_err_t spotify_api_init(const char *client_id, const char *client_secret);

// Autoryzacja OAuth2 (PKCE flow dla urządzeń)
const char *spotify_api_get_auth_url(void);
esp_err_t spotify_api_handle_callback(const char *code);
esp_err_t spotify_api_refresh_token(void);
spotify_auth_state_t spotify_api_get_auth_state(void);

// Sterowanie odtwarzaniem (telefon jako źródło)
esp_err_t spotify_api_play(void);
esp_err_t spotify_api_pause(void);
esp_err_t spotify_api_next(void);
esp_err_t spotify_api_previous(void);
esp_err_t spotify_api_seek(int position_ms);
esp_err_t spotify_api_set_volume(int volume_percent);

// Transfer odtwarzania na ESP32 (jako głośnik)
esp_err_t spotify_api_transfer_playback(const char *device_id);

// Pobieranie stanu
esp_err_t spotify_api_get_playback_state(spotify_playback_state_t *state);
esp_err_t spotify_api_get_devices(char ***devices, uint8_t *count);

// Odtwarzanie playlisty/albumu/utworu
esp_err_t spotify_api_play_uri(const char *spotify_uri);
esp_err_t spotify_api_play_playlist(const char *playlist_id);

// Callback
void spotify_api_register_callback(spotify_state_callback_t callback);

// Zapisywanie tokenów
esp_err_t spotify_api_save_tokens(void);
esp_err_t spotify_api_load_tokens(void);

#endif // SPOTIFY_API_H
