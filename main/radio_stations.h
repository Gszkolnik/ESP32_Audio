#ifndef RADIO_STATIONS_H
#define RADIO_STATIONS_H

#include "esp_err.h"
#include "config.h"

// Struktura stacji radiowej
typedef struct {
    uint8_t id;
    char name[64];
    char url[256];
    char logo_url[256];
    bool favorite;
} radio_station_t;

// Inicjalizacja modułu stacji
esp_err_t radio_stations_init(void);

// Zarządzanie stacjami
esp_err_t radio_stations_add(const char *name, const char *url, const char *logo_url);
esp_err_t radio_stations_remove(uint8_t id);
esp_err_t radio_stations_update(uint8_t id, const char *name, const char *url, const char *logo_url);
esp_err_t radio_stations_set_favorite(uint8_t id, bool favorite);

// Pobieranie stacji
radio_station_t *radio_stations_get(uint8_t id);
radio_station_t *radio_stations_get_all(uint8_t *count);
radio_station_t *radio_stations_get_favorites(uint8_t *count);

// Domyślne stacje
esp_err_t radio_stations_load_defaults(void);

// Zapisywanie/ładowanie z NVS
esp_err_t radio_stations_save(void);
esp_err_t radio_stations_load(void);

#endif // RADIO_STATIONS_H
