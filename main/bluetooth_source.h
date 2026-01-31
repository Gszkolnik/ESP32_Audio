/*
 * Bluetooth A2DP Source Module
 * Allows streaming audio to external Bluetooth speakers/headphones
 */

#ifndef BLUETOOTH_SOURCE_H
#define BLUETOOTH_SOURCE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of discovered devices
#define BT_SOURCE_MAX_DEVICES 16
#define BT_SOURCE_DEVICE_NAME_LEN 64

// Bluetooth Source states
typedef enum {
    BT_SOURCE_STATE_IDLE,
    BT_SOURCE_STATE_DISCOVERING,
    BT_SOURCE_STATE_CONNECTING,
    BT_SOURCE_STATE_CONNECTED,
    BT_SOURCE_STATE_STREAMING,
    BT_SOURCE_STATE_DISCONNECTING,
    BT_SOURCE_STATE_ERROR
} bt_source_state_t;

// Discovered Bluetooth device info
typedef struct {
    uint8_t bda[6];                           // Bluetooth Device Address
    char name[BT_SOURCE_DEVICE_NAME_LEN];     // Device name
    int32_t rssi;                             // Signal strength
    bool is_audio_sink;                       // Supports A2DP Sink
} bt_source_device_t;

// Bluetooth Source status
typedef struct {
    bt_source_state_t state;
    bt_source_device_t connected_device;      // Currently connected device
    bt_source_device_t devices[BT_SOURCE_MAX_DEVICES];  // Discovered devices
    uint8_t device_count;                     // Number of discovered devices
    uint8_t volume;                           // Current volume (0-127)
    char error_msg[64];                       // Error message if any
} bt_source_status_t;

// Callback for state changes
typedef void (*bt_source_state_callback_t)(bt_source_state_t state, const char *device_name);

// Callback for device discovery
typedef void (*bt_source_discovery_callback_t)(bt_source_device_t *device);

/**
 * @brief Initialize Bluetooth A2DP Source
 * @note This will deinitialize BT Sink if it was active
 * @return ESP_OK on success
 */
esp_err_t bt_source_init(void);

/**
 * @brief Deinitialize Bluetooth Source
 * @return ESP_OK on success
 */
esp_err_t bt_source_deinit(void);

/**
 * @brief Start device discovery (scanning)
 * @param duration_sec Scan duration in seconds (default 10)
 * @return ESP_OK on success
 */
esp_err_t bt_source_start_discovery(uint8_t duration_sec);

/**
 * @brief Stop device discovery
 * @return ESP_OK on success
 */
esp_err_t bt_source_stop_discovery(void);

/**
 * @brief Connect to a Bluetooth device by address
 * @param bda Bluetooth Device Address (6 bytes)
 * @return ESP_OK on success
 */
esp_err_t bt_source_connect(const uint8_t *bda);

/**
 * @brief Connect to a Bluetooth device by index from discovered list
 * @param device_index Index in the discovered devices list
 * @return ESP_OK on success
 */
esp_err_t bt_source_connect_by_index(uint8_t device_index);

/**
 * @brief Disconnect from current device
 * @return ESP_OK on success
 */
esp_err_t bt_source_disconnect(void);

/**
 * @brief Get current Bluetooth Source status
 * @return Pointer to status structure
 */
const bt_source_status_t *bt_source_get_status(void);

/**
 * @brief Get current state
 * @return Current state
 */
bt_source_state_t bt_source_get_state(void);

/**
 * @brief Check if Bluetooth Source is initialized
 * @return true if initialized
 */
bool bt_source_is_initialized(void);

/**
 * @brief Check if connected to a device
 * @return true if connected
 */
bool bt_source_is_connected(void);

/**
 * @brief Check if currently streaming
 * @return true if streaming audio
 */
bool bt_source_is_streaming(void);

/**
 * @brief Set volume for Bluetooth output
 * @param volume Volume level (0-127)
 * @return ESP_OK on success
 */
esp_err_t bt_source_set_volume(uint8_t volume);

/**
 * @brief Get current volume
 * @return Current volume (0-127)
 */
uint8_t bt_source_get_volume(void);

/**
 * @brief Register callback for state changes
 * @param callback Callback function
 */
void bt_source_register_state_callback(bt_source_state_callback_t callback);

/**
 * @brief Register callback for device discovery
 * @param callback Callback function
 */
void bt_source_register_discovery_callback(bt_source_discovery_callback_t callback);

/**
 * @brief Get discovered devices list
 * @param devices Output array for devices
 * @param max_devices Maximum devices to return
 * @return Number of devices copied
 */
uint8_t bt_source_get_discovered_devices(bt_source_device_t *devices, uint8_t max_devices);

/**
 * @brief Clear discovered devices list
 */
void bt_source_clear_devices(void);

/**
 * @brief Write audio data to Bluetooth stream
 * @param data PCM audio data (16-bit stereo, 44100Hz)
 * @param len Data length in bytes
 * @return Number of bytes written
 */
int bt_source_write_audio(const uint8_t *data, size_t len);

/**
 * @brief Get state name as string
 * @param state State enum value
 * @return State name string
 */
const char *bt_source_state_to_str(bt_source_state_t state);

/**
 * @brief Format BDA as string "XX:XX:XX:XX:XX:XX"
 * @param bda Bluetooth Device Address
 * @param str Output string buffer (at least 18 bytes)
 */
void bt_source_bda_to_str(const uint8_t *bda, char *str);

/**
 * @brief Parse BDA from string "XX:XX:XX:XX:XX:XX"
 * @param str Input string
 * @param bda Output BDA array (6 bytes)
 * @return ESP_OK on success
 */
esp_err_t bt_source_str_to_bda(const char *str, uint8_t *bda);

#ifdef __cplusplus
}
#endif

#endif // BLUETOOTH_SOURCE_H
