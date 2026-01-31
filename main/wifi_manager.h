#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi.h"

// Stan WiFi
typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_AP_MODE,
    WIFI_STATE_ERROR,
} wifi_state_t;

// Callback dla zmiany stanu WiFi
typedef void (*wifi_state_callback_t)(wifi_state_t state, const char *ip);

// Inicjalizacja WiFi
esp_err_t wifi_manager_init(void);

// Połączenie ze stacją
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_disconnect(void);

// Tryb AP (konfiguracyjny) - IP: 192.168.1.1
esp_err_t wifi_manager_start_ap(const char *ssid, const char *password);
esp_err_t wifi_manager_stop_ap(void);

// Stan
wifi_state_t wifi_manager_get_state(void);
const char *wifi_manager_get_ip(void);
int8_t wifi_manager_get_rssi(void);

// Callback
void wifi_manager_register_callback(wifi_state_callback_t callback);

// Skanowanie sieci
esp_err_t wifi_manager_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count);

// Zapis/odczyt ustawień WiFi z NVS
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);
esp_err_t wifi_manager_clear_credentials(void);
bool wifi_manager_has_saved_credentials(void);

// Auto-połączenie z zapisanych danych
esp_err_t wifi_manager_auto_connect(void);

#endif // WIFI_MANAGER_H
