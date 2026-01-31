#ifndef CONFIG_H
#define CONFIG_H

#include "../credentials.h"

// ============================================
// Konfiguracja urządzenia
// ============================================
#define DEVICE_NAME "ESP32-AudioPlayer"
#define DEVICE_VERSION "1.0.0"

// ============================================
// Przyciski fizyczne (ESP32-LyraT V4.3)
// ============================================
#define BUTTON_REC_GPIO         36      // GPIO36 (ADC1_CH0) - input only
#define BUTTON_MODE_GPIO        39      // GPIO39 (ADC1_CH3) - input only
#define BUTTON_PLAY_GPIO        -1      // Touch - obsługiwany przez ESP-ADF
#define BUTTON_SET_GPIO         -1      // Touch - obsługiwany przez ESP-ADF
#define BUTTON_VOL_UP_GPIO      -1      // Touch - obsługiwany przez ESP-ADF
#define BUTTON_VOL_DOWN_GPIO    -1      // Touch - obsługiwany przez ESP-ADF

// Factory Reset - trzymaj Rec + Mode przez 5 sekund podczas startu
#define FACTORY_RESET_HOLD_TIME_MS  5000
#define FACTORY_RESET_BEEP_FREQ     1000
#define FACTORY_RESET_BEEP_COUNT    3

// ============================================
// Detekcja słuchawek i AUX (ESP32-LyraT V4.3)
// ============================================
#define HEADPHONE_DETECT_GPIO       19      // GPIO19 - headphone jack detect
#define AUX_DETECT_GPIO             21      // GPIO21 - AUX jack detect (DIP SW7 = ON)

// ============================================
// Monitorowanie baterii (wymaga modyfikacji HW)
// Podłącz dzielnik napięcia 100k/100k z BAT+ do GPIO34
// Opcjonalnie: CHRG i STDBY z AP5056 do GPIO
// ============================================
#define BATTERY_ADC_CHANNEL         ADC1_CHANNEL_6  // GPIO34
// #define BATTERY_CHRG_GPIO        34      // Opcjonalne: AP5056 CHRG pin
// #define BATTERY_STDBY_GPIO       35      // Opcjonalne: AP5056 STDBY pin
// #define USB_DETECT_GPIO          -1      // Opcjonalne: wykrywanie USB

// ============================================
// Konfiguracja Bluetooth
// ============================================
#define BT_DEVICE_NAME              DEVICE_NAME
#define BT_DISCOVERABLE_TIMEOUT     300     // Sekund (0 = zawsze widoczny)

// ============================================
// Konfiguracja karty SD
// ============================================
#define SD_MOUNT_POINT              "/sdcard"
#define SD_MAX_FILES                5

// ============================================
// Konfiguracja Audio
// ============================================
#define DEFAULT_VOLUME 50
#define MAX_VOLUME 100
#define MIN_VOLUME 0

// ============================================
// Konfiguracja Web Server
// ============================================
#define WEB_SERVER_PORT 80

// ============================================
// Konfiguracja MQTT (Home Assistant)
// ============================================
#define MQTT_TOPIC_BASE "esp32_audio"

// Publikowanie stanu (ESP32 -> HA)
#define MQTT_TOPIC_STATE            MQTT_TOPIC_BASE "/state"
#define MQTT_TOPIC_STATE_PLAYER     MQTT_TOPIC_BASE "/state/player"
#define MQTT_TOPIC_STATE_VOLUME     MQTT_TOPIC_BASE "/state/volume"
#define MQTT_TOPIC_STATE_MEDIA      MQTT_TOPIC_BASE "/state/media"
#define MQTT_TOPIC_STATE_EQ         MQTT_TOPIC_BASE "/state/eq"
#define MQTT_TOPIC_STATE_STATIONS   MQTT_TOPIC_BASE "/state/stations"
#define MQTT_TOPIC_STATE_ALARMS     MQTT_TOPIC_BASE "/state/alarms"
#define MQTT_TOPIC_AVAILABILITY     MQTT_TOPIC_BASE "/availability"

// Komendy (HA -> ESP32)
#define MQTT_TOPIC_CMD              MQTT_TOPIC_BASE "/cmd"
#define MQTT_TOPIC_CMD_PLAYER       MQTT_TOPIC_BASE "/cmd/player"      // play, pause, stop
#define MQTT_TOPIC_CMD_VOLUME       MQTT_TOPIC_BASE "/cmd/volume"      // 0-100 lub up/down/mute
#define MQTT_TOPIC_CMD_STATION      MQTT_TOPIC_BASE "/cmd/station"     // id lub url
#define MQTT_TOPIC_CMD_EQ           MQTT_TOPIC_BASE "/cmd/eq"          // preset lub bands[]
#define MQTT_TOPIC_CMD_ALARM        MQTT_TOPIC_BASE "/cmd/alarm"       // enable/disable/stop/snooze
#define MQTT_TOPIC_CMD_SYSTEM       MQTT_TOPIC_BASE "/cmd/system"      // reboot, status

// Home Assistant Auto Discovery
#define MQTT_TOPIC_HA_CONFIG "homeassistant/media_player/esp32_audio/config"

// ============================================
// Konfiguracja NTP
// ============================================
#define NTP_SERVER "pool.ntp.org"
#define NTP_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"  // Europa/Warszawa

// ============================================
// Konfiguracja Alarmów
// ============================================
#define MAX_ALARMS 10
#define ALARM_SOUNDS_PATH "/sdcard/alarms"
#define ALARM_AUTO_STOP_MINUTES     5       // Auto-stop alarmu po X minutach
#define ALARM_DEFAULT_SNOOZE_MINUTES 5      // Domyślny czas drzemki

// ============================================
// Konfiguracja Stacji Radiowych
// ============================================
#define MAX_RADIO_STATIONS 50
#define STATIONS_NVS_NAMESPACE "radio_stations"

// ============================================
// Konfiguracja Spotify
// ============================================
#define SPOTIFY_AUTH_URL "https://accounts.spotify.com/authorize"
#define SPOTIFY_TOKEN_URL "https://accounts.spotify.com/api/token"
#define SPOTIFY_API_URL "https://api.spotify.com/v1"

#endif // CONFIG_H
