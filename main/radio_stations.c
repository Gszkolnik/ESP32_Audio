/*
 * Radio Stations Manager
 * Handles storage and management of internet radio stations
 */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#include "radio_stations.h"
#include "config.h"

static const char *TAG = "RADIO_STATIONS";

// Lista stacji
static radio_station_t stations[MAX_RADIO_STATIONS];
static uint8_t station_count = 0;

// NVS handle
static nvs_handle_t radio_nvs_handle;

// ============================================
// Domyślne stacje radiowe
// ============================================

static const radio_station_t default_stations[] = {
    {
        .id = 1,
        .name = "RMF FM",
        .url = "http://rs6-krk2.rmfstream.pl/rmf_fm",
        .logo_url = "",
        .favorite = true,
    },
    {
        .id = 2,
        .name = "VOX FM",
        .url = "http://ic1.smcdn.pl/3990-1.mp3",
        .logo_url = "",
        .favorite = true,
    },
    {
        .id = 3,
        .name = "Radio ZET",
        .url = "http://zt02.cdn.eurozet.pl/zet-old.mp3",
        .logo_url = "",
        .favorite = false,
    },
    {
        .id = 4,
        .name = "Eska Rock",
        .url = "http://ic1.smcdn.pl/2380-1.mp3",
        .logo_url = "",
        .favorite = false,
    },
    {
        .id = 5,
        .name = "Polskie Radio 3",
        .url = "http://mp3.polskieradio.pl:8956/",
        .logo_url = "",
        .favorite = false,
    },
    {
        .id = 6,
        .name = "Radioparty DJ Mixes",
        .url = "http://djmixes.radioparty.pl:8035/",
        .logo_url = "",
        .favorite = false,
    },
};

// ============================================
// Publiczne API
// ============================================

esp_err_t radio_stations_init(void)
{
    ESP_LOGI(TAG, "Initializing radio stations manager...");

    esp_err_t ret = nvs_open(STATIONS_NVS_NAMESPACE, NVS_READWRITE, &radio_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return ret;
    }

    memset(stations, 0, sizeof(stations));
    station_count = 0;

    ESP_LOGI(TAG, "Radio stations manager initialized");
    return ESP_OK;
}

esp_err_t radio_stations_add(const char *name, const char *url, const char *logo_url)
{
    if (station_count >= MAX_RADIO_STATIONS) {
        ESP_LOGE(TAG, "Maximum station count reached");
        return ESP_ERR_NO_MEM;
    }

    // Znajdź wolne ID
    uint8_t new_id = 1;
    for (int i = 0; i < station_count; i++) {
        if (stations[i].id >= new_id) {
            new_id = stations[i].id + 1;
        }
    }

    radio_station_t *station = &stations[station_count];
    station->id = new_id;
    strncpy(station->name, name, sizeof(station->name) - 1);
    strncpy(station->url, url, sizeof(station->url) - 1);
    if (logo_url) {
        strncpy(station->logo_url, logo_url, sizeof(station->logo_url) - 1);
    }
    station->favorite = false;

    station_count++;

    ESP_LOGI(TAG, "Added station: %s (ID: %d)", name, new_id);
    return radio_stations_save();
}

