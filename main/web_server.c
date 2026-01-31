/*
 * Web Server Module
 * HTTP server with REST API and WebSocket support
 */

#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "web_server.h"
#include "config.h"
#include "audio_player.h"
#include "radio_stations.h"
#include "radio_browser.h"
#include "alarm_manager.h"
#include "wifi_manager.h"
#include "nvs_flash.h"
#include "esp_system.h"

// Nowe moduły
#include "input_controls.h"
#include "bluetooth_sink.h"
#include "sdcard_player.h"
#include "aux_input.h"
#include "battery_monitor.h"
#include "piped_client.h"
#include "ota_update.h"
#include "bluetooth_source.h"
#include "audio_settings.h"
#include "system_diag.h"

static const char *TAG = "WEB_SERVER";

// HTTP server handle
static httpd_handle_t server = NULL;

// Embedded files (z CMakeLists.txt)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");

// WebSocket async list
#define MAX_WS_CLIENTS 4
static int ws_clients[MAX_WS_CLIENTS] = {-1, -1, -1, -1};

// ============================================
// Pomocnicze funkcje
// ============================================

static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

// ============================================
// Static file handlers
// ============================================

static esp_err_t index_html_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)app_js_start,
                    app_js_end - app_js_start);
    return ESP_OK;
}

static esp_err_t style_css_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)style_css_start,
                    style_css_end - style_css_start);
    return ESP_OK;
}

// ============================================
// API handlers - Status
// ============================================

static esp_err_t api_status_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    player_status_t *status = audio_player_get_status();

    const char *state_str = "idle";
    switch (status->state) {
        case PLAYER_STATE_BUFFERING: state_str = "buffering"; break;
        case PLAYER_STATE_PLAYING: state_str = "playing"; break;
        case PLAYER_STATE_PAUSED:  state_str = "paused"; break;
        case PLAYER_STATE_STOPPED: state_str = "stopped"; break;
        default: break;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddNumberToObject(root, "volume", status->volume);
    cJSON_AddBoolToObject(root, "muted", status->muted);
    cJSON_AddStringToObject(root, "url", status->current_url);
    cJSON_AddStringToObject(root, "title", status->current_title);
    cJSON_AddStringToObject(root, "artist", status->current_artist);
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "buffer_level", audio_player_get_buffer_level());

    // Czas
    time_t now = alarm_manager_get_time();
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo);
    cJSON_AddStringToObject(root, "time", time_str);
    cJSON_AddBoolToObject(root, "time_synced", alarm_manager_is_time_synced());

    // Uptime
    int64_t uptime_us = esp_timer_get_time();
    int uptime_sec = uptime_us / 1000000;
    int days = uptime_sec / 86400;
    int hours = (uptime_sec % 86400) / 3600;
    int minutes = (uptime_sec % 3600) / 60;
    int seconds = uptime_sec % 60;
    char uptime_str[32];
    if (days > 0) {
        snprintf(uptime_str, sizeof(uptime_str), "%dd %02d:%02d:%02d", days, hours, minutes, seconds);
    } else {
        snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d:%02d", hours, minutes, seconds);
    }
    cJSON_AddStringToObject(root, "uptime", uptime_str);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================
// API handlers - Player control
// ============================================

