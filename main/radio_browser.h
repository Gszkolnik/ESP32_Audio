/*
 * Radio Browser API Module
 * Integration with radio-browser.info API for searching radio stations
 */

#ifndef RADIO_BROWSER_H
#define RADIO_BROWSER_H

#include "esp_err.h"
#include <stdbool.h>

// Maksymalna liczba wynikow wyszukiwania (ograniczenie pamieci ESP32)
#define RADIO_BROWSER_MAX_RESULTS 20

// Struktura wyniku wyszukiwania
typedef struct {
    char name[64];
    char url[256];
    char country[32];
    char tags[64];
    int bitrate;
    int votes;
} radio_browser_station_t;

// Inicjalizacja modulu
esp_err_t radio_browser_init(void);

// Wyszukiwanie stacji po nazwie
// Wyniki zapisywane do tablicy results, zwraca liczbe znalezionych
int radio_browser_search_by_name(const char *name, const char *country_code,
                                  radio_browser_station_t *results, int max_results);

// Wyszukiwanie stacji po kraju
int radio_browser_search_by_country(const char *country_code,
                                     radio_browser_station_t *results, int max_results);

// Wyszukiwanie stacji po tagu/gatunku
int radio_browser_search_by_tag(const char *tag, const char *country_code,
                                 radio_browser_station_t *results, int max_results);

// Pobierz liste krajow (top 20 najczesciej uzywanych)
int radio_browser_get_countries(char countries[][32], int max_countries);

// Pobierz popularne stacje
int radio_browser_get_top_stations(const char *country_code,
                                    radio_browser_station_t *results, int max_results);

#endif // RADIO_BROWSER_H
