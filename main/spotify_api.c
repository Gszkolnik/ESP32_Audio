/*
 * Spotify Web API Module
 * Handles Spotify authentication and playback control
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#include "spotify_api.h"
#include "config.h"

static const char *TAG = "SPOTIFY_API";

// Konfiguracja Spotify
static char client_id[64] = "";
static char client_secret[64] = "";
static char access_token[512] = "";
static char refresh_token[512] = "";
static time_t token_expiry = 0;

// Stan
static spotify_auth_state_t auth_state = SPOTIFY_STATE_NOT_AUTHORIZED;
static spotify_playback_state_t playback_state = {0};

// Callback
static spotify_state_callback_t state_callback = NULL;

// NVS handle
static nvs_handle_t spotify_nvs_handle;

// HTTP response buffer
static char http_response[4096];
static int http_response_len = 0;

// ============================================
// HTTP event handler
// ============================================

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_len + evt->data_len < sizeof(http_response)) {
                memcpy(http_response + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// ============================================
// Helper functions
// ============================================

static esp_err_t spotify_api_request(const char *method, const char *endpoint,
                                      const char *post_data, char *response, size_t response_size)
{
    if (strlen(access_token) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s", SPOTIFY_API_URL, endpoint);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // Ustaw metodę
    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (strcmp(method, "PUT") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
    }

    // Nagłówki
    char auth_header[600];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", access_token);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Dane POST
    if (post_data) {
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    // Reset bufora odpowiedzi
    http_response_len = 0;
    memset(http_response, 0, sizeof(http_response));

    // Wykonaj request
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status_code >= 400) {
        ESP_LOGE(TAG, "Spotify API error: %d", status_code);
        return ESP_FAIL;
    }

    // Kopiuj odpowiedź
    if (response && response_size > 0) {
        strncpy(response, http_response, response_size - 1);
    }

    return ESP_OK;
}

// ============================================
// Publiczne API
// ============================================

esp_err_t spotify_api_init(const char *id, const char *secret)
{
    ESP_LOGI(TAG, "Initializing Spotify API...");

    strncpy(client_id, id, sizeof(client_id) - 1);
    strncpy(client_secret, secret, sizeof(client_secret) - 1);

    // Otwarcie NVS
    esp_err_t ret = nvs_open("spotify", NVS_READWRITE, &spotify_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return ret;
    }

    ESP_LOGI(TAG, "Spotify API initialized");
    return ESP_OK;
}

const char *spotify_api_get_auth_url(void)
{
    static char auth_url[512];

    // PKCE flow - generuj code_verifier i code_challenge
    // W rzeczywistej implementacji należy użyć bezpiecznego generatora

    snprintf(auth_url, sizeof(auth_url),
        "%s?client_id=%s&response_type=code&redirect_uri=http://%%s/callback"
        "&scope=user-read-playback-state%%20user-modify-playback-state"
        "%%20user-read-currently-playing",
        SPOTIFY_AUTH_URL, client_id);

    auth_state = SPOTIFY_STATE_AUTHORIZING;
    return auth_url;
}

esp_err_t spotify_api_handle_callback(const char *code)
{
    ESP_LOGI(TAG, "Handling OAuth callback...");

    // Wymień code na access_token
    char post_data[512];
    snprintf(post_data, sizeof(post_data),
        "grant_type=authorization_code&code=%s&redirect_uri=http://callback"
        "&client_id=%s&client_secret=%s",
        code, client_id, client_secret);

    esp_http_client_config_t config = {
        .url = SPOTIFY_TOKEN_URL,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    http_response_len = 0;
    memset(http_response, 0, sizeof(http_response));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        auth_state = SPOTIFY_STATE_ERROR;
        return err;
    }

    // Parsuj odpowiedź
    cJSON *root = cJSON_Parse(http_response);
    if (root == NULL) {
        auth_state = SPOTIFY_STATE_ERROR;
        return ESP_FAIL;
    }

    cJSON *access = cJSON_GetObjectItem(root, "access_token");
    cJSON *refresh = cJSON_GetObjectItem(root, "refresh_token");
    cJSON *expires = cJSON_GetObjectItem(root, "expires_in");

    if (access && cJSON_IsString(access)) {
        strncpy(access_token, access->valuestring, sizeof(access_token) - 1);
    }
    if (refresh && cJSON_IsString(refresh)) {
        strncpy(refresh_token, refresh->valuestring, sizeof(refresh_token) - 1);
    }
    if (expires && cJSON_IsNumber(expires)) {
        token_expiry = time(NULL) + expires->valueint - 60;  // 60s marginesu
    }

    cJSON_Delete(root);

    if (strlen(access_token) > 0) {
        auth_state = SPOTIFY_STATE_AUTHORIZED;
        spotify_api_save_tokens();
        ESP_LOGI(TAG, "Spotify authorized successfully");
        return ESP_OK;
    }

    auth_state = SPOTIFY_STATE_ERROR;
    return ESP_FAIL;
}

esp_err_t spotify_api_refresh_token(void)
{
    if (strlen(refresh_token) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Refreshing Spotify token...");

    char post_data[1024];
    snprintf(post_data, sizeof(post_data),
        "grant_type=refresh_token&refresh_token=%s&client_id=%s&client_secret=%s",
        refresh_token, client_id, client_secret);

    esp_http_client_config_t config = {
        .url = SPOTIFY_TOKEN_URL,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    http_response_len = 0;
    memset(http_response, 0, sizeof(http_response));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(http_response);
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *access = cJSON_GetObjectItem(root, "access_token");
    cJSON *expires = cJSON_GetObjectItem(root, "expires_in");

    if (access && cJSON_IsString(access)) {
        strncpy(access_token, access->valuestring, sizeof(access_token) - 1);
    }
    if (expires && cJSON_IsNumber(expires)) {
        token_expiry = time(NULL) + expires->valueint - 60;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Token refreshed");
    return ESP_OK;
}

spotify_auth_state_t spotify_api_get_auth_state(void)
{
    // Sprawdź czy token wygasł
    if (auth_state == SPOTIFY_STATE_AUTHORIZED && time(NULL) > token_expiry) {
        if (spotify_api_refresh_token() != ESP_OK) {
            auth_state = SPOTIFY_STATE_NOT_AUTHORIZED;
        }
    }

    return auth_state;
}

esp_err_t spotify_api_play(void)
{
    return spotify_api_request("PUT", "/me/player/play", NULL, NULL, 0);
}

esp_err_t spotify_api_pause(void)
{
    return spotify_api_request("PUT", "/me/player/pause", NULL, NULL, 0);
}

esp_err_t spotify_api_next(void)
{
    return spotify_api_request("POST", "/me/player/next", NULL, NULL, 0);
}

esp_err_t spotify_api_previous(void)
{
    return spotify_api_request("POST", "/me/player/previous", NULL, NULL, 0);
}

esp_err_t spotify_api_seek(int position_ms)
{
    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "/me/player/seek?position_ms=%d", position_ms);
    return spotify_api_request("PUT", endpoint, NULL, NULL, 0);
}

esp_err_t spotify_api_set_volume(int volume_percent)
{
    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "/me/player/volume?volume_percent=%d", volume_percent);
    return spotify_api_request("PUT", endpoint, NULL, NULL, 0);
}

esp_err_t spotify_api_transfer_playback(const char *device_id)
{
    char post_data[128];
    snprintf(post_data, sizeof(post_data), "{\"device_ids\":[\"%s\"]}", device_id);
    return spotify_api_request("PUT", "/me/player", post_data, NULL, 0);
}

esp_err_t spotify_api_get_playback_state(spotify_playback_state_t *state)
{
    char response[2048];
    esp_err_t err = spotify_api_request("GET", "/me/player", NULL, response, sizeof(response));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *is_playing = cJSON_GetObjectItem(root, "is_playing");
    cJSON *progress = cJSON_GetObjectItem(root, "progress_ms");
    cJSON *item = cJSON_GetObjectItem(root, "item");
    cJSON *device = cJSON_GetObjectItem(root, "device");

    if (is_playing) {
        state->is_playing = cJSON_IsTrue(is_playing);
    }
    if (progress) {
        state->progress_ms = progress->valueint;
    }

    if (item) {
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *duration = cJSON_GetObjectItem(item, "duration_ms");
        cJSON *artists = cJSON_GetObjectItem(item, "artists");
        cJSON *album = cJSON_GetObjectItem(item, "album");

        if (name && cJSON_IsString(name)) {
            strncpy(state->track_name, name->valuestring, sizeof(state->track_name) - 1);
        }
        if (duration) {
            state->duration_ms = duration->valueint;
        }
        if (artists && cJSON_IsArray(artists)) {
            cJSON *first_artist = cJSON_GetArrayItem(artists, 0);
            if (first_artist) {
                cJSON *artist_name = cJSON_GetObjectItem(first_artist, "name");
                if (artist_name && cJSON_IsString(artist_name)) {
                    strncpy(state->artist_name, artist_name->valuestring, sizeof(state->artist_name) - 1);
                }
            }
        }
        if (album) {
            cJSON *album_name = cJSON_GetObjectItem(album, "name");
            cJSON *images = cJSON_GetObjectItem(album, "images");
            if (album_name && cJSON_IsString(album_name)) {
                strncpy(state->album_name, album_name->valuestring, sizeof(state->album_name) - 1);
            }
            if (images && cJSON_IsArray(images)) {
                cJSON *first_image = cJSON_GetArrayItem(images, 0);
                if (first_image) {
                    cJSON *url = cJSON_GetObjectItem(first_image, "url");
                    if (url && cJSON_IsString(url)) {
                        strncpy(state->album_art_url, url->valuestring, sizeof(state->album_art_url) - 1);
                    }
                }
            }
        }
    }

    if (device) {
        cJSON *volume = cJSON_GetObjectItem(device, "volume_percent");
        cJSON *id = cJSON_GetObjectItem(device, "id");
        if (volume) {
            state->volume_percent = volume->valueint;
        }
        if (id && cJSON_IsString(id)) {
            strncpy(state->device_id, id->valuestring, sizeof(state->device_id) - 1);
        }
    }

    cJSON_Delete(root);
    memcpy(&playback_state, state, sizeof(spotify_playback_state_t));

    return ESP_OK;
}

esp_err_t spotify_api_get_devices(char ***devices, uint8_t *count)
{
    // TODO: Implementacja
    *devices = NULL;
    *count = 0;
    return ESP_OK;
}

esp_err_t spotify_api_play_uri(const char *spotify_uri)
{
    char post_data[256];
    snprintf(post_data, sizeof(post_data), "{\"uris\":[\"%s\"]}", spotify_uri);
    return spotify_api_request("PUT", "/me/player/play", post_data, NULL, 0);
}

esp_err_t spotify_api_play_playlist(const char *playlist_id)
{
    char post_data[256];
    snprintf(post_data, sizeof(post_data),
        "{\"context_uri\":\"spotify:playlist:%s\"}", playlist_id);
    return spotify_api_request("PUT", "/me/player/play", post_data, NULL, 0);
}

void spotify_api_register_callback(spotify_state_callback_t callback)
{
    state_callback = callback;
}

esp_err_t spotify_api_save_tokens(void)
{
    nvs_set_str(spotify_nvs_handle, "access_token", access_token);
    nvs_set_str(spotify_nvs_handle, "refresh_token", refresh_token);
    return nvs_commit(spotify_nvs_handle);
}

esp_err_t spotify_api_load_tokens(void)
{
    size_t size = sizeof(access_token);
    nvs_get_str(spotify_nvs_handle, "access_token", access_token, &size);

    size = sizeof(refresh_token);
    nvs_get_str(spotify_nvs_handle, "refresh_token", refresh_token, &size);

    if (strlen(refresh_token) > 0) {
        return spotify_api_refresh_token();
    }

    return ESP_OK;
}