esp_err_t radio_stations_remove(uint8_t id)
{
    for (int i = 0; i < station_count; i++) {
        if (stations[i].id == id) {
            // Przesuń pozostałe stacje
            memmove(&stations[i], &stations[i + 1],
                    (station_count - i - 1) * sizeof(radio_station_t));
            station_count--;

            ESP_LOGI(TAG, "Removed station ID: %d", id);
            return radio_stations_save();
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t radio_stations_update(uint8_t id, const char *name, const char *url, const char *logo_url)
{
    for (int i = 0; i < station_count; i++) {
        if (stations[i].id == id) {
            if (name) strncpy(stations[i].name, name, sizeof(stations[i].name) - 1);
            if (url) strncpy(stations[i].url, url, sizeof(stations[i].url) - 1);
            if (logo_url) strncpy(stations[i].logo_url, logo_url, sizeof(stations[i].logo_url) - 1);

            ESP_LOGI(TAG, "Updated station ID: %d", id);
            return radio_stations_save();
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t radio_stations_set_favorite(uint8_t id, bool favorite)
{
    for (int i = 0; i < station_count; i++) {
        if (stations[i].id == id) {
            stations[i].favorite = favorite;
            ESP_LOGI(TAG, "Station ID %d favorite: %s", id, favorite ? "yes" : "no");
            return radio_stations_save();
        }
    }

    return ESP_ERR_NOT_FOUND;
}

radio_station_t *radio_stations_get(uint8_t id)
{
    for (int i = 0; i < station_count; i++) {
        if (stations[i].id == id) {
            return &stations[i];
        }
    }
    return NULL;
}

radio_station_t *radio_stations_get_all(uint8_t *count)
{
    *count = station_count;
    return stations;
}

radio_station_t *radio_stations_get_favorites(uint8_t *count)
{
    static radio_station_t favorites[MAX_RADIO_STATIONS];
    uint8_t fav_count = 0;

    for (int i = 0; i < station_count; i++) {
        if (stations[i].favorite) {
            memcpy(&favorites[fav_count], &stations[i], sizeof(radio_station_t));
            fav_count++;
        }
    }

    *count = fav_count;
    return favorites;
}

esp_err_t radio_stations_load_defaults(void)
{
    ESP_LOGI(TAG, "Loading default stations...");

    station_count = sizeof(default_stations) / sizeof(default_stations[0]);
    memcpy(stations, default_stations, sizeof(default_stations));

    ESP_LOGI(TAG, "Loaded %d default stations", station_count);
    return radio_stations_save();
}

esp_err_t radio_stations_save(void)
{
    ESP_LOGI(TAG, "Saving stations to NVS...");

    // Serializacja do JSON
    cJSON *root = cJSON_CreateArray();

    for (int i = 0; i < station_count; i++) {
        cJSON *station = cJSON_CreateObject();
        cJSON_AddNumberToObject(station, "id", stations[i].id);
        cJSON_AddStringToObject(station, "name", stations[i].name);
        cJSON_AddStringToObject(station, "url", stations[i].url);
        cJSON_AddStringToObject(station, "logo", stations[i].logo_url);
        cJSON_AddBoolToObject(station, "fav", stations[i].favorite);
        cJSON_AddItemToArray(root, station);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nvs_set_str(radio_nvs_handle, "stations", json_str);
    free(json_str);

    if (ret == ESP_OK) {
        ret = nvs_commit(radio_nvs_handle);
    }

    ESP_LOGI(TAG, "Stations saved (%d stations)", station_count);
    return ret;
}

esp_err_t radio_stations_load(void)
{
    ESP_LOGI(TAG, "Loading stations from NVS...");

    // Sprawdź rozmiar
    size_t required_size = 0;
    esp_err_t ret = nvs_get_str(radio_nvs_handle, "stations", NULL, &required_size);
    if (ret != ESP_OK || required_size == 0) {
        ESP_LOGW(TAG, "No stations in NVS");
        return ESP_ERR_NOT_FOUND;
    }

    char *json_str = malloc(required_size);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = nvs_get_str(radio_nvs_handle, "stations", json_str, &required_size);
    if (ret != ESP_OK) {
        free(json_str);
        return ret;
    }

    // Parsowanie JSON
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse stations JSON");
        return ESP_FAIL;
    }

    station_count = 0;
    cJSON *station_json;
    cJSON_ArrayForEach(station_json, root) {
        if (station_count >= MAX_RADIO_STATIONS) break;

        radio_station_t *station = &stations[station_count];

        cJSON *id = cJSON_GetObjectItem(station_json, "id");
        cJSON *name = cJSON_GetObjectItem(station_json, "name");
        cJSON *url = cJSON_GetObjectItem(station_json, "url");
        cJSON *logo = cJSON_GetObjectItem(station_json, "logo");
        cJSON *fav = cJSON_GetObjectItem(station_json, "fav");

        if (id && name && url) {
            station->id = id->valueint;
            strncpy(station->name, name->valuestring, sizeof(station->name) - 1);
            strncpy(station->url, url->valuestring, sizeof(station->url) - 1);
            if (logo && logo->valuestring) {
                strncpy(station->logo_url, logo->valuestring, sizeof(station->logo_url) - 1);
            }
            station->favorite = fav ? cJSON_IsTrue(fav) : false;
            station_count++;
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Loaded %d stations from NVS", station_count);
    return ESP_OK;
}