static esp_err_t api_play_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *url = cJSON_GetObjectItem(root, "url");
    if (url && cJSON_IsString(url)) {
        esp_err_t ret = audio_player_play_url(url->valuestring);
        if (ret == ESP_OK) {
            httpd_resp_sendstr(req, "{\"success\":true}");
        } else {
            ESP_LOGE(TAG, "Failed to play URL: %s, error: %s", url->valuestring, esp_err_to_name(ret));
            char error_json[128];
            snprintf(error_json, sizeof(error_json), "{\"success\":false,\"error\":\"%s\"}", esp_err_to_name(ret));
            httpd_resp_sendstr(req, error_json);
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing url");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_stop_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    audio_player_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

static esp_err_t api_pause_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    audio_player_pause();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

static esp_err_t api_resume_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    audio_player_resume();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

static esp_err_t api_volume_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *volume = cJSON_GetObjectItem(root, "volume");
    if (volume && cJSON_IsNumber(volume)) {
        audio_player_set_volume(volume->valueint);
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing volume");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================
// API handlers - Radio stations
// ============================================

static esp_err_t api_stations_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    uint8_t count;
    radio_station_t *stations = radio_stations_get_all(&count);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *station = cJSON_CreateObject();
        cJSON_AddNumberToObject(station, "id", stations[i].id);
        cJSON_AddStringToObject(station, "name", stations[i].name);
        cJSON_AddStringToObject(station, "url", stations[i].url);
        cJSON_AddStringToObject(station, "logo", stations[i].logo_url);
        cJSON_AddBoolToObject(station, "favorite", stations[i].favorite);
        cJSON_AddItemToArray(root, station);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_stations_add_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *url = cJSON_GetObjectItem(root, "url");
    cJSON *logo = cJSON_GetObjectItem(root, "logo");

    if (name && url && cJSON_IsString(name) && cJSON_IsString(url)) {
        esp_err_t err = radio_stations_add(name->valuestring, url->valuestring,
                                           logo ? logo->valuestring : "");
        if (err == ESP_OK) {
            httpd_resp_sendstr(req, "{\"success\":true}");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to add station");
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name or url");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_stations_delete_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (id && cJSON_IsNumber(id)) {
        esp_err_t err = radio_stations_remove((uint8_t)id->valueint);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Station %d deleted", id->valueint);
            httpd_resp_sendstr(req, "{\"success\":true}");
        } else {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Station not found");
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_stations_favorite_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (id && cJSON_IsNumber(id)) {
        // Toggle favorite status
        radio_station_t *station = radio_stations_get((uint8_t)id->valueint);
        if (station) {
            esp_err_t err = radio_stations_set_favorite((uint8_t)id->valueint, !station->favorite);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Station %d favorite toggled", id->valueint);
                httpd_resp_sendstr(req, "{\"success\":true}");
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update");
            }
        } else {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Station not found");
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================
// API handlers - Radio Browser (wyszukiwanie)
// ============================================

static esp_err_t api_radio_search_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    // Pobierz parametry z query string
    char query[256] = {0};
    char name[64] = {0};
    char country[8] = {0};
    char tag[32] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
        httpd_query_key_value(query, "country", country, sizeof(country));
        httpd_query_key_value(query, "tag", tag, sizeof(tag));
    }

    ESP_LOGI(TAG, "Radio search: name=%s, country=%s, tag=%s", name, country, tag);

    // Alokacja wynikow
    radio_browser_station_t *results = calloc(RADIO_BROWSER_MAX_RESULTS, sizeof(radio_browser_station_t));
    if (!results) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int count = 0;

    // Wybierz metode wyszukiwania
    if (strlen(name) > 0) {
        count = radio_browser_search_by_name(name, country, results, RADIO_BROWSER_MAX_RESULTS);
    } else if (strlen(tag) > 0) {
        count = radio_browser_search_by_tag(tag, country, results, RADIO_BROWSER_MAX_RESULTS);
    } else if (strlen(country) > 0) {
        count = radio_browser_get_top_stations(country, results, RADIO_BROWSER_MAX_RESULTS);
    } else {
        // Domyslnie: popularne stacje
        count = radio_browser_get_top_stations(NULL, results, RADIO_BROWSER_MAX_RESULTS);
    }

    // Buduj JSON response
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *station = cJSON_CreateObject();
        cJSON_AddStringToObject(station, "name", results[i].name);
        cJSON_AddStringToObject(station, "url", results[i].url);
        cJSON_AddStringToObject(station, "country", results[i].country);
        cJSON_AddStringToObject(station, "tags", results[i].tags);
        cJSON_AddNumberToObject(station, "bitrate", results[i].bitrate);
        cJSON_AddNumberToObject(station, "votes", results[i].votes);
        cJSON_AddItemToArray(root, station);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);
    free(results);

    return ESP_OK;
}

static esp_err_t api_radio_countries_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    // Statyczna lista krajow z nazwami
    const char *countries_json = "["
        "{\"code\":\"PL\",\"name\":\"Polska\"},"
        "{\"code\":\"DE\",\"name\":\"Niemcy\"},"
        "{\"code\":\"US\",\"name\":\"USA\"},"
        "{\"code\":\"GB\",\"name\":\"Wielka Brytania\"},"
        "{\"code\":\"FR\",\"name\":\"Francja\"},"
        "{\"code\":\"ES\",\"name\":\"Hiszpania\"},"
        "{\"code\":\"IT\",\"name\":\"Wlochy\"},"
        "{\"code\":\"NL\",\"name\":\"Holandia\"},"
        "{\"code\":\"AT\",\"name\":\"Austria\"},"
        "{\"code\":\"CH\",\"name\":\"Szwajcaria\"},"
        "{\"code\":\"CZ\",\"name\":\"Czechy\"},"
        "{\"code\":\"SK\",\"name\":\"Slowacja\"},"
        "{\"code\":\"UA\",\"name\":\"Ukraina\"},"
        "{\"code\":\"RU\",\"name\":\"Rosja\"},"
        "{\"code\":\"BR\",\"name\":\"Brazylia\"},"
        "{\"code\":\"CA\",\"name\":\"Kanada\"},"
        "{\"code\":\"AU\",\"name\":\"Australia\"},"
        "{\"code\":\"JP\",\"name\":\"Japonia\"},"
        "{\"code\":\"IN\",\"name\":\"Indie\"},"
        "{\"code\":\"MX\",\"name\":\"Meksyk\"}"
    "]";

    httpd_resp_sendstr(req, countries_json);
    return ESP_OK;
}

// ============================================
// API handlers - Alarms
// ============================================

static esp_err_t api_alarms_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    uint8_t count;
    alarm_t *alarms = alarm_manager_get_all(&count);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *alarm = cJSON_CreateObject();
        cJSON_AddNumberToObject(alarm, "id", alarms[i].id);
        cJSON_AddStringToObject(alarm, "name", alarms[i].name);
        cJSON_AddBoolToObject(alarm, "enabled", alarms[i].enabled);
        cJSON_AddNumberToObject(alarm, "hour", alarms[i].hour);
        cJSON_AddNumberToObject(alarm, "minute", alarms[i].minute);
        cJSON_AddNumberToObject(alarm, "days", alarms[i].days);
        cJSON_AddNumberToObject(alarm, "source", alarms[i].source);
        cJSON_AddStringToObject(alarm, "source_uri", alarms[i].source_uri);
        cJSON_AddNumberToObject(alarm, "volume", alarms[i].volume);
        cJSON_AddNumberToObject(alarm, "snooze", alarms[i].snooze_minutes);
        cJSON_AddItemToArray(root, alarm);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// Helper function to parse alarm from JSON
static bool parse_alarm_json(cJSON *root, alarm_t *alarm) {
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *hour = cJSON_GetObjectItem(root, "hour");
    cJSON *minute = cJSON_GetObjectItem(root, "minute");
    cJSON *days = cJSON_GetObjectItem(root, "days");
    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    cJSON *source = cJSON_GetObjectItem(root, "source");
    cJSON *source_uri = cJSON_GetObjectItem(root, "source_uri");
    cJSON *volume = cJSON_GetObjectItem(root, "volume");
    cJSON *snooze = cJSON_GetObjectItem(root, "snooze");

    if (!hour || !minute) {
        return false;
    }

    alarm->hour = hour->valueint;
    alarm->minute = minute->valueint;
    alarm->enabled = enabled ? cJSON_IsTrue(enabled) : true;
    alarm->days = days ? days->valueint : 0x7F;  // Default: everyday
    alarm->source = source ? source->valueint : ALARM_SOURCE_RADIO;
    alarm->volume = volume ? volume->valueint : 50;
    alarm->snooze_minutes = snooze ? snooze->valueint : 5;

    if (name && cJSON_IsString(name)) {
        strncpy(alarm->name, name->valuestring, sizeof(alarm->name) - 1);
        alarm->name[sizeof(alarm->name) - 1] = '\0';
    } else {
        snprintf(alarm->name, sizeof(alarm->name), "Alarm %02d:%02d", alarm->hour, alarm->minute);
    }

    if (source_uri && cJSON_IsString(source_uri)) {
        strncpy(alarm->source_uri, source_uri->valuestring, sizeof(alarm->source_uri) - 1);
        alarm->source_uri[sizeof(alarm->source_uri) - 1] = '\0';
    } else {
        alarm->source_uri[0] = '\0';
    }

    return true;
}

static esp_err_t api_alarms_add_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    alarm_t alarm = {0};
    if (!parse_alarm_json(root, &alarm)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing hour or minute");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    esp_err_t err = alarm_manager_add(&alarm);
    if (err == ESP_OK) {
        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddNumberToObject(response, "id", alarm.id);
        char *json = cJSON_PrintUnformatted(response);
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(response);
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed to add alarm\"}");
    }

    return ESP_OK;
}

static esp_err_t api_alarms_update_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    if (!id_json) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }

    uint8_t id = id_json->valueint;
    alarm_t *existing = alarm_manager_get(id);
    if (!existing) {
        cJSON_Delete(root);
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Alarm not found\"}");
        return ESP_OK;
    }

    // Copy existing alarm and update fields
    alarm_t updated = *existing;

    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *hour = cJSON_GetObjectItem(root, "hour");
    cJSON *minute = cJSON_GetObjectItem(root, "minute");
    cJSON *days = cJSON_GetObjectItem(root, "days");
    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    cJSON *source = cJSON_GetObjectItem(root, "source");
    cJSON *source_uri = cJSON_GetObjectItem(root, "source_uri");
    cJSON *volume = cJSON_GetObjectItem(root, "volume");
    cJSON *snooze = cJSON_GetObjectItem(root, "snooze");

    if (name && cJSON_IsString(name)) {
        strncpy(updated.name, name->valuestring, sizeof(updated.name) - 1);
        updated.name[sizeof(updated.name) - 1] = '\0';
    }
    if (hour) updated.hour = hour->valueint;
    if (minute) updated.minute = minute->valueint;
    if (days) updated.days = days->valueint;
    if (enabled) updated.enabled = cJSON_IsTrue(enabled);
    if (source) updated.source = source->valueint;
    if (volume) updated.volume = volume->valueint;
    if (snooze) updated.snooze_minutes = snooze->valueint;
    if (source_uri && cJSON_IsString(source_uri)) {
        strncpy(updated.source_uri, source_uri->valuestring, sizeof(updated.source_uri) - 1);
        updated.source_uri[sizeof(updated.source_uri) - 1] = '\0';
    }

    cJSON_Delete(root);

    esp_err_t err = alarm_manager_update(&updated);
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed to update alarm\"}");
    }

    return ESP_OK;
}

