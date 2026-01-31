/*
 * Piped Client Module
 * YouTube audio streaming via Piped API
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "piped_client.h"
#include "audio_player.h"

static const char *TAG = "PIPED";

// ============================================
// State
// ============================================
static char api_base_url[128] = PIPED_INSTANCE_DEFAULT;
static bool initialized = false;
static SemaphoreHandle_t api_mutex = NULL;

// HTTP response buffer
#define HTTP_BUFFER_SIZE    8192
static char *http_buffer = NULL;
static int http_buffer_len = 0;

// ============================================
// HTTP helpers
// ============================================

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (http_buffer && (http_buffer_len + evt->data_len < HTTP_BUFFER_SIZE)) {
                    memcpy(http_buffer + http_buffer_len, evt->data, evt->data_len);
                    http_buffer_len += evt->data_len;
                    http_buffer[http_buffer_len] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t http_get(const char *url, char *response, size_t response_size)
{
    if (!response || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    http_buffer = response;
    http_buffer_len = 0;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "HTTP status: %d", status);
            err = ESP_ERR_HTTP_BASE + status;
        }
    }

    esp_http_client_cleanup(client);
    http_buffer = NULL;

    return err;
}

// ============================================
// JSON parsing helpers
// ============================================

static void extract_video_id(const char *url, char *video_id, size_t len)
{
    // URL format: /watch?v=VIDEO_ID or just VIDEO_ID
    video_id[0] = '\0';

    if (!url) return;

    const char *v_param = strstr(url, "v=");
    if (v_param) {
        v_param += 2;
        size_t copy_len = 11;  // YouTube video ID is 11 chars
        if (copy_len >= len) copy_len = len - 1;
        strncpy(video_id, v_param, copy_len);
        video_id[copy_len] = '\0';
    } else if (strlen(url) == 11) {
        // Might be just the ID
        strncpy(video_id, url, len - 1);
        video_id[len - 1] = '\0';
    }
}

static uint32_t parse_duration(const char *duration_str)
{
    // Parse ISO 8601 duration or just seconds
    if (!duration_str) return 0;
    return atoi(duration_str);
}

// ============================================
// Public API
// ============================================

esp_err_t piped_client_init(const char *base_url)
{
    if (initialized) {
        return ESP_OK;
    }

    if (base_url) {
        strncpy(api_base_url, base_url, sizeof(api_base_url) - 1);
        api_base_url[sizeof(api_base_url) - 1] = '\0';
    }

    api_mutex = xSemaphoreCreateMutex();
    if (!api_mutex) {
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    ESP_LOGI(TAG, "Piped client initialized with instance: %s", api_base_url);

    return ESP_OK;
}

esp_err_t piped_client_deinit(void)
{
    if (!initialized) {
        return ESP_OK;
    }

    if (api_mutex) {
        vSemaphoreDelete(api_mutex);
        api_mutex = NULL;
    }

    initialized = false;
    return ESP_OK;
}

esp_err_t piped_client_set_instance(const char *base_url)
{
    if (!base_url) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(api_mutex, portMAX_DELAY);
    strncpy(api_base_url, base_url, sizeof(api_base_url) - 1);
    api_base_url[sizeof(api_base_url) - 1] = '\0';
    xSemaphoreGive(api_mutex);

    ESP_LOGI(TAG, "Piped instance changed to: %s", api_base_url);
    return ESP_OK;
}

const char *piped_client_get_instance(void)
{
    return api_base_url;
}

esp_err_t piped_search(const char *query, const char *filter, piped_search_results_t *results)
{
    if (!initialized || !query || !results) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(results, 0, sizeof(piped_search_results_t));

    // Allocate response buffer
    char *response = malloc(HTTP_BUFFER_SIZE);
    if (!response) {
        return ESP_ERR_NO_MEM;
    }

    // Build URL with query encoding
    char url[512];
    char encoded_query[128];

    // Simple URL encoding for spaces
    int j = 0;
    for (int i = 0; query[i] && j < sizeof(encoded_query) - 3; i++) {
        if (query[i] == ' ') {
            encoded_query[j++] = '%';
            encoded_query[j++] = '2';
            encoded_query[j++] = '0';
        } else {
            encoded_query[j++] = query[i];
        }
    }
    encoded_query[j] = '\0';

    if (filter) {
        snprintf(url, sizeof(url), "%s/search?q=%s&filter=%s", api_base_url, encoded_query, filter);
    } else {
        snprintf(url, sizeof(url), "%s/search?q=%s&filter=music_songs", api_base_url, encoded_query);
    }

    ESP_LOGI(TAG, "Searching: %s", query);

    xSemaphoreTake(api_mutex, portMAX_DELAY);
    esp_err_t err = http_get(url, response, HTTP_BUFFER_SIZE);
    xSemaphoreGive(api_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Search request failed: %d", err);
        free(response);
        return err;
    }

    // Parse JSON response
    cJSON *root = cJSON_Parse(response);
    free(response);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse search response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Get items array
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!items || !cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Parse each item
    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (count >= PIPED_MAX_SEARCH_RESULTS) break;

        piped_search_item_t *result = &results->items[count];

        // Get URL and extract video ID
        cJSON *url_json = cJSON_GetObjectItem(item, "url");
        if (url_json && cJSON_IsString(url_json)) {
            extract_video_id(url_json->valuestring, result->video_id, sizeof(result->video_id));
        }

        // Skip if no valid video ID
        if (result->video_id[0] == '\0') continue;

        // Title
        cJSON *title = cJSON_GetObjectItem(item, "title");
        if (title && cJSON_IsString(title)) {
            strncpy(result->title, title->valuestring, sizeof(result->title) - 1);
        }

        // Artist/Uploader
        cJSON *uploader = cJSON_GetObjectItem(item, "uploaderName");
        if (uploader && cJSON_IsString(uploader)) {
            strncpy(result->artist, uploader->valuestring, sizeof(result->artist) - 1);
        }

        // Duration
        cJSON *duration = cJSON_GetObjectItem(item, "duration");
        if (duration && cJSON_IsNumber(duration)) {
            result->duration_seconds = duration->valueint;
        }

        // Views
        cJSON *views = cJSON_GetObjectItem(item, "views");
        if (views && cJSON_IsNumber(views)) {
            result->views = views->valueint;
        }

        // Thumbnail
        cJSON *thumb = cJSON_GetObjectItem(item, "thumbnail");
        if (thumb && cJSON_IsString(thumb)) {
            strncpy(result->thumbnail_url, thumb->valuestring, sizeof(result->thumbnail_url) - 1);
        }

        count++;
    }

    results->count = count;

    // Check for next page
    cJSON *nextpage = cJSON_GetObjectItem(root, "nextpage");
    if (nextpage && cJSON_IsString(nextpage) && strlen(nextpage->valuestring) > 0) {
        results->has_more = true;
        strncpy(results->next_page, nextpage->valuestring, sizeof(results->next_page) - 1);
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Search found %d results", count);
    return ESP_OK;
}

esp_err_t piped_get_stream(const char *video_id, piped_stream_info_t *stream)
{
    if (!initialized || !video_id || !stream) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(stream, 0, sizeof(piped_stream_info_t));
    strncpy(stream->video_id, video_id, sizeof(stream->video_id) - 1);

    // Allocate response buffer
    char *response = malloc(HTTP_BUFFER_SIZE);
    if (!response) {
        return ESP_ERR_NO_MEM;
    }

    // Build URL
    char url[256];
    snprintf(url, sizeof(url), "%s/streams/%s", api_base_url, video_id);

    ESP_LOGI(TAG, "Getting stream info for: %s", video_id);

    xSemaphoreTake(api_mutex, portMAX_DELAY);
    esp_err_t err = http_get(url, response, HTTP_BUFFER_SIZE);
    xSemaphoreGive(api_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream request failed: %d", err);
        free(response);
        return err;
    }

    // Parse JSON response
    cJSON *root = cJSON_Parse(response);
    free(response);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse stream response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Title
    cJSON *title = cJSON_GetObjectItem(root, "title");
    if (title && cJSON_IsString(title)) {
        strncpy(stream->title, title->valuestring, sizeof(stream->title) - 1);
    }

    // Uploader
    cJSON *uploader = cJSON_GetObjectItem(root, "uploader");
    if (uploader && cJSON_IsString(uploader)) {
        strncpy(stream->artist, uploader->valuestring, sizeof(stream->artist) - 1);
    }

    // Duration
    cJSON *duration = cJSON_GetObjectItem(root, "duration");
    if (duration && cJSON_IsNumber(duration)) {
        stream->duration_seconds = duration->valueint;
    }

    // Thumbnail
    cJSON *thumb = cJSON_GetObjectItem(root, "thumbnailUrl");
    if (thumb && cJSON_IsString(thumb)) {
        strncpy(stream->thumbnail_url, thumb->valuestring, sizeof(stream->thumbnail_url) - 1);
    }

    // Find best audio stream
    cJSON *audio_streams = cJSON_GetObjectItem(root, "audioStreams");
    if (audio_streams && cJSON_IsArray(audio_streams)) {
        int best_bitrate = 0;
        cJSON *audio_item;

        cJSON_ArrayForEach(audio_item, audio_streams) {
            cJSON *bitrate = cJSON_GetObjectItem(audio_item, "bitrate");
            cJSON *url_json = cJSON_GetObjectItem(audio_item, "url");
            cJSON *mime = cJSON_GetObjectItem(audio_item, "mimeType");

            if (!bitrate || !url_json) continue;

            int br = bitrate->valueint;

            // Prefer medium bitrate (128kbps is good for ESP32)
            // Too high bitrate may cause buffering issues
            if (br > best_bitrate && br <= 192000) {
                best_bitrate = br;

                strncpy(stream->audio.url, url_json->valuestring, sizeof(stream->audio.url) - 1);
                stream->audio.bitrate = br;

                if (mime && cJSON_IsString(mime)) {
                    strncpy(stream->audio.mime_type, mime->valuestring, sizeof(stream->audio.mime_type) - 1);
                }

                cJSON *quality = cJSON_GetObjectItem(audio_item, "quality");
                if (quality && cJSON_IsString(quality)) {
                    strncpy(stream->audio.quality, quality->valuestring, sizeof(stream->audio.quality) - 1);
                }

                cJSON *codec = cJSON_GetObjectItem(audio_item, "codec");
                if (codec && cJSON_IsString(codec)) {
                    strncpy(stream->audio.codec, codec->valuestring, sizeof(stream->audio.codec) - 1);
                }
            }
        }
    }

    cJSON_Delete(root);

    if (stream->audio.url[0] == '\0') {
        ESP_LOGE(TAG, "No audio stream found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Stream: %s - %s (%d kbps)",
             stream->title, stream->artist, stream->audio.bitrate / 1000);

    return ESP_OK;
}

esp_err_t piped_get_audio_url(const char *video_id, char *audio_url, size_t url_len)
{
    piped_stream_info_t stream;
    esp_err_t err = piped_get_stream(video_id, &stream);

    if (err == ESP_OK && stream.audio.url[0] != '\0') {
        strncpy(audio_url, stream.audio.url, url_len - 1);
        audio_url[url_len - 1] = '\0';
        return ESP_OK;
    }

    return err;
}

esp_err_t piped_play_search(const char *query)
{
    piped_search_results_t results;
    esp_err_t err = piped_search(query, "music_songs", &results);

    if (err != ESP_OK || results.count == 0) {
        ESP_LOGW(TAG, "No results found for: %s", query);
        return ESP_ERR_NOT_FOUND;
    }

    return piped_play_video(results.items[0].video_id);
}

esp_err_t piped_play_video(const char *video_id)
{
    piped_stream_info_t stream;
    esp_err_t err = piped_get_stream(video_id, &stream);

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Playing: %s - %s", stream.title, stream.artist);

    // Use the audio player to play the stream
    return audio_player_play_url(stream.audio.url);
}

esp_err_t piped_test_instance(const char *base_url)
{
    char url[256];
    char response[256];

    snprintf(url, sizeof(url), "%s/healthcheck", base_url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "Instance %s is working", base_url);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Instance %s failed (status: %d)", base_url, status);
    return ESP_FAIL;
}

esp_err_t piped_find_working_instance(void)
{
    const char *instances[] = {
        PIPED_INSTANCE_DEFAULT,
        PIPED_INSTANCE_BACKUP_1,
        PIPED_INSTANCE_BACKUP_2,
    };

    for (int i = 0; i < sizeof(instances) / sizeof(instances[0]); i++) {
        if (piped_test_instance(instances[i]) == ESP_OK) {
            piped_client_set_instance(instances[i]);
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "No working Piped instance found");
    return ESP_FAIL;
}
