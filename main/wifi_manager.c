/*
 * WiFi Manager Module
 * Handles WiFi connection, AP mode, and network scanning
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"

#include "wifi_manager.h"
#include "config.h"

// NVS namespace dla WiFi
#define WIFI_NVS_NAMESPACE "wifi_creds"

// AP mode IP address
#define AP_IP_ADDR      "192.168.1.1"
#define AP_GW_ADDR      "192.168.1.1"
#define AP_NETMASK      "255.255.255.0"

static const char *TAG = "WIFI_MGR";

// Event group
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Stan
static wifi_state_t current_state = WIFI_STATE_DISCONNECTED;
static char current_ip[16] = "";
static int retry_count = 0;
#define MAX_RETRY 5

// Callback
static wifi_state_callback_t state_callback = NULL;

// Netif handles
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

// ============================================
// Event handlers
// ============================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                current_state = WIFI_STATE_CONNECTING;
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                if (retry_count < MAX_RETRY) {
                    esp_wifi_connect();
                    retry_count++;
                    ESP_LOGI(TAG, "Retrying connection (%d/%d)", retry_count, MAX_RETRY);
                } else {
                    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                    current_state = WIFI_STATE_DISCONNECTED;
                    if (state_callback) {
                        state_callback(current_state, "");
                    }
                }
                break;

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "WiFi AP started");
                current_state = WIFI_STATE_AP_MODE;
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Station connected, MAC: " MACSTR, MAC2STR(event->mac));
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Station disconnected, MAC: " MACSTR, MAC2STR(event->mac));
                break;
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Got IP: %s", current_ip);

            // Disable WiFi power save after connection for stable audio streaming
            esp_wifi_set_ps(WIFI_PS_NONE);
            ESP_LOGI(TAG, "WiFi power save disabled");

            retry_count = 0;
            current_state = WIFI_STATE_CONNECTED;
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

            if (state_callback) {
                state_callback(current_state, current_ip);
            }
        }
    }
}

// ============================================
// Publiczne API
// ============================================

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi manager...");

    wifi_event_group = xEventGroupCreate();

    // Tworzenie netif
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    // Domyślna konfiguracja WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Rejestracja event handlerów
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    // Zapisz credentials do NVS przed połączeniem
    wifi_manager_save_credentials(ssid, password);

    // Wyczyść poprzednie flagi
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_wifi_stop();  // Stop jeśli już działa
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // Disable power save for stable streaming

    // Czekaj na połączenie max 15 sekund
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to SSID: %s", ssid);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid);
        return ESP_FAIL;
    }
}

esp_err_t wifi_manager_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting from WiFi...");
    current_state = WIFI_STATE_DISCONNECTED;
    return esp_wifi_disconnect();
}

esp_err_t wifi_manager_start_ap(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Starting AP mode: %s", ssid);

    // Ustaw statyczny IP dla AP: 192.168.1.1
    esp_netif_dhcps_stop(ap_netif);

    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = ipaddr_addr(AP_IP_ADDR);
    ip_info.gw.addr = ipaddr_addr(AP_GW_ADDR);
    ip_info.netmask.addr = ipaddr_addr(AP_NETMASK);

    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    wifi_config_t wifi_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);

    if (strlen(password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // Disable power save for stable streaming

    snprintf(current_ip, sizeof(current_ip), "%s", AP_IP_ADDR);
    current_state = WIFI_STATE_AP_MODE;

    ESP_LOGI(TAG, "AP started, IP: %s", current_ip);

    if (state_callback) {
        state_callback(current_state, current_ip);
    }

    return ESP_OK;
}

esp_err_t wifi_manager_stop_ap(void)
{
    ESP_LOGI(TAG, "Stopping AP mode...");
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

wifi_state_t wifi_manager_get_state(void)
{
    return current_state;
}

const char *wifi_manager_get_ip(void)
{
    return current_ip;
}

int8_t wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

void wifi_manager_register_callback(wifi_state_callback_t callback)
{
    state_callback = callback;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count)
{
    ESP_LOGI(TAG, "Starting WiFi scan...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    uint16_t number = 20;
    *ap_list = malloc(sizeof(wifi_ap_record_t) * number);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, *ap_list));
    *ap_count = number;

    ESP_LOGI(TAG, "Found %d access points", number);
    return ESP_OK;
}

// ============================================
// NVS - Zapis/odczyt credentials
// ============================================

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for WiFi credentials");
        return ret;
    }

    nvs_set_str(nvs_handle, "ssid", ssid);
    nvs_set_str(nvs_handle, "password", password);
    ret = nvs_commit(nvs_handle);

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi credentials saved for SSID: %s", ssid);
    return ret;
}

esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_str(nvs_handle, "password", password, &pass_len);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi credentials loaded for SSID: %s", ssid);
    return ret;
}

esp_err_t wifi_manager_clear_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_all(nvs_handle);
    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi credentials cleared");
    return ret;
}

bool wifi_manager_has_saved_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return false;
    }

    size_t required_size = 0;
    ret = nvs_get_str(nvs_handle, "ssid", NULL, &required_size);
    nvs_close(nvs_handle);

    return (ret == ESP_OK && required_size > 1);
}

esp_err_t wifi_manager_auto_connect(void)
{
    if (!wifi_manager_has_saved_credentials()) {
        ESP_LOGI(TAG, "No saved credentials, starting AP mode...");
        return wifi_manager_start_ap("ESP32_Audio_Setup", "");
    }

    char ssid[33] = {0};
    char password[65] = {0};

    esp_err_t ret = wifi_manager_load_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load credentials");
        return wifi_manager_start_ap("ESP32_Audio_Setup", "");
    }

    ESP_LOGI(TAG, "Auto-connecting to saved network: %s", ssid);
    return wifi_manager_connect(ssid, password);
}