static esp_err_t api_alarms_delete_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (!id) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }

    esp_err_t err = alarm_manager_remove(id->valueint);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Alarm not found\"}");
    }

    return ESP_OK;
}

static esp_err_t api_alarms_enable_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (!id || !enabled) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id or enabled");
        return ESP_FAIL;
    }

    esp_err_t err = alarm_manager_enable(id->valueint, cJSON_IsTrue(enabled));
    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Alarm not found\"}");
    }

    return ESP_OK;
}

static esp_err_t api_alarm_control_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    const char *action_str = action->valuestring;

    if (strcmp(action_str, "stop") == 0) {
        err = alarm_manager_stop_alarm();
    } else if (strcmp(action_str, "snooze") == 0) {
        err = alarm_manager_snooze();
    }

    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"No active alarm\"}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed\"}");
    }

    return ESP_OK;
}

static esp_err_t api_alarm_status_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    alarm_t *next = alarm_manager_get_next();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", alarm_manager_is_alarm_active());
    cJSON_AddBoolToObject(root, "time_synced", alarm_manager_is_time_synced());

    // Add active alarm info if alarm is ringing
    alarm_t *active = alarm_manager_get_active_alarm();
    if (active) {
        cJSON *active_alarm = cJSON_CreateObject();
        cJSON_AddNumberToObject(active_alarm, "id", active->id);
        cJSON_AddStringToObject(active_alarm, "name", active->name);
        cJSON_AddNumberToObject(active_alarm, "volume", active->volume);
        cJSON_AddItemToObject(root, "active_alarm", active_alarm);
    }

    if (next) {
        cJSON *next_alarm = cJSON_CreateObject();
        cJSON_AddNumberToObject(next_alarm, "id", next->id);
        cJSON_AddStringToObject(next_alarm, "name", next->name);
        cJSON_AddNumberToObject(next_alarm, "hour", next->hour);
        cJSON_AddNumberToObject(next_alarm, "minute", next->minute);
        cJSON_AddItemToObject(root, "next_alarm", next_alarm);
    } else {
        cJSON_AddNullToObject(root, "next_alarm");
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================
// System handlers (WiFi, restart, factory reset)
// ============================================

static esp_err_t api_wifi_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *password = cJSON_GetObjectItem(root, "password");

    if (ssid && cJSON_IsString(ssid)) {
        const char *pwd = (password && cJSON_IsString(password)) ? password->valuestring : "";

        // Zapisz credentials i odpowiedz od razu (połączenie nastąpi przy restarcie)
        wifi_manager_save_credentials(ssid->valuestring, pwd);
        cJSON_Delete(root);

        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"WiFi credentials saved. Restarting...\"}");

        // Restart po 1 sekundzie żeby odpowiedź zdążyła dojść
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
    }

    return ESP_OK;
}

static esp_err_t api_restart_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");

    // Restart po 1 sekundzie
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    ESP_LOGW(TAG, "Factory reset requested!");

    // Wyslij odpowiedz przed resetem
    httpd_resp_sendstr(req, "{\"success\":true}");

    // Poczekaj chwile
    vTaskDelay(pdMS_TO_TICKS(500));

    // Wyczysc NVS
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
    }

    // Restart
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

// ============================================
// Autostart API
// ============================================

static esp_err_t api_autostart_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_GET) {
        // GET - zwróć status autostartu
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "{\"autostart\":%s,\"last_url\":\"%s\"}",
                 audio_settings_get_autostart() ? "true" : "false",
                 audio_settings_get_last_url());
        httpd_resp_sendstr(req, resp);
    } else {
        // POST - ustaw autostart
        char buf[64];
        int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret <= 0) {
            httpd_resp_sendstr(req, "{\"success\":false}");
            return ESP_OK;
        }
        buf[ret] = '\0';

        cJSON *json = cJSON_Parse(buf);
        if (json) {
            cJSON *autostart = cJSON_GetObjectItem(json, "autostart");
            if (autostart) {
                audio_settings_set_autostart(cJSON_IsTrue(autostart));
            }
            cJSON_Delete(json);
        }
        httpd_resp_sendstr(req, "{\"success\":true}");
    }
    return ESP_OK;
}

// ============================================
// OPTIONS handler (CORS preflight)
// ============================================

static esp_err_t options_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ============================================
// WebSocket handler
// ============================================

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "WS received: %s", ws_pkt.payload);
            // TODO: Obsługa komend przez WebSocket
        }
        free(buf);
    }

    return ret;
}

// ============================================
// API handlers - Source control
// ============================================

static esp_err_t api_source_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_GET) {
        // Get current source
        cJSON *root = cJSON_CreateObject();
        audio_source_mode_t source = input_controls_get_current_source();
        cJSON_AddStringToObject(root, "source", input_controls_get_source_name(source));
        cJSON_AddNumberToObject(root, "source_id", source);
        cJSON_AddBoolToObject(root, "headphones", input_controls_is_headphone_connected());

        char *json = cJSON_PrintUnformatted(root);
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(root);
    } else {
        // Set source
        char content[128];
        int ret = httpd_req_recv(req, content, sizeof(content) - 1);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
            return ESP_FAIL;
        }
        content[ret] = '\0';

        cJSON *root = cJSON_Parse(content);
        if (root) {
            cJSON *source = cJSON_GetObjectItem(root, "source");
            if (source && cJSON_IsNumber(source)) {
                input_controls_set_source(source->valueint);
            }
            cJSON_Delete(root);
        }
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    }
    return ESP_OK;
}

// ============================================
// API handlers - Bluetooth
// ============================================

