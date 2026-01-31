#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include "esp_err.h"
#include <time.h>

// Typ źródła alarmu
typedef enum {
    ALARM_SOURCE_TONE = 0,      // Wbudowany ton
    ALARM_SOURCE_RADIO,         // Stacja radiowa
    ALARM_SOURCE_SOUND,         // Plik z karty SD
    ALARM_SOURCE_SPOTIFY,       // Spotify
} alarm_source_t;

// Dni tygodnia (bitmask)
#define ALARM_DAY_MONDAY    (1 << 0)
#define ALARM_DAY_TUESDAY   (1 << 1)
#define ALARM_DAY_WEDNESDAY (1 << 2)
#define ALARM_DAY_THURSDAY  (1 << 3)
#define ALARM_DAY_FRIDAY    (1 << 4)
#define ALARM_DAY_SATURDAY  (1 << 5)
#define ALARM_DAY_SUNDAY    (1 << 6)
#define ALARM_DAY_WEEKDAYS  (ALARM_DAY_MONDAY | ALARM_DAY_TUESDAY | ALARM_DAY_WEDNESDAY | ALARM_DAY_THURSDAY | ALARM_DAY_FRIDAY)
#define ALARM_DAY_WEEKEND   (ALARM_DAY_SATURDAY | ALARM_DAY_SUNDAY)
#define ALARM_DAY_EVERYDAY  (ALARM_DAY_WEEKDAYS | ALARM_DAY_WEEKEND)

// Struktura alarmu
typedef struct {
    uint8_t id;
    bool enabled;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;           // Bitmask dni tygodnia
    alarm_source_t source;
    uint8_t tone_type;      // Typ wbudowanego tonu (dla ALARM_SOURCE_TONE)
    char source_uri[256];   // URL radia lub ścieżka do pliku
    uint8_t volume;
    uint8_t snooze_minutes;
    char name[32];
} alarm_t;

// Callback wywoływany gdy alarm się włącza
typedef void (*alarm_trigger_callback_t)(alarm_t *alarm);

// Inicjalizacja
esp_err_t alarm_manager_init(void);

// Synchronizacja czasu NTP
esp_err_t alarm_manager_sync_time(void);
bool alarm_manager_is_time_synced(void);
time_t alarm_manager_get_time(void);

// Zarządzanie alarmami
esp_err_t alarm_manager_add(alarm_t *alarm);
esp_err_t alarm_manager_remove(uint8_t id);
esp_err_t alarm_manager_update(alarm_t *alarm);
esp_err_t alarm_manager_enable(uint8_t id, bool enable);

// Pobieranie alarmów
alarm_t *alarm_manager_get(uint8_t id);
alarm_t *alarm_manager_get_all(uint8_t *count);
alarm_t *alarm_manager_get_next(void);

// Snooze i stop
esp_err_t alarm_manager_snooze(void);
esp_err_t alarm_manager_stop_alarm(void);
bool alarm_manager_is_alarm_active(void);
alarm_t *alarm_manager_get_active_alarm(void);

// Callback
void alarm_manager_register_callback(alarm_trigger_callback_t callback);

// Zapisywanie/ładowanie z NVS
esp_err_t alarm_manager_save(void);
esp_err_t alarm_manager_load(void);

// Lista dostępnych dźwięków budzika
esp_err_t alarm_manager_get_sounds(char ***sounds, uint8_t *count);

#endif // ALARM_MANAGER_H
