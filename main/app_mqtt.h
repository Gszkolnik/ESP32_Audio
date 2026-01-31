#ifndef APP_MQTT_H
#define APP_MQTT_H

#include "esp_err.h"

// Stan MQTT
typedef enum {
    MQTT_STATE_DISCONNECTED = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_ERROR,
} mqtt_state_t;

// Typy komend MQTT od Home Assistant
typedef enum {
    // Sterowanie odtwarzaniem
    MQTT_CMD_PLAY = 0,
    MQTT_CMD_PAUSE,
    MQTT_CMD_STOP,
    MQTT_CMD_NEXT_STATION,
    MQTT_CMD_PREV_STATION,

    // Głośność
    MQTT_CMD_VOLUME_SET,
    MQTT_CMD_VOLUME_UP,
    MQTT_CMD_VOLUME_DOWN,
    MQTT_CMD_MUTE,

    // Media
    MQTT_CMD_PLAY_MEDIA,        // Odtwórz URL
    MQTT_CMD_PLAY_STATION,      // Odtwórz stację po ID
    MQTT_CMD_SELECT_SOURCE,     // radio/spotify/bluetooth

    // Equalizer
    MQTT_CMD_EQ_PRESET,         // Ustaw preset EQ
    MQTT_CMD_EQ_BAND,           // Ustaw pojedyncze pasmo
    MQTT_CMD_EQ_BASS_BOOST,
    MQTT_CMD_EQ_LOUDNESS,
    MQTT_CMD_BALANCE,

    // Alarmy
    MQTT_CMD_ALARM_ENABLE,
    MQTT_CMD_ALARM_DISABLE,
    MQTT_CMD_ALARM_STOP,        // Zatrzymaj aktywny alarm
    MQTT_CMD_ALARM_SNOOZE,

    // System
    MQTT_CMD_REBOOT,
    MQTT_CMD_GET_STATUS,
} mqtt_command_type_t;

// Struktura komendy
typedef struct {
    mqtt_command_type_t type;
    char data[256];
    int value;
} mqtt_command_t;

// Callback dla komend
typedef void (*mqtt_command_callback_t)(mqtt_command_t *cmd);

// Inicjalizacja
esp_err_t app_mqtt_client_init(const char *server, uint16_t port,
                                const char *user, const char *password);
esp_err_t app_mqtt_client_deinit(void);

// Połączenie
esp_err_t app_mqtt_client_connect(void);
esp_err_t app_mqtt_client_disconnect(void);

// Publikowanie stanu dla Home Assistant
esp_err_t mqtt_publish_state(const char *state);
esp_err_t mqtt_publish_volume(int volume);
esp_err_t mqtt_publish_media_info(const char *title, const char *artist, const char *album);
esp_err_t mqtt_publish_availability(bool online);

// Home Assistant Auto Discovery
esp_err_t mqtt_send_ha_discovery(void);

// Stan i callback
mqtt_state_t app_mqtt_get_state(void);
void mqtt_register_command_callback(mqtt_command_callback_t callback);

// Struktura ustawień MQTT
typedef struct {
    char server[64];
    uint16_t port;
    char user[32];
    char password[64];
    bool auto_connect;
} mqtt_settings_t;

// Zapis/odczyt ustawień z NVS
esp_err_t mqtt_settings_save(const mqtt_settings_t *settings);
esp_err_t mqtt_settings_load(mqtt_settings_t *settings);
esp_err_t mqtt_settings_clear(void);
bool mqtt_has_saved_settings(void);

// Auto-połączenie z zapisanych ustawień
esp_err_t mqtt_auto_connect(void);

#endif // APP_MQTT_H