static esp_err_t api_bluetooth_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();

    const char *state_names[] = {"off", "idle", "discoverable", "connecting", "connected", "streaming"};
    bt_state_t state = bluetooth_sink_get_state();
    cJSON_AddStringToObject(root, "state", state_names[state]);
    cJSON_AddBoolToObject(root, "connected", bluetooth_sink_is_connected());
    cJSON_AddBoolToObject(root, "streaming", bluetooth_sink_is_streaming());

    if (bluetooth_sink_is_connected()) {
        const bt_device_info_t *device = bluetooth_sink_get_connected_device();
        cJSON_AddStringToObject(root, "device_name", device->name);
        cJSON_AddStringToObject(root, "device_address", device->address);

        const bt_track_info_t *track = bluetooth_sink_get_track_info();
        if (track->title[0]) {
            cJSON_AddStringToObject(root, "title", track->title);
            cJSON_AddStringToObject(root, "artist", track->artist);
            cJSON_AddStringToObject(root, "album", track->album);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_bluetooth_control_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root) {
        cJSON *action = cJSON_GetObjectItem(root, "action");
        if (action && cJSON_IsString(action)) {
            if (strcmp(action->valuestring, "start") == 0) {
                bluetooth_sink_start();
            } else if (strcmp(action->valuestring, "stop") == 0) {
                bluetooth_sink_stop();
            } else if (strcmp(action->valuestring, "disconnect") == 0) {
                bluetooth_sink_disconnect();
            } else if (strcmp(action->valuestring, "play") == 0) {
                bluetooth_sink_play();
            } else if (strcmp(action->valuestring, "pause") == 0) {
                bluetooth_sink_pause();
            } else if (strcmp(action->valuestring, "next") == 0) {
                bluetooth_sink_next();
            } else if (strcmp(action->valuestring, "prev") == 0) {
                bluetooth_sink_prev();
            }
        }
        cJSON_Delete(root);
    }

    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ============================================
// API handlers - SD Card
// ============================================

static esp_err_t api_sdcard_status_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();

    bool inserted = sdcard_player_is_card_inserted();
    cJSON_AddBoolToObject(root, "inserted", inserted);

    if (inserted) {
        uint64_t total, free_space;
        if (sdcard_player_get_card_info(&total, &free_space) == ESP_OK) {
            cJSON_AddNumberToObject(root, "total_mb", total / (1024 * 1024));
            cJSON_AddNumberToObject(root, "free_mb", free_space / (1024 * 1024));
        }

        sd_player_status_t *status = sdcard_player_get_status();
        const char *state_names[] = {"idle", "playing", "paused", "stopped", "error"};
        cJSON_AddStringToObject(root, "state", state_names[status->state]);
        cJSON_AddNumberToObject(root, "playlist_index", status->playlist_index);
        cJSON_AddNumberToObject(root, "playlist_total", status->playlist_total);

        const char *mode_names[] = {"normal", "repeat_one", "repeat_all", "shuffle"};
        cJSON_AddStringToObject(root, "play_mode", mode_names[status->play_mode]);

        if (status->current_file.filename[0]) {
            cJSON_AddStringToObject(root, "current_file", status->current_file.filename);
            cJSON_AddStringToObject(root, "current_title", status->current_file.title);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_sdcard_browse_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    // Get path from query string
    char path[256] = "/";
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[256];
        if (httpd_query_key_value(query, "path", param, sizeof(param)) == ESP_OK) {
            strncpy(path, param, sizeof(path) - 1);
        }
    }

    sd_file_info_t *files;
    int count;
    esp_err_t ret = sdcard_player_scan_directory(path, &files, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path);

    if (ret == ESP_OK) {
        cJSON *items = cJSON_CreateArray();
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", files[i].filename);
            cJSON_AddStringToObject(item, "path", files[i].filepath);
            cJSON_AddBoolToObject(item, "is_dir", files[i].is_directory);
            if (!files[i].is_directory) {
                cJSON_AddNumberToObject(item, "size", files[i].file_size);
            }
            cJSON_AddItemToArray(items, item);
        }
        cJSON_AddItemToObject(root, "files", items);
        cJSON_AddNumberToObject(root, "count", count);
        sdcard_player_free_file_list(files, count);
    } else {
        cJSON_AddStringToObject(root, "error", "Failed to read directory");
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_sdcard_play_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root) {
        cJSON *path = cJSON_GetObjectItem(root, "path");
        cJSON *action = cJSON_GetObjectItem(root, "action");

        if (action && cJSON_IsString(action)) {
            if (strcmp(action->valuestring, "play") == 0 && path) {
                sdcard_player_play_file(path->valuestring);
            } else if (strcmp(action->valuestring, "play_dir") == 0 && path) {
                sdcard_player_play_directory(path->valuestring);
            } else if (strcmp(action->valuestring, "pause") == 0) {
                sdcard_player_pause();
            } else if (strcmp(action->valuestring, "resume") == 0) {
                sdcard_player_resume();
            } else if (strcmp(action->valuestring, "stop") == 0) {
                sdcard_player_stop();
            } else if (strcmp(action->valuestring, "next") == 0) {
                sdcard_player_next();
            } else if (strcmp(action->valuestring, "prev") == 0) {
                sdcard_player_prev();
            } else if (strcmp(action->valuestring, "mode") == 0) {
                cJSON *mode = cJSON_GetObjectItem(root, "mode");
                if (mode && cJSON_IsNumber(mode)) {
                    sdcard_player_set_play_mode(mode->valueint);
                }
            }
        }
        cJSON_Delete(root);
    }

    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ============================================
// API handlers - AUX Input
// ============================================

static esp_err_t api_aux_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();

        const char *state_names[] = {"disabled", "unplugged", "plugged", "active"};
        aux_state_t state = aux_input_get_state();
        cJSON_AddStringToObject(root, "state", state_names[state]);
        cJSON_AddBoolToObject(root, "connected", aux_input_is_connected());
        cJSON_AddBoolToObject(root, "active", aux_input_is_active());
        cJSON_AddNumberToObject(root, "gain", aux_input_get_gain());
        cJSON_AddNumberToObject(root, "signal_level", aux_input_get_signal_level());

        char *json = cJSON_PrintUnformatted(root);
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(root);
    } else {
        char content[128];
        int ret = httpd_req_recv(req, content, sizeof(content) - 1);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
            return ESP_FAIL;
        }
        content[ret] = '\0';

        cJSON *root = cJSON_Parse(content);
        if (root) {
            cJSON *action = cJSON_GetObjectItem(root, "action");
            if (action && cJSON_IsString(action)) {
                if (strcmp(action->valuestring, "enable") == 0) {
                    aux_input_enable();
                } else if (strcmp(action->valuestring, "disable") == 0) {
                    aux_input_disable();
                }
            }
            cJSON *gain = cJSON_GetObjectItem(root, "gain");
            if (gain && cJSON_IsNumber(gain)) {
                aux_input_set_gain(gain->valueint);
            }
            cJSON_Delete(root);
        }
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    }
    return ESP_OK;
}

// ============================================
// API handlers - Battery
// ============================================

static esp_err_t api_battery_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    battery_status_t *status = battery_monitor_get_status();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "voltage", status->voltage);
    cJSON_AddNumberToObject(root, "percentage", status->percentage);

    const char *charge_states[] = {"discharging", "charging", "full", "not_present", "error"};
    cJSON_AddStringToObject(root, "charge_state", charge_states[status->charge_state]);

    cJSON_AddBoolToObject(root, "usb_powered", status->usb_powered);
    cJSON_AddBoolToObject(root, "low_battery", status->low_battery);
    cJSON_AddBoolToObject(root, "critical", status->critical_battery);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================
// API handlers - System Info (extended)
// ============================================

static esp_err_t api_system_info_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();

    // Device info
    cJSON_AddStringToObject(root, "name", DEVICE_NAME);
    cJSON_AddStringToObject(root, "version", DEVICE_VERSION);
    cJSON_AddStringToObject(root, "board", "ESP32-LyraT V4.3");

    // Memory
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_heap", esp_get_minimum_free_heap_size());

    // WiFi
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "buffer_level", audio_player_get_buffer_level());

    // Current source
    audio_source_mode_t source = input_controls_get_current_source();
    cJSON_AddStringToObject(root, "source", input_controls_get_source_name(source));

    // Headphones
    cJSON_AddBoolToObject(root, "headphones", input_controls_is_headphone_connected());

    // Battery
    battery_status_t *bat = battery_monitor_get_status();
    cJSON *battery = cJSON_CreateObject();
    cJSON_AddNumberToObject(battery, "percentage", bat->percentage);
    cJSON_AddBoolToObject(battery, "charging", bat->charge_state == BATTERY_CHARGING);
    cJSON_AddItemToObject(root, "battery", battery);

    // SD Card
    cJSON_AddBoolToObject(root, "sdcard", sdcard_player_is_card_inserted());

    // Bluetooth
    cJSON_AddBoolToObject(root, "bt_connected", bluetooth_sink_is_connected());

    // AUX
    cJSON_AddBoolToObject(root, "aux_connected", aux_input_is_connected());

    // Time
    cJSON_AddBoolToObject(root, "time_synced", alarm_manager_is_time_synced());

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================
// API handlers - System Diagnostics
static esp_err_t api_system_diag_handler(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char *json = system_diag_get_json();
    if (json) { httpd_resp_sendstr(req, json); free(json); }
    else { httpd_resp_sendstr(req, "{\"error\":\"Failed\"}"); }
    return ESP_OK;
}

