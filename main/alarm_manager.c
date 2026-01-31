/*
 * Alarm Manager Module
 * Handles alarm clock functionality with NTP time sync
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#include "alarm_manager.h"
#include "config.h"

static const char *TAG = "ALARM_MGR";

// Lista alarmów
static alarm_t alarms[MAX_ALARMS];
static uint8_t alarm_count = 0;

// Stan
static bool time_synced = false;
static alarm_trigger_callback_t trigger_callback = NULL;
static bool alarm_active = false;
static alarm_t *active_alarm = NULL;
static time_t alarm_start_time = 0;
static time_t snooze_until = 0;

// Użyj wartości z config.h lub domyślnych
#ifndef ALARM_AUTO_STOP_MINUTES
#define ALARM_AUTO_STOP_MINUTES     5       // Auto-stop po 5 minutach
#endif

// NVS handle
static nvs_handle_t alarm_nvs_handle;

// Task handle
static TaskHandle_t alarm_task_handle = NULL;

// ============================================
// NTP callback
// ============================================

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synchronized");
    time_synced = true;
}

// ============================================
// Alarm check task
// ============================================

static void trigger_alarm(alarm_t *alarm) {
    ESP_LOGI(TAG, "ALARM TRIGGERED: %s (%02d:%02d)",
             alarm->name, alarm->hour, alarm->minute);

    alarm_active = true;
    active_alarm = alarm;
    alarm_start_time = time(NULL);
    snooze_until = 0;

    if (trigger_callback) {
        trigger_callback(alarm);
    }
}

static void alarm_check_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));  // Sprawdzaj co 5 sekund - oszczędność CPU

        if (!time_synced) {
            continue;
        }

        time_t now = time(NULL);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        // Jeśli alarm jest aktywny - sprawdź auto-stop
        if (alarm_active) {
            int elapsed_minutes = (now - alarm_start_time) / 60;
            if (elapsed_minutes >= ALARM_AUTO_STOP_MINUTES) {
                ESP_LOGW(TAG, "Alarm auto-stop after %d minutes", elapsed_minutes);
                alarm_active = false;
                active_alarm = NULL;
                alarm_start_time = 0;
            }
            continue;
        }

        // Sprawdź czy snooze się skończył
        if (snooze_until > 0 && now >= snooze_until) {
            ESP_LOGI(TAG, "Snooze ended - retriggering alarm");
            snooze_until = 0;
            // Znajdź alarm który był w snooze i uruchom ponownie
            for (int i = 0; i < alarm_count; i++) {
                if (alarms[i].enabled) {
                    trigger_alarm(&alarms[i]);
                    break;
                }
            }
            continue;
        }

        // Sprawdź każdy alarm
        for (int i = 0; i < alarm_count; i++) {
            if (!alarms[i].enabled) {
                continue;
            }

            // Sprawdź godzinę i minutę
            if (alarms[i].hour != timeinfo.tm_hour ||
                alarms[i].minute != timeinfo.tm_min) {
                continue;
            }

            // Sprawdź sekundę (tylko raz na minutę)
            if (timeinfo.tm_sec != 0) {
                continue;
            }

            // Sprawdź dzień tygodnia (0=niedziela w tm, ale w naszym formacie 0=poniedziałek)
            int day_bit;
            if (timeinfo.tm_wday == 0) {
                day_bit = ALARM_DAY_SUNDAY;
            } else {
                day_bit = (1 << (timeinfo.tm_wday - 1));
            }

            if (!(alarms[i].days & day_bit)) {
                continue;
            }

            // Alarm!
            trigger_alarm(&alarms[i]);
        }
    }
}

// ============================================
// Publiczne API
// ============================================

esp_err_t alarm_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing alarm manager...");

    // Otwarcie NVS
    esp_err_t ret = nvs_open("alarms", NVS_READWRITE, &alarm_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return ret;
    }

    memset(alarms, 0, sizeof(alarms));
    alarm_count = 0;

    // Uruchom task sprawdzający alarmy (4KB dla tone_generator i audio calls)
    xTaskCreate(alarm_check_task, "alarm_check", 4096, NULL, 3, &alarm_task_handle);

    ESP_LOGI(TAG, "Alarm manager initialized");
    return ESP_OK;
}

esp_err_t alarm_manager_sync_time(void)
{
    ESP_LOGI(TAG, "Synchronizing time with NTP...");

    // Konfiguracja strefy czasowej
    setenv("TZ", NTP_TIMEZONE, 1);
    tzset();

    // Inicjalizacja SNTP
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    // Czekaj max 10 sekund na synchronizację
    int retry = 0;
    while (!time_synced && retry < 10) {
        ESP_LOGI(TAG, "Waiting for NTP sync... (%d)", retry);
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }

    if (time_synced) {
        time_t now = time(NULL);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "Current time: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "NTP sync timeout");
    return ESP_ERR_TIMEOUT;
}

bool alarm_manager_is_time_synced(void)
{
    return time_synced;
}

time_t alarm_manager_get_time(void)
{
    return time(NULL);
}

esp_err_t alarm_manager_add(alarm_t *alarm)
{
    if (alarm_count >= MAX_ALARMS) {
        ESP_LOGE(TAG, "Maximum alarm count reached");
        return ESP_ERR_NO_MEM;
    }

    // Znajdź wolne ID
    uint8_t new_id = 1;
    for (int i = 0; i < alarm_count; i++) {
        if (alarms[i].id >= new_id) {
            new_id = alarms[i].id + 1;
        }
    }

    alarm->id = new_id;
    memcpy(&alarms[alarm_count], alarm, sizeof(alarm_t));
    alarm_count++;

    ESP_LOGI(TAG, "Added alarm: %s (ID: %d) at %02d:%02d",
             alarm->name, new_id, alarm->hour, alarm->minute);

    return alarm_manager_save();
}

esp_err_t alarm_manager_remove(uint8_t id)
{
    for (int i = 0; i < alarm_count; i++) {
        if (alarms[i].id == id) {
            memmove(&alarms[i], &alarms[i + 1],
                    (alarm_count - i - 1) * sizeof(alarm_t));
            alarm_count--;

            ESP_LOGI(TAG, "Removed alarm ID: %d", id);
            return alarm_manager_save();
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t alarm_manager_update(alarm_t *alarm)
{
    for (int i = 0; i < alarm_count; i++) {
        if (alarms[i].id == alarm->id) {
            memcpy(&alarms[i], alarm, sizeof(alarm_t));
            ESP_LOGI(TAG, "Updated alarm ID: %d", alarm->id);
            return alarm_manager_save();
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t alarm_manager_enable(uint8_t id, bool enable)
{
    for (int i = 0; i < alarm_count; i++) {
        if (alarms[i].id == id) {
            alarms[i].enabled = enable;
            ESP_LOGI(TAG, "Alarm ID %d %s", id, enable ? "enabled" : "disabled");
            return alarm_manager_save();
        }
    }

    return ESP_ERR_NOT_FOUND;
}

alarm_t *alarm_manager_get(uint8_t id)
{
    for (int i = 0; i < alarm_count; i++) {
        if (alarms[i].id == id) {
            return &alarms[i];
        }
    }
    return NULL;
}

alarm_t *alarm_manager_get_all(uint8_t *count)
{
    *count = alarm_count;
    return alarms;
}

alarm_t *alarm_manager_get_next(void)
{
    if (!time_synced || alarm_count == 0) {
        return NULL;
    }

    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    int current_minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int current_day = (timeinfo.tm_wday == 0) ? 6 : timeinfo.tm_wday - 1;  // 0=poniedziałek

    alarm_t *next_alarm = NULL;
    int min_diff = INT32_MAX;

    for (int i = 0; i < alarm_count; i++) {
        if (!alarms[i].enabled) {
            continue;
        }

        int alarm_minutes = alarms[i].hour * 60 + alarms[i].minute;

        // Sprawdź dla każdego dnia tygodnia
        for (int day = 0; day < 7; day++) {
            int check_day = (current_day + day) % 7;
            int day_bit = (1 << check_day);

            if (!(alarms[i].days & day_bit)) {
                continue;
            }

            int diff;
            if (day == 0 && alarm_minutes > current_minutes) {
                diff = alarm_minutes - current_minutes;
            } else if (day > 0) {
                diff = day * 24 * 60 + alarm_minutes - current_minutes;
            } else {
                continue;
            }

            if (diff < min_diff) {
                min_diff = diff;
                next_alarm = &alarms[i];
            }

            break;  // Tylko pierwszy pasujący dzień
        }
    }

    return next_alarm;
}

esp_err_t alarm_manager_snooze(void)
{
    if (!alarm_active || !active_alarm) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t snooze_minutes = active_alarm->snooze_minutes;
    if (snooze_minutes == 0) snooze_minutes = 5;  // Default 5 minutes

    ESP_LOGI(TAG, "Snooze for %d minutes", snooze_minutes);

    // Ustaw czas ponownego uruchomienia alarmu
    snooze_until = time(NULL) + (snooze_minutes * 60);

    // Zatrzymaj aktualny alarm (dźwięk zostanie zatrzymany przez callback)
    alarm_active = false;
    // Zachowaj active_alarm dla info o snooze
    alarm_start_time = 0;

    return ESP_OK;
}

esp_err_t alarm_manager_stop_alarm(void)
{
    if (!alarm_active && snooze_until == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Alarm stopped");
    alarm_active = false;
    active_alarm = NULL;
    alarm_start_time = 0;
    snooze_until = 0;  // Anuluj też snooze

    return ESP_OK;
}

bool alarm_manager_is_alarm_active(void)
{
    return alarm_active;
}

alarm_t *alarm_manager_get_active_alarm(void)
{
    return active_alarm;
}

void alarm_manager_register_callback(alarm_trigger_callback_t callback)
{
    trigger_callback = callback;
}

esp_err_t alarm_manager_save(void)
{
    ESP_LOGI(TAG, "Saving alarms to NVS...");

    cJSON *root = cJSON_CreateArray();

    for (int i = 0; i < alarm_count; i++) {
        cJSON *alarm = cJSON_CreateObject();
        cJSON_AddNumberToObject(alarm, "id", alarms[i].id);
        cJSON_AddStringToObject(alarm, "name", alarms[i].name);
        cJSON_AddBoolToObject(alarm, "enabled", alarms[i].enabled);
        cJSON_AddNumberToObject(alarm, "hour", alarms[i].hour);
        cJSON_AddNumberToObject(alarm, "minute", alarms[i].minute);
        cJSON_AddNumberToObject(alarm, "days", alarms[i].days);
        cJSON_AddNumberToObject(alarm, "source", alarms[i].source);
        cJSON_AddStringToObject(alarm, "uri", alarms[i].source_uri);
        cJSON_AddNumberToObject(alarm, "volume", alarms[i].volume);
        cJSON_AddNumberToObject(alarm, "snooze", alarms[i].snooze_minutes);
        cJSON_AddItemToArray(root, alarm);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nvs_set_str(alarm_nvs_handle, "alarms", json_str);
    free(json_str);

    if (ret == ESP_OK) {
        ret = nvs_commit(alarm_nvs_handle);
    }

    ESP_LOGI(TAG, "Alarms saved (%d alarms)", alarm_count);
    return ret;
}

esp_err_t alarm_manager_load(void)
{
    ESP_LOGI(TAG, "Loading alarms from NVS...");

    size_t required_size = 0;
    esp_err_t ret = nvs_get_str(alarm_nvs_handle, "alarms", NULL, &required_size);
    if (ret != ESP_OK || required_size == 0) {
        ESP_LOGW(TAG, "No alarms in NVS");
        return ESP_ERR_NOT_FOUND;
    }

    char *json_str = malloc(required_size);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = nvs_get_str(alarm_nvs_handle, "alarms", json_str, &required_size);
    if (ret != ESP_OK) {
        free(json_str);
        return ret;
    }

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse alarms JSON");
        return ESP_FAIL;
    }

    alarm_count = 0;
    cJSON *alarm_json;
    cJSON_ArrayForEach(alarm_json, root) {
        if (alarm_count >= MAX_ALARMS) break;

        alarm_t *alarm = &alarms[alarm_count];

        cJSON *id = cJSON_GetObjectItem(alarm_json, "id");
        cJSON *name = cJSON_GetObjectItem(alarm_json, "name");
        cJSON *enabled = cJSON_GetObjectItem(alarm_json, "enabled");
        cJSON *hour = cJSON_GetObjectItem(alarm_json, "hour");
        cJSON *minute = cJSON_GetObjectItem(alarm_json, "minute");
        cJSON *days = cJSON_GetObjectItem(alarm_json, "days");
        cJSON *source = cJSON_GetObjectItem(alarm_json, "source");
        cJSON *uri = cJSON_GetObjectItem(alarm_json, "uri");
        cJSON *volume = cJSON_GetObjectItem(alarm_json, "volume");
        cJSON *snooze = cJSON_GetObjectItem(alarm_json, "snooze");

        if (id && hour && minute) {
            alarm->id = id->valueint;
            alarm->enabled = enabled ? cJSON_IsTrue(enabled) : false;
            alarm->hour = hour->valueint;
            alarm->minute = minute->valueint;
            alarm->days = days ? days->valueint : ALARM_DAY_EVERYDAY;
            alarm->source = source ? source->valueint : ALARM_SOURCE_RADIO;
            alarm->volume = volume ? volume->valueint : 50;
            alarm->snooze_minutes = snooze ? snooze->valueint : 5;

            if (name && cJSON_IsString(name)) {
                strncpy(alarm->name, name->valuestring, sizeof(alarm->name) - 1);
            }
            if (uri && cJSON_IsString(uri)) {
                strncpy(alarm->source_uri, uri->valuestring, sizeof(alarm->source_uri) - 1);
            }

            alarm_count++;
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Loaded %d alarms from NVS", alarm_count);
    return ESP_OK;
}

esp_err_t alarm_manager_get_sounds(char ***sounds, uint8_t *count)
{
    // TODO: Skanuj kartę SD w poszukiwaniu plików dźwiękowych
    *sounds = NULL;
    *count = 0;
    return ESP_OK;
}
