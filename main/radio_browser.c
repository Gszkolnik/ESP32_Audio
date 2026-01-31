/*
 * Radio Browser API Module
 * Integration with radio-browser.info API for searching radio stations
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "radio_browser.h"

static const char *TAG = "RADIO_BROWSER";

// RadioBrowser API base URL (jeden z wielu serwerow)
#define RADIO_BROWSER_API_BASE "http://de1.api.radio-browser.info/json"

// Bufor na odpowiedz HTTP (w PSRAM)
#define HTTP_RESPONSE_BUFFER_SIZE 32768  // 32KB - enough for ~20 stations

// Struktura do przechowywania odpowiedzi HTTP
typedef struct {
    char *buffer;
    int len;
    int max_len;
} http_response_t;

// Callback dla ESP HTTP Client
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (response && response->buffer) {
                int copy_len = evt->data_len;
                if (response->len + copy_len >= response->max_len) {
                    copy_len = response->max_len - response->len - 1;
                }
                if (copy_len > 0) {
                    memcpy(response->buffer + response->len, evt->data, copy_len);
                    response->len += copy_len;
                    response->buffer[response->len] = '\0';
                }
                // Yield CPU to allow audio tasks to run
                vTaskDelay(1);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Wykonaj zapytanie HTTP i zwroc JSON
static cJSON* radio_browser_request(const char *endpoint)
{
    char url[512];
    snprintf(url, sizeof(url), "%s%s", RADIO_BROWSER_API_BASE, endpoint);

    ESP_LOGI(TAG, "Requesting: %s", url);

    // Alokacja bufora na odpowiedz w PSRAM
    http_response_t response = {
        .buffer = heap_caps_malloc(HTTP_RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM),
        .len = 0,
        .max_len = HTTP_RESPONSE_BUFFER_SIZE
    };

    if (!response.buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return NULL;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(response.buffer);
        return NULL;
    }

    // Ustaw naglowki
    esp_http_client_set_header(client, "User-Agent", "ESP32-AudioPlayer/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);

    cJSON *json = NULL;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status: %d, response len: %d", status, response.len);

        if (status == 200 && response.len > 0) {
            // Debug: show first 200 chars of response
            ESP_LOGI(TAG, "Response (first 200 chars): %.200s", response.buffer);

            json = cJSON_Parse(response.buffer);
            if (!json) {
                ESP_LOGE(TAG, "Failed to parse JSON, error: %s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
            }
        } else if (status != 200) {
            ESP_LOGE(TAG, "HTTP error status: %d", status);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s (0x%x)", esp_err_to_name(err), err);
    }

    esp_http_client_cleanup(client);
    free(response.buffer);

    return json;
}

// Parsuj tablice stacji z JSON
static int parse_stations(cJSON *json, radio_browser_station_t *results, int max_results)
{
    if (!cJSON_IsArray(json)) {
        return 0;
    }

    int count = 0;
    cJSON *station;
    cJSON_ArrayForEach(station, json) {
        if (count >= max_results) break;

        cJSON *name = cJSON_GetObjectItem(station, "name");
        cJSON *url = cJSON_GetObjectItem(station, "url_resolved");
        if (!url || !cJSON_IsString(url) || strlen(url->valuestring) == 0) {
            url = cJSON_GetObjectItem(station, "url");
        }
        cJSON *country = cJSON_GetObjectItem(station, "country");
        cJSON *tags = cJSON_GetObjectItem(station, "tags");
        cJSON *bitrate = cJSON_GetObjectItem(station, "bitrate");
        cJSON *votes = cJSON_GetObjectItem(station, "votes");

        // Sprawdz czy stacja ma prawidlowy URL
        if (!url || !cJSON_IsString(url) || strlen(url->valuestring) == 0) {
            continue;
        }

        // Kopiuj dane
        if (name && cJSON_IsString(name)) {
            strncpy(results[count].name, name->valuestring, sizeof(results[count].name) - 1);
        } else {
            strcpy(results[count].name, "Unknown");
        }

        strncpy(results[count].url, url->valuestring, sizeof(results[count].url) - 1);

        if (country && cJSON_IsString(country)) {
            strncpy(results[count].country, country->valuestring, sizeof(results[count].country) - 1);
        } else {
            results[count].country[0] = '\0';
        }

        if (tags && cJSON_IsString(tags)) {
            strncpy(results[count].tags, tags->valuestring, sizeof(results[count].tags) - 1);
        } else {
            results[count].tags[0] = '\0';
        }

        results[count].bitrate = (bitrate && cJSON_IsNumber(bitrate)) ? bitrate->valueint : 0;
        results[count].votes = (votes && cJSON_IsNumber(votes)) ? votes->valueint : 0;

        count++;
    }

    return count;
}

// URL encode string
static void url_encode(const char *src, char *dst, size_t dst_size)
{
    const char *hex = "0123456789ABCDEF";
    size_t pos = 0;

    while (*src && pos < dst_size - 4) {
        if ((*src >= 'a' && *src <= 'z') ||
            (*src >= 'A' && *src <= 'Z') ||
            (*src >= '0' && *src <= '9') ||
            *src == '-' || *src == '_' || *src == '.' || *src == '~') {
            dst[pos++] = *src;
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[(*src >> 4) & 0x0F];
            dst[pos++] = hex[*src & 0x0F];
        }
        src++;
    }
    dst[pos] = '\0';
}

// ============================================
// Publiczne API
// ============================================

esp_err_t radio_browser_init(void)
{
    ESP_LOGI(TAG, "Radio Browser module initialized");
    return ESP_OK;
}

int radio_browser_search_by_name(const char *name, const char *country_code,
                                  radio_browser_station_t *results, int max_results)
{
    if (!name || !results || max_results <= 0) {
        return 0;
    }

    char encoded_name[128];
    url_encode(name, encoded_name, sizeof(encoded_name));

    char endpoint[256];
    if (country_code && strlen(country_code) > 0) {
        snprintf(endpoint, sizeof(endpoint),
                 "/stations/search?name=%s&countrycode=%s&limit=%d&order=votes&reverse=true",
                 encoded_name, country_code, max_results);
    } else {
        snprintf(endpoint, sizeof(endpoint),
                 "/stations/search?name=%s&limit=%d&order=votes&reverse=true",
                 encoded_name, max_results);
    }

    cJSON *json = radio_browser_request(endpoint);
    if (!json) {
        return 0;
    }

    int count = parse_stations(json, results, max_results);
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Found %d stations for name: %s", count, name);
    return count;
}

int radio_browser_search_by_country(const char *country_code,
                                     radio_browser_station_t *results, int max_results)
{
    if (!country_code || !results || max_results <= 0) {
        return 0;
    }

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint),
             "/stations/bycountrycodeexact/%s?limit=%d&order=votes&reverse=true",
             country_code, max_results);

    cJSON *json = radio_browser_request(endpoint);
    if (!json) {
        return 0;
    }

    int count = parse_stations(json, results, max_results);
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Found %d stations for country: %s", count, country_code);
    return count;
}

int radio_browser_search_by_tag(const char *tag, const char *country_code,
                                 radio_browser_station_t *results, int max_results)
{
    if (!tag || !results || max_results <= 0) {
        return 0;
    }

    char encoded_tag[64];
    url_encode(tag, encoded_tag, sizeof(encoded_tag));

    char endpoint[256];
    if (country_code && strlen(country_code) > 0) {
        snprintf(endpoint, sizeof(endpoint),
                 "/stations/search?tag=%s&countrycode=%s&limit=%d&order=votes&reverse=true",
                 encoded_tag, country_code, max_results);
    } else {
        snprintf(endpoint, sizeof(endpoint),
                 "/stations/bytag/%s?limit=%d&order=votes&reverse=true",
                 encoded_tag, max_results);
    }

    cJSON *json = radio_browser_request(endpoint);
    if (!json) {
        return 0;
    }

    int count = parse_stations(json, results, max_results);
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Found %d stations for tag: %s", count, tag);
    return count;
}

int radio_browser_get_countries(char countries[][32], int max_countries)
{
    // Statyczna lista najpopularniejszych krajow (oszczednosc pamieci)
    static const char *popular_countries[] = {
        "PL", "DE", "US", "GB", "FR", "ES", "IT", "NL", "AT", "CH",
        "CZ", "SK", "UA", "RU", "BR", "CA", "AU", "JP", "IN", "MX"
    };

    int count = sizeof(popular_countries) / sizeof(popular_countries[0]);
    if (count > max_countries) {
        count = max_countries;
    }

    for (int i = 0; i < count; i++) {
        strncpy(countries[i], popular_countries[i], 31);
        countries[i][31] = '\0';
    }

    return count;
}

int radio_browser_get_top_stations(const char *country_code,
                                    radio_browser_station_t *results, int max_results)
{
    if (!results || max_results <= 0) {
        return 0;
    }

    char endpoint[128];
    if (country_code && strlen(country_code) > 0) {
        snprintf(endpoint, sizeof(endpoint),
                 "/stations/bycountrycodeexact/%s?limit=%d&order=votes&reverse=true",
                 country_code, max_results);
    } else {
        snprintf(endpoint, sizeof(endpoint),
                 "/stations/topvote/%d", max_results);
    }

    cJSON *json = radio_browser_request(endpoint);
    if (!json) {
        return 0;
    }

    int count = parse_stations(json, results, max_results);
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Found %d top stations", count);
    return count;
}