// API handlers - Piped (YouTube Music)
// ============================================

static esp_err_t api_piped_search_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    // Get query parameter
    char query[128] = {0};
    char filter[32] = "music_songs";

    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (buf && httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            httpd_query_key_value(buf, "q", query, sizeof(query));
            httpd_query_key_value(buf, "filter", filter, sizeof(filter));
        }
        free(buf);
    }

    if (query[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing q parameter");
        return ESP_FAIL;
    }

    // Perform search
    piped_search_results_t *results = malloc(sizeof(piped_search_results_t));
    if (!results) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    esp_err_t err = piped_search(query, filter, results);
    if (err != ESP_OK) {
        free(results);
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Search failed\",\"items\":[]}");
        return ESP_OK;
    }

    // Build JSON response
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "count", results->count);
    cJSON_AddBoolToObject(root, "has_more", results->has_more);

    cJSON *items = cJSON_CreateArray();
    for (int i = 0; i < results->count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", results->items[i].video_id);
        cJSON_AddStringToObject(item, "title", results->items[i].title);
        cJSON_AddStringToObject(item, "artist", results->items[i].artist);
        cJSON_AddNumberToObject(item, "duration", results->items[i].duration_seconds);
        cJSON_AddNumberToObject(item, "views", results->items[i].views);
        cJSON_AddStringToObject(item, "thumbnail", results->items[i].thumbnail_url);
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddItemToObject(root, "items", items);

    free(results);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_piped_play_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *video_id = cJSON_GetObjectItem(root, "id");
    cJSON *query = cJSON_GetObjectItem(root, "query");

    esp_err_t err = ESP_FAIL;

    if (video_id && cJSON_IsString(video_id)) {
        // Play by video ID
        err = piped_play_video(video_id->valuestring);
    } else if (query && cJSON_IsString(query)) {
        // Search and play first result
        err = piped_play_search(query->valuestring);
    }

    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Playback failed\"}");
    }

    return ESP_OK;
}

static esp_err_t api_piped_stream_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    // Get video ID from query parameter
    char video_id[16] = {0};

    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (buf && httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            httpd_query_key_value(buf, "id", video_id, sizeof(video_id));
        }
        free(buf);
    }

    if (video_id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id parameter");
        return ESP_FAIL;
    }

    piped_stream_info_t *stream = malloc(sizeof(piped_stream_info_t));
    if (!stream) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    esp_err_t err = piped_get_stream(video_id, stream);
    if (err != ESP_OK) {
        free(stream);
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed to get stream\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "id", stream->video_id);
    cJSON_AddStringToObject(root, "title", stream->title);
    cJSON_AddStringToObject(root, "artist", stream->artist);
    cJSON_AddNumberToObject(root, "duration", stream->duration_seconds);
    cJSON_AddStringToObject(root, "thumbnail", stream->thumbnail_url);

    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "url", stream->audio.url);
    cJSON_AddStringToObject(audio, "mime", stream->audio.mime_type);
    cJSON_AddNumberToObject(audio, "bitrate", stream->audio.bitrate);
    cJSON_AddStringToObject(audio, "quality", stream->audio.quality);
    cJSON_AddItemToObject(root, "audio", audio);

    free(stream);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_piped_instance_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_GET) {
        // Return current instance
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "instance", piped_client_get_instance());
        char *json = cJSON_PrintUnformatted(root);
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(root);
    } else {
        // Set instance
        char content[256];
        int ret = httpd_req_recv(req, content, sizeof(content) - 1);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
            return ESP_FAIL;
        }
        content[ret] = '\0';

        cJSON *root = cJSON_Parse(content);
        if (!root) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        cJSON *instance = cJSON_GetObjectItem(root, "instance");
        if (instance && cJSON_IsString(instance)) {
            piped_client_set_instance(instance->valuestring);
            httpd_resp_sendstr(req, "{\"success\":true}");
        } else if (cJSON_GetObjectItem(root, "auto")) {
            // Auto-find working instance
            esp_err_t err = piped_find_working_instance();
            if (err == ESP_OK) {
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", true);
                cJSON_AddStringToObject(resp, "instance", piped_client_get_instance());
                char *json = cJSON_PrintUnformatted(resp);
                httpd_resp_sendstr(req, json);
                free(json);
                cJSON_Delete(resp);
            } else {
                httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"No working instance found\"}");
            }
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing instance");
        }
        cJSON_Delete(root);
    }

    return ESP_OK;
}

// ============================================
// API handlers - OTA Update
// ============================================

static esp_err_t api_ota_status_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    const ota_progress_t *progress = ota_update_get_progress();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "current_version", ota_update_get_version());

    const char *state_names[] = {"idle", "downloading", "verifying", "completed", "error"};
    cJSON_AddStringToObject(root, "state", state_names[progress->state]);

    if (progress->state == OTA_STATE_DOWNLOADING || progress->state == OTA_STATE_VERIFYING) {
        cJSON_AddNumberToObject(root, "progress", progress->progress_percent);
        cJSON_AddNumberToObject(root, "received", progress->received_size);
        cJSON_AddNumberToObject(root, "total", progress->total_size);
    }

    if (progress->error_msg[0]) {
        cJSON_AddStringToObject(root, "error", progress->error_msg);
    }

    if (progress->new_version[0]) {
        cJSON_AddStringToObject(root, "new_version", progress->new_version);
    }

    cJSON_AddBoolToObject(root, "can_rollback", ota_update_can_rollback());

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_ota_upload_handler(httpd_req_t *req)
{
    add_cors_headers(req);

    ESP_LOGI(TAG, "OTA upload started, content length: %d", req->content_len);

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }

    // Check if already in progress
    if (ota_update_is_in_progress()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already in progress");
        return ESP_FAIL;
    }

    // Set longer socket timeout for OTA (60 seconds)
    int timeout_sec = 60;
    setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_RCVTIMEO, &timeout_sec, sizeof(timeout_sec));

    // Begin OTA
    esp_err_t err = ota_update_begin(req->content_len);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    // Use 8KB buffer - smaller chunks to reduce audio glitches during OTA
    #define OTA_BUF_SIZE (8 * 1024)
    char *buf = heap_caps_malloc(OTA_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ota_update_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int total_received = 0;
    int last_progress = -1;

    while (remaining > 0) {
        int to_recv = MIN(remaining, OTA_BUF_SIZE);
        int recv_len = httpd_req_recv(req, buf, to_recv);

        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "OTA recv timeout, retrying...");
                continue;  // Retry on timeout
            }
            ESP_LOGE(TAG, "OTA receive error: %d", recv_len);
            free(buf);
            ota_update_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        err = ota_update_write((uint8_t *)buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write error");
            free(buf);
            ota_update_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
            return ESP_FAIL;
        }

        remaining -= recv_len;
        total_received += recv_len;

        // Log progress every 10%
        int progress = (total_received * 100) / req->content_len;
        if (progress / 10 > last_progress / 10) {
            ESP_LOGI(TAG, "OTA progress: %d%%", progress);
            last_progress = progress;
        }
    }

    free(buf);

    ESP_LOGI(TAG, "OTA upload complete, verifying...");

    // Finish OTA (this will reboot on success)
    err = ota_update_end();
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Verification failed\"}");
        return ESP_OK;
    }

    // Won't reach here - device reboots
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Rebooting...\"}");
    return ESP_OK;
}

