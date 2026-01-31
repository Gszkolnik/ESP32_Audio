/*
 * MQTT Client Module
 * Home Assistant integration via MQTT with auto-discovery
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"  // ESP-IDF MQTT client
#include "app_mqtt.h"     // Local header - app MQTT interface
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "config.h"

#define MQTT_NVS_NAMESPACE "mqtt_settings"

static const char *TAG = "MQTT_CLIENT";

// MQTT client handle
static esp_mqtt_client_handle_t mqtt_client = NULL;

// Stan
static mqtt_state_t current_state = MQTT_STATE_DISCONNECTED;

// Callback
static mqtt_command_callback_t command_callback = NULL;

// ============================================
// MQTT Event Handler
// ============================================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            current_state = MQTT_STATE_CONNECTED;

            // Subskrybuj topic komend
            esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_CMD, 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            current_state = MQTT_STATE_DISCONNECTED;
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            current_state = MQTT_STATE_ERROR;
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT data received on topic: %.*s",
                     event->topic_len, event->topic);

            // Parsowanie komendy
            if (strncmp(event->topic, MQTT_TOPIC_CMD, event->topic_len) == 0) {
                char *data = malloc(event->data_len + 1);
                memcpy(data, event->data, event->data_len);
                data[event->data_len] = '\0';

                cJSON *root = cJSON_Parse(data);
                if (root && command_callback) {
                    mqtt_command_t cmd = {0};

                    cJSON *action = cJSON_GetObjectItem(root, "action");
                    if (action && cJSON_IsString(action)) {
                        if (strcmp(action->valuestring, "play") == 0) {
                            cmd.type = MQTT_CMD_PLAY;
                        } else if (strcmp(action->valuestring, "pause") == 0) {
                            cmd.type = MQTT_CMD_PAUSE;
                        } else if (strcmp(action->valuestring, "stop") == 0) {
                            cmd.type = MQTT_CMD_STOP;
                        } else if (strcmp(action->valuestring, "volume_set") == 0) {
                            cmd.type = MQTT_CMD_VOLUME_SET;
                            cJSON *vol = cJSON_GetObjectItem(root, "volume");
                            if (vol) cmd.value = vol->valueint;
                        } else if (strcmp(action->valuestring, "volume_up") == 0) {
                            cmd.type = MQTT_CMD_VOLUME_UP;
                        } else if (strcmp(action->valuestring, "volume_down") == 0) {
                            cmd.type = MQTT_CMD_VOLUME_DOWN;
                        } else if (strcmp(action->valuestring, "play_media") == 0) {
                            cmd.type = MQTT_CMD_PLAY_MEDIA;
                            cJSON *url = cJSON_GetObjectItem(root, "media_content_id");
                            if (url && cJSON_IsString(url)) {
                                strncpy(cmd.data, url->valuestring, sizeof(cmd.data) - 1);
                            }
                        }

                        command_callback(&cmd);
                    }

                    cJSON_Delete(root);
                }

                free(data);
            }
            break;

        default:
            break;
    }
}

// ============================================
// Publiczne API
// ============================================

esp_err_t app_mqtt_client_init(const char *server, uint16_t port,
                                const char *user, const char *password)
{
    ESP_LOGI(TAG, "Initializing MQTT client...");

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", server, port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = user,
        .credentials.authentication.password = password,
        .session.keepalive = 60,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    ESP_LOGI(TAG, "MQTT client initialized");
    return ESP_OK;
}

esp_err_t app_mqtt_client_deinit(void)
{
    if (mqtt_client) {
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }
    return ESP_OK;
}

esp_err_t app_mqtt_client_connect(void)
{
    if (mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Connecting to MQTT broker...");
    current_state = MQTT_STATE_CONNECTING;

    return esp_mqtt_client_start(mqtt_client);
}

esp_err_t app_mqtt_client_disconnect(void)
{
    if (mqtt_client) {
        return esp_mqtt_client_stop(mqtt_client);
    }
    return ESP_OK;
}

esp_err_t mqtt_publish_state(const char *state)
{
    if (current_state != MQTT_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state);

    char *json = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATE, json, 0, 1, true);

    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t mqtt_publish_volume(int volume)
{
    if (current_state != MQTT_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[32];
    snprintf(payload, sizeof(payload), "{\"volume\":%d}", volume);

    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATE "/volume",
                            payload, 0, 1, true);
    return ESP_OK;
}

esp_err_t mqtt_publish_media_info(const char *title, const char *artist, const char *album)
{
    if (current_state != MQTT_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "media_title", title ? title : "");
    cJSON_AddStringToObject(root, "media_artist", artist ? artist : "");
    cJSON_AddStringToObject(root, "media_album_name", album ? album : "");

    char *json = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATE "/media",
                            json, 0, 1, true);

    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t mqtt_publish_availability(bool online)
{
    if (mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_AVAILABILITY,
                            online ? "online" : "offline", 0, 1, true);
    return ESP_OK;
}

esp_err_t mqtt_send_ha_discovery(void)
{
    if (current_state != MQTT_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Sending Home Assistant discovery config...");

    cJSON *root = cJSON_CreateObject();

    // Podstawowe info
    cJSON_AddStringToObject(root, "name", DEVICE_NAME);
    cJSON_AddStringToObject(root, "unique_id", "esp32_audio_player_001");
    cJSON_AddStringToObject(root, "object_id", "esp32_audio");

    // Topics
    cJSON_AddStringToObject(root, "state_topic", MQTT_TOPIC_STATE);
    cJSON_AddStringToObject(root, "command_topic", MQTT_TOPIC_CMD);
    cJSON_AddStringToObject(root, "availability_topic", MQTT_TOPIC_AVAILABILITY);

    // Mapowanie wartości
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    // Wspierane funkcje
    cJSON *supported = cJSON_CreateArray();
    cJSON_AddItemToArray(supported, cJSON_CreateString("play"));
    cJSON_AddItemToArray(supported, cJSON_CreateString("pause"));
    cJSON_AddItemToArray(supported, cJSON_CreateString("stop"));
    cJSON_AddItemToArray(supported, cJSON_CreateString("volume_set"));
    cJSON_AddItemToArray(supported, cJSON_CreateString("volume_step"));
    cJSON_AddItemToArray(supported, cJSON_CreateString("play_media"));
    cJSON_AddItemToObject(root, "supported_features", supported);

    // Device info
    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "name", DEVICE_NAME);
    cJSON_AddStringToObject(device, "manufacturer", "Custom");
    cJSON_AddStringToObject(device, "model", "ESP32-LyraT V4.3");
    cJSON_AddStringToObject(device, "sw_version", DEVICE_VERSION);

    cJSON *identifiers = cJSON_CreateArray();
    cJSON_AddItemToArray(identifiers, cJSON_CreateString("esp32_audio_001"));
    cJSON_AddItemToObject(device, "identifiers", identifiers);
    cJSON_AddItemToObject(root, "device", device);

    char *json = cJSON_Print(root);
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_HA_CONFIG, json, 0, 1, true);

    ESP_LOGI(TAG, "Discovery config sent");

    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

mqtt_state_t app_mqtt_get_state(void)
{
    return current_state;
}

void mqtt_register_command_callback(mqtt_command_callback_t callback)
{
    command_callback = callback;
}

// ============================================
// NVS - Zapis/odczyt ustawień MQTT
// ============================================

esp_err_t mqtt_settings_save(const mqtt_settings_t *settings)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for MQTT settings");
        return ret;
    }

    nvs_set_str(nvs_handle, "server", settings->server);
    nvs_set_u16(nvs_handle, "port", settings->port);
    nvs_set_str(nvs_handle, "user", settings->user);
    nvs_set_str(nvs_handle, "password", settings->password);
    nvs_set_u8(nvs_handle, "auto_connect", settings->auto_connect ? 1 : 0);

    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "MQTT settings saved: server=%s, port=%d", settings->server, settings->port);
    return ret;
}

esp_err_t mqtt_settings_load(mqtt_settings_t *settings)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t size = sizeof(settings->server);
    nvs_get_str(nvs_handle, "server", settings->server, &size);

    nvs_get_u16(nvs_handle, "port", &settings->port);

    size = sizeof(settings->user);
    nvs_get_str(nvs_handle, "user", settings->user, &size);

    size = sizeof(settings->password);
    nvs_get_str(nvs_handle, "password", settings->password, &size);

    uint8_t auto_conn = 0;
    if (nvs_get_u8(nvs_handle, "auto_connect", &auto_conn) == ESP_OK) {
        settings->auto_connect = auto_conn;
    }

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "MQTT settings loaded: server=%s, port=%d", settings->server, settings->port);
    return ESP_OK;
}

esp_err_t mqtt_settings_clear(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_all(nvs_handle);
    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "MQTT settings cleared");
    return ret;
}

bool mqtt_has_saved_settings(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return false;
    }

    size_t required_size = 0;
    ret = nvs_get_str(nvs_handle, "server", NULL, &required_size);
    nvs_close(nvs_handle);

    return (ret == ESP_OK && required_size > 1);
}

esp_err_t mqtt_auto_connect(void)
{
    if (!mqtt_has_saved_settings()) {
        ESP_LOGI(TAG, "No saved MQTT settings");
        return ESP_ERR_NOT_FOUND;
    }

    mqtt_settings_t settings = {0};
    settings.port = 1883;  // default

    esp_err_t ret = mqtt_settings_load(&settings);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!settings.auto_connect) {
        ESP_LOGI(TAG, "MQTT auto-connect disabled");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "MQTT auto-connecting to %s:%d", settings.server, settings.port);
    return app_mqtt_client_init(settings.server, settings.port, settings.user, settings.password);
}
