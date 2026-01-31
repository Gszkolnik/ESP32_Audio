/*
 * OTA Update Module Implementation
 * Over-The-Air firmware update via HTTP
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_flash_partitions.h"
#include "esp_http_client.h"
#include "esp_system.h"

#include "ota_update.h"
#include "config.h"

static const char *TAG = "OTA_UPDATE";

// OTA handle
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;

// Progress tracking
static ota_progress_t progress = {0};
static ota_progress_callback_t progress_callback = NULL;

// Version from app description
extern const esp_app_desc_t esp_app_desc;

// ============================================
// Helper functions
// ============================================

static void update_progress(ota_state_t state, const char *error) {
    progress.state = state;
    if (error) {
        strncpy(progress.error_msg, error, sizeof(progress.error_msg) - 1);
    } else {
        progress.error_msg[0] = '\0';
    }

    if (progress.total_size > 0) {
        progress.progress_percent = (progress.received_size * 100) / progress.total_size;
    }

    if (progress_callback) {
        progress_callback(&progress);
    }
}

// ============================================
// Public API
// ============================================

esp_err_t ota_update_init(void) {
    ESP_LOGI(TAG, "Initializing OTA module");

    // Get current version
    strncpy(progress.current_version, esp_app_desc.version, sizeof(progress.current_version) - 1);
    progress.state = OTA_STATE_IDLE;

    // Check if we just booted from an OTA update
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA, marking as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    ESP_LOGI(TAG, "Running partition: %s, version: %s",
             running->label, progress.current_version);

    return ESP_OK;
}

esp_err_t ota_update_begin(uint32_t total_size) {
    ESP_LOGI(TAG, "Beginning OTA update, size: %lu bytes", (unsigned long)total_size);

    if (progress.state == OTA_STATE_DOWNLOADING) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Find next update partition
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found, trying running partition");
        // For single-partition OTA, use running partition (risky!)
        update_partition = esp_ota_get_running_partition();
        if (update_partition == NULL) {
            update_progress(OTA_STATE_ERROR, "No update partition");
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGW(TAG, "Using running partition for OTA (single-partition mode)");
    }

    ESP_LOGI(TAG, "Writing to partition: %s at 0x%lx",
             update_partition->label, (unsigned long)update_partition->address);

    // Check if partition is large enough
    if (total_size > update_partition->size) {
        ESP_LOGE(TAG, "Firmware too large: %lu > %lu",
                 (unsigned long)total_size, (unsigned long)update_partition->size);
        update_progress(OTA_STATE_ERROR, "Firmware too large");
        return ESP_ERR_INVALID_SIZE;
    }

    // Begin OTA
    esp_err_t err = esp_ota_begin(update_partition, total_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        update_progress(OTA_STATE_ERROR, "OTA begin failed");
        return err;
    }

    // Reset progress
    progress.total_size = total_size;
    progress.received_size = 0;
    progress.progress_percent = 0;
    progress.new_version[0] = '\0';

    update_progress(OTA_STATE_DOWNLOADING, NULL);

    return ESP_OK;
}

esp_err_t ota_update_write(const uint8_t *data, size_t len) {
    if (progress.state != OTA_STATE_DOWNLOADING) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ota_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_write(ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        update_progress(OTA_STATE_ERROR, "Write failed");
        return err;
    }

    progress.received_size += len;

    // Yield CPU after flash write to allow audio tasks to run
    vTaskDelay(1);

    // Update progress every 1%
    uint8_t new_percent = (progress.total_size > 0) ?
        (progress.received_size * 100) / progress.total_size : 0;

    if (new_percent != progress.progress_percent) {
        progress.progress_percent = new_percent;
        if (progress_callback) {
            progress_callback(&progress);
        }

        // Log every 10%
        if (new_percent % 10 == 0) {
            ESP_LOGI(TAG, "OTA progress: %d%%", new_percent);
        }
    }

    return ESP_OK;
}

esp_err_t ota_update_end(void) {
    ESP_LOGI(TAG, "Finishing OTA update");

    if (progress.state != OTA_STATE_DOWNLOADING) {
        return ESP_ERR_INVALID_STATE;
    }

    update_progress(OTA_STATE_VERIFYING, NULL);

    // End OTA write
    esp_err_t err = esp_ota_end(ota_handle);
    ota_handle = 0;

    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            update_progress(OTA_STATE_ERROR, "Validation failed");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            update_progress(OTA_STATE_ERROR, "OTA end failed");
        }
        return err;
    }

    // Set boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        update_progress(OTA_STATE_ERROR, "Set boot failed");
        return err;
    }

    // Get new app description
    esp_app_desc_t new_app_info;
    err = esp_ota_get_partition_description(update_partition, &new_app_info);
    if (err == ESP_OK) {
        strncpy(progress.new_version, new_app_info.version, sizeof(progress.new_version) - 1);
        ESP_LOGI(TAG, "New firmware version: %s", progress.new_version);
    }

    update_progress(OTA_STATE_COMPLETED, NULL);

    ESP_LOGI(TAG, "OTA update successful! Rebooting in 2 seconds...");

    // Delay and reboot
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;  // Won't reach here
}

void ota_update_abort(void) {
    ESP_LOGW(TAG, "Aborting OTA update");

    if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
    }

    progress.state = OTA_STATE_IDLE;
    progress.received_size = 0;
    progress.progress_percent = 0;
    progress.error_msg[0] = '\0';

    if (progress_callback) {
        progress_callback(&progress);
    }
}

const ota_progress_t *ota_update_get_progress(void) {
    return &progress;
}

bool ota_update_is_in_progress(void) {
    return progress.state == OTA_STATE_DOWNLOADING ||
           progress.state == OTA_STATE_VERIFYING;
}

void ota_update_set_callback(ota_progress_callback_t callback) {
    progress_callback = callback;
}

const char *ota_update_get_version(void) {
    return progress.current_version;
}

esp_err_t ota_update_mark_valid(void) {
    return esp_ota_mark_app_valid_cancel_rollback();
}

esp_err_t ota_update_rollback(void) {
    ESP_LOGW(TAG, "Rolling back to previous firmware");

    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(err));
    }

    return err;
}

bool ota_update_can_rollback(void) {
    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        // Can rollback if current image is not yet validated
        return (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    }

    // Check if there's a valid previous partition
    const esp_partition_t *prev = esp_ota_get_last_invalid_partition();
    return (prev != NULL);
}

// ============================================
// HTTP OTA (from URL)
// ============================================

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    // Don't write data here - we handle it in the read loop
    // This handler is only for debugging/status if needed
    return ESP_OK;
}

esp_err_t ota_update_from_url(const char *url) {
    ESP_LOGI(TAG, "Starting OTA from URL: %s", url);

    if (progress.state == OTA_STATE_DOWNLOADING) {
        return ESP_ERR_INVALID_STATE;
    }

    // Get content length first
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 30000,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        update_progress(OTA_STATE_ERROR, "HTTP init failed");
        return ESP_FAIL;
    }

    // Open connection
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        update_progress(OTA_STATE_ERROR, "Connection failed");
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Invalid content length");
        esp_http_client_cleanup(client);
        update_progress(OTA_STATE_ERROR, "Invalid response");
        return ESP_FAIL;
    }

    // Begin OTA
    err = ota_update_begin(content_length);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    // Download and write
    char *buffer = malloc(1024);
    if (buffer == NULL) {
        esp_http_client_cleanup(client);
        ota_update_abort();
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, buffer, 1024);
        if (read_len <= 0) {
            ESP_LOGE(TAG, "Read error");
            break;
        }

        err = ota_update_write((uint8_t *)buffer, read_len);
        if (err != ESP_OK) {
            break;
        }

        total_read += read_len;
    }

    free(buffer);
    esp_http_client_cleanup(client);

    if (total_read != content_length || err != ESP_OK) {
        ota_update_abort();
        update_progress(OTA_STATE_ERROR, "Download incomplete");
        return ESP_FAIL;
    }

    return ota_update_end();
}