static esp_err_t api_ota_url_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!url || !cJSON_IsString(url)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing URL");
        return ESP_FAIL;
    }

    // Start OTA from URL (async - will reboot on success)
    esp_err_t err = ota_update_from_url(url->valuestring);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"OTA failed\"}");
    } else {
        // Won't reach here - device reboots
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Rebooting...\"}");
    }

    return ESP_OK;
}

static esp_err_t api_ota_rollback_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (!ota_update_can_rollback()) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Rollback not available\"}");
        return ESP_OK;
    }

    // Perform rollback (will reboot)
    esp_err_t err = ota_update_rollback();
    if (err != ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Rollback failed\"}");
    } else {
        // Won't reach here - device reboots
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Rolling back...\"}");
    }

    return ESP_OK;
}

// ============================================
// Bluetooth Source API Handlers
// ============================================

static esp_err_t api_bt_source_status_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    const bt_source_status_t *status = bt_source_get_status();

    char json[512];
    char connected_bda[18] = "";
    if (bt_source_is_connected()) {
        bt_source_bda_to_str(status->connected_device.bda, connected_bda);
    }

    snprintf(json, sizeof(json),
        "{\"initialized\":%s,\"state\":\"%s\",\"connected\":%s,\"streaming\":%s,"
        "\"device_name\":\"%s\",\"device_bda\":\"%s\","
        "\"device_count\":%d,\"volume\":%d,\"error\":\"%s\"}",
        bt_source_is_initialized() ? "true" : "false",
        bt_source_state_to_str(status->state),
        bt_source_is_connected() ? "true" : "false",
        bt_source_is_streaming() ? "true" : "false",
        status->connected_device.name,
        connected_bda,
        status->device_count,
        status->volume,
        status->error_msg);

    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t api_bt_source_scan_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = bt_source_start_discovery(10);
    if (err != ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed to start scan\"}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Scanning for 10 seconds\"}");
    }

    return ESP_OK;
}

static esp_err_t api_bt_source_stop_scan_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    bt_source_stop_discovery();
    httpd_resp_sendstr(req, "{\"success\":true}");

    return ESP_OK;
}

static esp_err_t api_bt_source_devices_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    bt_source_device_t devices[BT_SOURCE_MAX_DEVICES];
    uint8_t count = bt_source_get_discovered_devices(devices, BT_SOURCE_MAX_DEVICES);

    // Build JSON array
    char *json = malloc(2048);
    if (!json) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    strcpy(json, "[");
    for (int i = 0; i < count; i++) {
        char bda_str[18];
        bt_source_bda_to_str(devices[i].bda, bda_str);

        char device_json[256];
        snprintf(device_json, sizeof(device_json),
            "%s{\"index\":%d,\"name\":\"%s\",\"bda\":\"%s\",\"rssi\":%ld,\"audio\":%s}",
            i > 0 ? "," : "",
            i,
            devices[i].name,
            bda_str,
            (long)devices[i].rssi,
            devices[i].is_audio_sink ? "true" : "false");
        strcat(json, device_json);
    }
    strcat(json, "]");

    httpd_resp_sendstr(req, json);
    free(json);

    return ESP_OK;
}

static esp_err_t api_bt_source_connect_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"No data\"}");
        return ESP_OK;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return ESP_OK;
    }

    esp_err_t err = ESP_FAIL;

    // Try to connect by index first
    cJSON *index_json = cJSON_GetObjectItem(root, "index");
    if (index_json && cJSON_IsNumber(index_json)) {
        err = bt_source_connect_by_index((uint8_t)index_json->valueint);
    } else {
        // Try to connect by BDA
        cJSON *bda_json = cJSON_GetObjectItem(root, "bda");
        if (bda_json && cJSON_IsString(bda_json)) {
            uint8_t bda[6];
            if (bt_source_str_to_bda(bda_json->valuestring, bda) == ESP_OK) {
                err = bt_source_connect(bda);
            }
        }
    }

    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Connecting...\"}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Connect failed\"}");
    }

    return ESP_OK;
}

static esp_err_t api_bt_source_disconnect_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = bt_source_disconnect();
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Not connected\"}");
    }

    return ESP_OK;
}

static esp_err_t api_bt_source_init_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = bt_source_init();
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"BT Source initialized\"}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Init failed\"}");
    }

    return ESP_OK;
}

static esp_err_t api_bt_source_deinit_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = bt_source_deinit();
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Deinit failed\"}");
    }

    return ESP_OK;
}

// ============================================
// API handlers - Audio Settings (EQ, Balance, Effects)
// ============================================

static esp_err_t api_audio_get_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    audio_settings_t *settings = audio_settings_get();

    cJSON *root = cJSON_CreateObject();

    // EQ bands array
    cJSON *eq = cJSON_CreateArray();
    for (int i = 0; i < EQ_BANDS; i++) {
        cJSON_AddItemToArray(eq, cJSON_CreateNumber(settings->bands[i]));
    }
    cJSON_AddItemToObject(root, "eq", eq);

    // Balance
    cJSON_AddNumberToObject(root, "balance", settings->balance);

    // Preset
    cJSON_AddNumberToObject(root, "preset", settings->preset);

    // Effects
    cJSON *effects = cJSON_CreateObject();
    cJSON_AddBoolToObject(effects, "bassBoost", settings->bass_boost);
    cJSON_AddBoolToObject(effects, "loudness", settings->loudness);
    cJSON_AddBoolToObject(effects, "stereoWide", settings->stereo_wide);
    cJSON_AddItemToObject(root, "effects", effects);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_audio_eq_band_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *band = cJSON_GetObjectItem(root, "band");
    cJSON *value = cJSON_GetObjectItem(root, "value");

    if (band && cJSON_IsNumber(band) && value && cJSON_IsNumber(value)) {
        esp_err_t err = audio_settings_set_band((eq_band_t)band->valueint, (uint8_t)value->valueint);
        if (err == ESP_OK) {
            audio_settings_save();
            httpd_resp_sendstr(req, "{\"success\":true}");
        } else {
            httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid band\"}");
        }
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Missing band or value\"}");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_audio_eq_preset_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *preset = cJSON_GetObjectItem(root, "preset");

    if (preset && cJSON_IsNumber(preset)) {
        esp_err_t err = audio_settings_apply_preset((eq_preset_t)preset->valueint);
        if (err == ESP_OK) {
            audio_settings_save();
            httpd_resp_sendstr(req, "{\"success\":true}");
        } else {
            httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid preset\"}");
        }
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Missing preset\"}");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_audio_balance_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *balance = cJSON_GetObjectItem(root, "balance");

    if (balance && cJSON_IsNumber(balance)) {
        audio_settings_set_balance((int8_t)balance->valueint);
        audio_settings_save();
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Missing balance\"}");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_audio_effects_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *effect = cJSON_GetObjectItem(root, "effect");
    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");

    if (effect && cJSON_IsString(effect) && enabled && cJSON_IsBool(enabled)) {
        const char *effect_name = effect->valuestring;
        bool is_enabled = cJSON_IsTrue(enabled);

        if (strcmp(effect_name, "bassBoost") == 0) {
            audio_settings_set_bass_boost(is_enabled);
        } else if (strcmp(effect_name, "loudness") == 0) {
            audio_settings_set_loudness(is_enabled);
        } else if (strcmp(effect_name, "stereoWide") == 0) {
            audio_settings_set_stereo_wide(is_enabled);
        } else {
            cJSON_Delete(root);
            httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Unknown effect\"}");
            return ESP_OK;
        }

        audio_settings_save();
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Missing effect or enabled\"}");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_audio_reset_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    audio_settings_reset();
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

