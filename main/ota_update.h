/*
 * OTA Update Module
 * Over-The-Air firmware update via HTTP
 */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// OTA state
typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_COMPLETED,
    OTA_STATE_ERROR
} ota_state_t;

// OTA progress info
typedef struct {
    ota_state_t state;
    uint32_t total_size;
    uint32_t received_size;
    uint8_t progress_percent;
    char error_msg[64];
    char current_version[32];
    char new_version[32];
} ota_progress_t;

// Callback for progress updates
typedef void (*ota_progress_callback_t)(const ota_progress_t *progress);

/**
 * @brief Initialize OTA module
 * @return ESP_OK on success
 */
esp_err_t ota_update_init(void);

/**
 * @brief Start OTA update from URL
 * @param url URL of firmware binary
 * @return ESP_OK if download started successfully
 */
esp_err_t ota_update_from_url(const char *url);

/**
 * @brief Begin OTA update from HTTP upload
 * @param total_size Expected total size of firmware
 * @return ESP_OK on success
 */
esp_err_t ota_update_begin(uint32_t total_size);

/**
 * @brief Write chunk of firmware data
 * @param data Pointer to data
 * @param len Length of data
 * @return ESP_OK on success
 */
esp_err_t ota_update_write(const uint8_t *data, size_t len);

/**
 * @brief Finish OTA update and verify
 * @return ESP_OK on success, will reboot on success
 */
esp_err_t ota_update_end(void);

/**
 * @brief Abort ongoing OTA update
 */
void ota_update_abort(void);

/**
 * @brief Get current OTA progress
 * @return Pointer to progress structure
 */
const ota_progress_t *ota_update_get_progress(void);

/**
 * @brief Check if OTA is in progress
 * @return true if update is in progress
 */
bool ota_update_is_in_progress(void);

/**
 * @brief Set progress callback
 * @param callback Callback function
 */
void ota_update_set_callback(ota_progress_callback_t callback);

/**
 * @brief Get current firmware version
 * @return Version string
 */
const char *ota_update_get_version(void);

/**
 * @brief Mark current firmware as valid (after successful boot)
 * @return ESP_OK on success
 */
esp_err_t ota_update_mark_valid(void);

/**
 * @brief Rollback to previous firmware
 * @return ESP_OK on success
 */
esp_err_t ota_update_rollback(void);

/**
 * @brief Check if rollback is possible
 * @return true if rollback is available
 */
bool ota_update_can_rollback(void);

#endif // OTA_UPDATE_H
