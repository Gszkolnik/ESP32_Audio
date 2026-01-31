/*
 * ESP32 Audio Player - Main Application
 * For ESP32-LyraT V4.3
 *
 * Features:
 * - Internet Radio streaming
 * - Web interface control
 * - Home Assistant integration (MQTT)
 * - Spotify Web API control
 * - Alarm clock with NTP sync
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"

#include "board.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"

#include "config.h"
#include "audio_player.h"
#include "audio_settings.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "app_mqtt.h"
#include "radio_stations.h"
#include "alarm_manager.h"
#include "spotify_api.h"
#include "tone_generator.h"
#include "ota_update.h"

static const char *TAG = "MAIN";

// Event group dla synchronizacji startowej
static EventGroupHandle_t app_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define TIME_SYNCED_BIT    BIT1

// Aktualny stan aplikacji
static player_status_t current_status;

// ============================================
// Callbacki
// ============================================

static void wifi_state_handler(wifi_state_t state, const char *ip)
{
    switch (state) {
        case WIFI_STATE_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected, IP: %s", ip);
            xEventGroupSetBits(app_event_group, WIFI_CONNECTED_BIT);
            mqtt_publish_availability(true);
            break;
        case WIFI_STATE_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected");
            xEventGroupClearBits(app_event_group, WIFI_CONNECTED_BIT);
            break;
        default:
            break;
    }
}

static void player_state_handler(player_status_t *status)
{
    memcpy(&current_status, status, sizeof(player_status_t));

    // Publikuj stan do MQTT (Home Assistant)
    const char *state_str = "idle";
    switch (status->state) {
        case PLAYER_STATE_PLAYING: state_str = "playing"; break;
        case PLAYER_STATE_PAUSED:  state_str = "paused"; break;
        case PLAYER_STATE_STOPPED: state_str = "idle"; break;
        default: break;
    }
    mqtt_publish_state(state_str);
    mqtt_publish_volume(status->volume);
    mqtt_publish_media_info(status->current_title, status->current_artist, "");

    // Wyślij aktualizację do WebSocket
    char json[512];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"volume\":%d,\"muted\":%s,\"title\":\"%s\",\"artist\":\"%s\"}",
        state_str, status->volume, status->muted ? "true" : "false",
        status->current_title, status->current_artist);
    web_server_send_state_update(json);
}

static void mqtt_command_handler(mqtt_command_t *cmd)
{
    ESP_LOGI(TAG, "MQTT command: %d", cmd->type);

    switch (cmd->type) {
        case MQTT_CMD_PLAY:
            audio_player_resume();
            break;
        case MQTT_CMD_PAUSE:
            audio_player_pause();
            break;
        case MQTT_CMD_STOP:
            audio_player_stop();
            break;
        case MQTT_CMD_VOLUME_SET:
            audio_player_set_volume(cmd->value);
            break;
        case MQTT_CMD_VOLUME_UP:
            audio_player_set_volume(audio_player_get_volume() + 5);
            break;
        case MQTT_CMD_VOLUME_DOWN:
            audio_player_set_volume(audio_player_get_volume() - 5);
            break;
        case MQTT_CMD_MUTE:
            audio_player_mute(cmd->value);
            break;
        case MQTT_CMD_PLAY_MEDIA:
            audio_player_play_url(cmd->data);
            break;
        default:
            ESP_LOGW(TAG, "Unknown MQTT command");
            break;
    }
}

static void alarm_trigger_handler(alarm_t *alarm)
{
    ESP_LOGI(TAG, "Alarm triggered: %s", alarm->name);

    // Ustaw głośność alarmu
    audio_player_set_volume(alarm->volume);

    // Odtwórz źródło alarmu
    switch (alarm->source) {
        case ALARM_SOURCE_TONE:
            // Wbudowany ton
            tone_generator_play(alarm->tone_type, alarm->volume);
            break;
        case ALARM_SOURCE_RADIO:
            audio_player_play_url(alarm->source_uri);
            break;
        case ALARM_SOURCE_SOUND:
            audio_player_play_sdcard(alarm->source_uri);
            break;
        case ALARM_SOURCE_SPOTIFY:
            spotify_api_play_uri(alarm->source_uri);
            break;
    }
}

// ============================================
// Inicjalizacja komponentów
// ============================================

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_board(void)
{
    ESP_LOGI(TAG, "Initializing board: ESP32-LyraT V4.3");

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) return ret;

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) return ret;

    // Inicjalizacja audio board (ESP-ADF)
    audio_board_handle_t board_handle = audio_board_init();
    if (board_handle == NULL) {
        ESP_LOGE(TAG, "Failed to init audio board");
        return ESP_FAIL;
    }

    // Konfiguracja kodeków audio
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    return ESP_OK;
}

// ============================================
// Factory Reset via physical buttons
// ============================================

static void init_factory_reset_buttons(void)
{
    // GPIO36 i GPIO39 to piny input-only, nie wymagają pull-up (są zewnętrzne na płytce)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_REC_GPIO) | (1ULL << BUTTON_MODE_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static bool are_factory_reset_buttons_pressed(void)
{
    // Przyciski na ESP32-LyraT są aktywne LOW (wciśnięty = 0)
    return (gpio_get_level(BUTTON_REC_GPIO) == 0) && (gpio_get_level(BUTTON_MODE_GPIO) == 0);
}

static void play_factory_reset_beeps(void)
{
    ESP_LOGI(TAG, "Playing factory reset confirmation beeps...");

    // Inicjalizuj tone generator jeśli jeszcze nie jest
    if (tone_generator_init() == ESP_OK) {
        for (int i = 0; i < FACTORY_RESET_BEEP_COUNT; i++) {
            tone_generator_beep(FACTORY_RESET_BEEP_FREQ, 200, 80);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}

static void check_factory_reset(void)
{
    init_factory_reset_buttons();

    // Sprawdź czy oba przyciski są wciśnięte
    if (!are_factory_reset_buttons_pressed()) {
        ESP_LOGI(TAG, "Factory reset buttons not pressed, continuing normal boot...");
        return;
    }

    ESP_LOGW(TAG, "Factory reset buttons detected! Hold for %d seconds to reset...",
             FACTORY_RESET_HOLD_TIME_MS / 1000);

    // Sprawdzaj co 100ms przez 5 sekund
    int check_intervals = FACTORY_RESET_HOLD_TIME_MS / 100;
    for (int i = 0; i < check_intervals; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (!are_factory_reset_buttons_pressed()) {
            ESP_LOGI(TAG, "Factory reset cancelled - buttons released");
            return;
        }

        // Co sekundę wyświetl countdown
        if ((i + 1) % 10 == 0) {
            int seconds_left = (FACTORY_RESET_HOLD_TIME_MS - (i + 1) * 100) / 1000;
            ESP_LOGW(TAG, "Factory reset in %d seconds...", seconds_left);
        }
    }

    // Przyciski trzymane przez cały czas - wykonaj factory reset
    ESP_LOGW(TAG, "===========================================");
    ESP_LOGW(TAG, "  FACTORY RESET TRIGGERED!");
    ESP_LOGW(TAG, "  Erasing all settings...");
    ESP_LOGW(TAG, "===========================================");

    // Odtwórz sygnał dźwiękowy
    play_factory_reset_beeps();

    // Wyczyść całe NVS
    esp_err_t ret = nvs_flash_erase();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS erased successfully");
    } else {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
    }

    // Reinicjalizuj NVS (puste)
    nvs_flash_init();

    ESP_LOGW(TAG, "Factory reset complete. Restarting in AP mode...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Restart urządzenia
    esp_restart();
}

// ============================================
// Main
// ============================================

void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "  ESP32 Audio Player v%s", DEVICE_VERSION);
    ESP_LOGI(TAG, "  Board: ESP32-LyraT V4.3");
    ESP_LOGI(TAG, "=================================");

    // Event group
    app_event_group = xEventGroupCreate();

    // 1. Inicjalizacja NVS
    ESP_ERROR_CHECK(init_nvs());
    ESP_LOGI(TAG, "NVS initialized");

    // 1b. Inicjalizacja ustawień audio (przed audio_player_init!)
    ESP_ERROR_CHECK(audio_settings_init());
    ESP_LOGI(TAG, "Audio settings initialized");

    // 2. Inicjalizacja płytki audio
    ESP_ERROR_CHECK(init_board());
    ESP_LOGI(TAG, "Audio board initialized");

    // 2b. Sprawdź czy uruchomiono factory reset (Rec + Mode przez 5s)
    check_factory_reset();

    // 3. Inicjalizacja WiFi - auto-połączenie z zapisanych danych lub AP mode
    ESP_ERROR_CHECK(wifi_manager_init());
    wifi_manager_register_callback(wifi_state_handler);

    // Próba auto-połączenia z zapisanych danych WiFi
    // Jeśli brak zapisanych danych -> uruchomienie AP mode na 192.168.1.1
    esp_err_t wifi_ret = wifi_manager_auto_connect();
    if (wifi_ret != ESP_OK && wifi_manager_get_state() != WIFI_STATE_AP_MODE) {
        // Jeśli auto_connect nie zadziałał i nie jesteśmy w AP mode, spróbuj z config.h
        ESP_LOGI(TAG, "Trying default WiFi credentials...");
        wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD);
    }
    ESP_LOGI(TAG, "WiFi manager initialized");

    // 4. Czekaj na połączenie WiFi (max 30 sekund) jeśli nie w AP mode
    if (wifi_manager_get_state() != WIFI_STATE_AP_MODE) {
        EventBits_t bits = xEventGroupWaitBits(app_event_group, WIFI_CONNECTED_BIT,
                                                pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
        if (!(bits & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "WiFi connection timeout, starting AP mode...");
            wifi_manager_start_ap(DEVICE_NAME, "");
        }
    }

    // 5. Inicjalizacja audio playera
    ESP_ERROR_CHECK(audio_player_init());
    audio_player_register_callback(player_state_handler);

    // Ustaw głośność z zapisanych ustawień audio
    audio_settings_t *audio_cfg = audio_settings_get();
    audio_player_set_volume(audio_cfg->volume);
    ESP_LOGI(TAG, "Audio player initialized (volume: %d)", audio_cfg->volume);

    // 5b. Inicjalizacja generatora tonów
    ESP_ERROR_CHECK(tone_generator_init());
    ESP_LOGI(TAG, "Tone generator initialized");

    // 6. Ładowanie stacji radiowych
    ESP_ERROR_CHECK(radio_stations_init());
    if (radio_stations_load() != ESP_OK) {
        ESP_LOGI(TAG, "Loading default radio stations");
        radio_stations_load_defaults();
    }
    ESP_LOGI(TAG, "Radio stations loaded");

    // 7. Inicjalizacja serwera WWW
    ESP_ERROR_CHECK(web_server_init());
    ESP_LOGI(TAG, "Web server started on port %d", WEB_SERVER_PORT);

    // 7a. Inicjalizacja OTA
    ESP_ERROR_CHECK(ota_update_init());
    ESP_LOGI(TAG, "OTA update module initialized, version: %s", ota_update_get_version());

    // 8. Inicjalizacja MQTT (Home Assistant)
    ESP_ERROR_CHECK(app_mqtt_client_init(MQTT_SERVER_DEFAULT, MQTT_PORT_DEFAULT,
                                          MQTT_USER_DEFAULT, MQTT_PASSWORD_DEFAULT));
    mqtt_register_command_callback(mqtt_command_handler);
    ESP_ERROR_CHECK(app_mqtt_client_connect());
    ESP_LOGI(TAG, "MQTT client initialized");

    // 9. Home Assistant Auto Discovery
    vTaskDelay(pdMS_TO_TICKS(1000));  // Poczekaj na stabilne połączenie
    mqtt_send_ha_discovery();
    mqtt_publish_availability(true);
    ESP_LOGI(TAG, "Home Assistant discovery sent");

    // 10. Inicjalizacja alarm manager z NTP
    ESP_ERROR_CHECK(alarm_manager_init());
    alarm_manager_register_callback(alarm_trigger_handler);
    alarm_manager_sync_time();
    alarm_manager_load();
    ESP_LOGI(TAG, "Alarm manager initialized");

    // 11. Autostart - odtwórz ostatnią stację jeśli włączone
    if (audio_settings_get_autostart()) {
        const char *last_url = audio_settings_get_last_url();
        if (last_url && strlen(last_url) > 0) {
            ESP_LOGI(TAG, "Autostart enabled, playing last station: %s", last_url);
            vTaskDelay(pdMS_TO_TICKS(2000));  // Poczekaj na stabilność
            audio_player_play_url(last_url);
        }
    }

    // 11. Inicjalizacja Spotify API (opcjonalnie)
    // spotify_api_init(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET);
    // spotify_api_load_tokens();

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "  System ready!");
    ESP_LOGI(TAG, "  Web UI: http://%s", wifi_manager_get_ip());
    ESP_LOGI(TAG, "=================================");

    // Główna pętla - obsługa zdarzeń
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Okresowe zadania
        static int counter = 0;
        counter++;

        // Co 60 sekund - sync NTP
        if (counter % 60 == 0) {
            if (!alarm_manager_is_time_synced()) {
                alarm_manager_sync_time();
            }
        }

        // Co 30 sekund - heartbeat MQTT
        if (counter % 30 == 0) {
            if (app_mqtt_get_state() == MQTT_STATE_CONNECTED) {
                mqtt_publish_availability(true);
            }
        }
    }
}