// ============================================
// Publiczne API
// ============================================

esp_err_t web_server_init(void)
{
    ESP_LOGI(TAG, "Starting web server...");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 64;  // Increased for all API endpoints
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.core_id = 0;  // Pin web server to core 0, leave core 1 for audio

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }

    // Static files
    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_html_handler };
    httpd_uri_t app_js_uri = { .uri = "/app.js", .method = HTTP_GET, .handler = app_js_handler };
    httpd_uri_t style_css_uri = { .uri = "/style.css", .method = HTTP_GET, .handler = style_css_handler };

    // API endpoints
    httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler };
    httpd_uri_t play_uri = { .uri = "/api/play", .method = HTTP_POST, .handler = api_play_handler };
    httpd_uri_t stop_uri = { .uri = "/api/stop", .method = HTTP_POST, .handler = api_stop_handler };
    httpd_uri_t pause_uri = { .uri = "/api/pause", .method = HTTP_POST, .handler = api_pause_handler };
    httpd_uri_t resume_uri = { .uri = "/api/resume", .method = HTTP_POST, .handler = api_resume_handler };
    httpd_uri_t volume_uri = { .uri = "/api/volume", .method = HTTP_POST, .handler = api_volume_handler };
    httpd_uri_t stations_uri = { .uri = "/api/stations", .method = HTTP_GET, .handler = api_stations_handler };
    httpd_uri_t stations_add_uri = { .uri = "/api/stations", .method = HTTP_POST, .handler = api_stations_add_handler };
    httpd_uri_t stations_delete_uri = { .uri = "/api/stations/delete", .method = HTTP_POST, .handler = api_stations_delete_handler };
    httpd_uri_t stations_favorite_uri = { .uri = "/api/stations/favorite", .method = HTTP_POST, .handler = api_stations_favorite_handler };
    httpd_uri_t alarms_uri = { .uri = "/api/alarms", .method = HTTP_GET, .handler = api_alarms_handler };
    httpd_uri_t alarms_add_uri = { .uri = "/api/alarms", .method = HTTP_POST, .handler = api_alarms_add_handler };
    httpd_uri_t alarms_update_uri = { .uri = "/api/alarms/update", .method = HTTP_POST, .handler = api_alarms_update_handler };
    httpd_uri_t alarms_delete_uri = { .uri = "/api/alarms/delete", .method = HTTP_POST, .handler = api_alarms_delete_handler };
    httpd_uri_t alarms_enable_uri = { .uri = "/api/alarms/enable", .method = HTTP_POST, .handler = api_alarms_enable_handler };
    httpd_uri_t alarm_control_uri = { .uri = "/api/alarm/control", .method = HTTP_POST, .handler = api_alarm_control_handler };
    httpd_uri_t alarm_status_uri = { .uri = "/api/alarm/status", .method = HTTP_GET, .handler = api_alarm_status_handler };

    // System API
    httpd_uri_t wifi_uri = { .uri = "/api/wifi", .method = HTTP_POST, .handler = api_wifi_handler };
    httpd_uri_t restart_uri = { .uri = "/api/restart", .method = HTTP_POST, .handler = api_restart_handler };
    httpd_uri_t factory_reset_uri = { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = api_factory_reset_handler };
    httpd_uri_t autostart_get_uri = { .uri = "/api/autostart", .method = HTTP_GET, .handler = api_autostart_handler };
    httpd_uri_t autostart_set_uri = { .uri = "/api/autostart", .method = HTTP_POST, .handler = api_autostart_handler };

    // Radio Browser API
    httpd_uri_t radio_search_uri = { .uri = "/api/radio/search", .method = HTTP_GET, .handler = api_radio_search_handler };
    httpd_uri_t radio_countries_uri = { .uri = "/api/radio/countries", .method = HTTP_GET, .handler = api_radio_countries_handler };

    // Source control API
    httpd_uri_t source_get_uri = { .uri = "/api/source", .method = HTTP_GET, .handler = api_source_handler };
    httpd_uri_t source_set_uri = { .uri = "/api/source", .method = HTTP_POST, .handler = api_source_handler };

    // Bluetooth API
    httpd_uri_t bt_status_uri = { .uri = "/api/bluetooth", .method = HTTP_GET, .handler = api_bluetooth_handler };
    httpd_uri_t bt_control_uri = { .uri = "/api/bluetooth", .method = HTTP_POST, .handler = api_bluetooth_control_handler };

    // SD Card API
    httpd_uri_t sd_status_uri = { .uri = "/api/sdcard", .method = HTTP_GET, .handler = api_sdcard_status_handler };
    httpd_uri_t sd_browse_uri = { .uri = "/api/sdcard/browse", .method = HTTP_GET, .handler = api_sdcard_browse_handler };
    httpd_uri_t sd_play_uri = { .uri = "/api/sdcard/play", .method = HTTP_POST, .handler = api_sdcard_play_handler };

    // AUX Input API
    httpd_uri_t aux_get_uri = { .uri = "/api/aux", .method = HTTP_GET, .handler = api_aux_handler };
    httpd_uri_t aux_set_uri = { .uri = "/api/aux", .method = HTTP_POST, .handler = api_aux_handler };

    // Battery API
    httpd_uri_t battery_uri = { .uri = "/api/battery", .method = HTTP_GET, .handler = api_battery_handler };

    // System Info API
    httpd_uri_t sysinfo_uri = { .uri = "/api/system", .method = HTTP_GET, .handler = api_system_info_handler };
    httpd_uri_t sysdiag_uri = { .uri = "/api/system/diag", .method = HTTP_GET, .handler = api_system_diag_handler };

    // Piped (YouTube Music) API
    httpd_uri_t piped_search_uri = { .uri = "/api/piped/search", .method = HTTP_GET, .handler = api_piped_search_handler };
    httpd_uri_t piped_play_uri = { .uri = "/api/piped/play", .method = HTTP_POST, .handler = api_piped_play_handler };
    httpd_uri_t piped_stream_uri = { .uri = "/api/piped/stream", .method = HTTP_GET, .handler = api_piped_stream_handler };
    httpd_uri_t piped_instance_get_uri = { .uri = "/api/piped/instance", .method = HTTP_GET, .handler = api_piped_instance_handler };
    httpd_uri_t piped_instance_set_uri = { .uri = "/api/piped/instance", .method = HTTP_POST, .handler = api_piped_instance_handler };

    // OTA Update API
    httpd_uri_t ota_status_uri = { .uri = "/api/ota", .method = HTTP_GET, .handler = api_ota_status_handler };
    httpd_uri_t ota_upload_uri = { .uri = "/api/ota/upload", .method = HTTP_POST, .handler = api_ota_upload_handler };
    httpd_uri_t ota_url_uri = { .uri = "/api/ota/url", .method = HTTP_POST, .handler = api_ota_url_handler };
    httpd_uri_t ota_rollback_uri = { .uri = "/api/ota/rollback", .method = HTTP_POST, .handler = api_ota_rollback_handler };

    // Bluetooth Source API
    httpd_uri_t bt_source_status_uri = { .uri = "/api/bt/source/status", .method = HTTP_GET, .handler = api_bt_source_status_handler };
    httpd_uri_t bt_source_init_uri = { .uri = "/api/bt/source/init", .method = HTTP_POST, .handler = api_bt_source_init_handler };
    httpd_uri_t bt_source_deinit_uri = { .uri = "/api/bt/source/deinit", .method = HTTP_POST, .handler = api_bt_source_deinit_handler };
    httpd_uri_t bt_source_scan_uri = { .uri = "/api/bt/source/scan", .method = HTTP_POST, .handler = api_bt_source_scan_handler };
    httpd_uri_t bt_source_stop_scan_uri = { .uri = "/api/bt/source/scan/stop", .method = HTTP_POST, .handler = api_bt_source_stop_scan_handler };
    httpd_uri_t bt_source_devices_uri = { .uri = "/api/bt/source/devices", .method = HTTP_GET, .handler = api_bt_source_devices_handler };
    httpd_uri_t bt_source_connect_uri = { .uri = "/api/bt/source/connect", .method = HTTP_POST, .handler = api_bt_source_connect_handler };
    httpd_uri_t bt_source_disconnect_uri = { .uri = "/api/bt/source/disconnect", .method = HTTP_POST, .handler = api_bt_source_disconnect_handler };

    // Audio Settings API (EQ, Balance, Effects)
    httpd_uri_t audio_get_uri = { .uri = "/api/audio", .method = HTTP_GET, .handler = api_audio_get_handler };
    httpd_uri_t audio_eq_uri = { .uri = "/api/audio/eq", .method = HTTP_POST, .handler = api_audio_eq_band_handler };
    httpd_uri_t audio_eq_preset_uri = { .uri = "/api/audio/eq/preset", .method = HTTP_POST, .handler = api_audio_eq_preset_handler };
    httpd_uri_t audio_balance_uri = { .uri = "/api/audio/balance", .method = HTTP_POST, .handler = api_audio_balance_handler };
    httpd_uri_t audio_effects_uri = { .uri = "/api/audio/effects", .method = HTTP_POST, .handler = api_audio_effects_handler };
    httpd_uri_t audio_reset_uri = { .uri = "/api/audio/reset", .method = HTTP_POST, .handler = api_audio_reset_handler };

    // CORS preflight
    httpd_uri_t options_uri = { .uri = "/api/*", .method = HTTP_OPTIONS, .handler = options_handler };

    // WebSocket
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };

    // Rejestracja - Static files
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &app_js_uri);
    httpd_register_uri_handler(server, &style_css_uri);

    // Rejestracja - Player API
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &play_uri);
    httpd_register_uri_handler(server, &stop_uri);
    httpd_register_uri_handler(server, &pause_uri);
    httpd_register_uri_handler(server, &resume_uri);
    httpd_register_uri_handler(server, &volume_uri);

    // Rejestracja - Stations API
    httpd_register_uri_handler(server, &stations_uri);
    httpd_register_uri_handler(server, &stations_add_uri);
    httpd_register_uri_handler(server, &stations_delete_uri);
    httpd_register_uri_handler(server, &stations_favorite_uri);
    httpd_register_uri_handler(server, &alarms_uri);
    httpd_register_uri_handler(server, &alarms_add_uri);
    httpd_register_uri_handler(server, &alarms_update_uri);
    httpd_register_uri_handler(server, &alarms_delete_uri);
    httpd_register_uri_handler(server, &alarms_enable_uri);
    httpd_register_uri_handler(server, &alarm_control_uri);
    httpd_register_uri_handler(server, &alarm_status_uri);

    // Rejestracja - System API
    httpd_register_uri_handler(server, &wifi_uri);
    httpd_register_uri_handler(server, &restart_uri);
    httpd_register_uri_handler(server, &factory_reset_uri);
    httpd_register_uri_handler(server, &autostart_get_uri);
    httpd_register_uri_handler(server, &autostart_set_uri);

    // Rejestracja - Radio Browser API
    httpd_register_uri_handler(server, &radio_search_uri);
    httpd_register_uri_handler(server, &radio_countries_uri);

    // Rejestracja - Source control API
    httpd_register_uri_handler(server, &source_get_uri);
    httpd_register_uri_handler(server, &source_set_uri);

    // Rejestracja - Bluetooth API
    httpd_register_uri_handler(server, &bt_status_uri);
    httpd_register_uri_handler(server, &bt_control_uri);

    // Rejestracja - SD Card API
    httpd_register_uri_handler(server, &sd_status_uri);
    httpd_register_uri_handler(server, &sd_browse_uri);
    httpd_register_uri_handler(server, &sd_play_uri);

    // Rejestracja - AUX API
    httpd_register_uri_handler(server, &aux_get_uri);
    httpd_register_uri_handler(server, &aux_set_uri);

    // Rejestracja - Battery & System API
    httpd_register_uri_handler(server, &battery_uri);
    httpd_register_uri_handler(server, &sysinfo_uri);
    httpd_register_uri_handler(server, &sysdiag_uri);

    // Rejestracja - Piped API
    httpd_register_uri_handler(server, &piped_search_uri);
    httpd_register_uri_handler(server, &piped_play_uri);
    httpd_register_uri_handler(server, &piped_stream_uri);
    httpd_register_uri_handler(server, &piped_instance_get_uri);
    httpd_register_uri_handler(server, &piped_instance_set_uri);

    // Rejestracja - OTA API
    httpd_register_uri_handler(server, &ota_status_uri);
    httpd_register_uri_handler(server, &ota_upload_uri);
    httpd_register_uri_handler(server, &ota_url_uri);
    httpd_register_uri_handler(server, &ota_rollback_uri);

    // Rejestracja - Bluetooth Source API
    httpd_register_uri_handler(server, &bt_source_status_uri);
    httpd_register_uri_handler(server, &bt_source_init_uri);
    httpd_register_uri_handler(server, &bt_source_deinit_uri);
    httpd_register_uri_handler(server, &bt_source_scan_uri);
    httpd_register_uri_handler(server, &bt_source_stop_scan_uri);
    httpd_register_uri_handler(server, &bt_source_devices_uri);
    httpd_register_uri_handler(server, &bt_source_connect_uri);
    httpd_register_uri_handler(server, &bt_source_disconnect_uri);

    // Rejestracja - Audio Settings API
    httpd_register_uri_handler(server, &audio_get_uri);
    httpd_register_uri_handler(server, &audio_eq_uri);
    httpd_register_uri_handler(server, &audio_eq_preset_uri);
    httpd_register_uri_handler(server, &audio_balance_uri);
    httpd_register_uri_handler(server, &audio_effects_uri);
    httpd_register_uri_handler(server, &audio_reset_uri);

    // Rejestracja - CORS & WebSocket
    httpd_register_uri_handler(server, &options_uri);
    httpd_register_uri_handler(server, &ws_uri);

    ESP_LOGI(TAG, "Web server started successfully");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    return ESP_OK;
}

bool web_server_is_running(void)
{
    return server != NULL;
}

esp_err_t web_server_send_state_update(const char *json_state)
{
    // TODO: Broadcast do wszystkich WebSocket klientów
    return ESP_OK;
}
