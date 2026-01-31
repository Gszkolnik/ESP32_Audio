/*
 * AUX Input Module
 * Handles external audio input via 3.5mm jack
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/adc.h"

#include "aux_input.h"
#include "config.h"
#include "board.h"

static const char *TAG = "AUX_INPUT";

// ============================================
// Configuration
// ============================================
#define AUX_DETECT_CHECK_INTERVAL_MS    1000  // Reduced from 200ms to save CPU
#define AUX_SIGNAL_THRESHOLD            100     // ADC threshold for signal detection
#define AUX_SIGNAL_SAMPLES              10      // Samples for signal level averaging

// ============================================
// State variables
// ============================================
static aux_state_t current_state = AUX_STATE_DISABLED;
static int current_gain = 0;  // dB
static int signal_level = 0;
static bool enabled = false;
static TaskHandle_t aux_task_handle = NULL;
static bool task_running = false;

// Callback
static aux_state_callback_t state_callback = NULL;

// Audio board handle
static audio_board_handle_t board_handle = NULL;

// ============================================
// Helper functions
// ============================================

static void set_state(aux_state_t new_state) {
    if (new_state != current_state) {
        ESP_LOGI(TAG, "State changed: %d -> %d", current_state, new_state);
        current_state = new_state;
        if (state_callback) {
            state_callback(new_state);
        }
    }
}

static bool check_aux_plugged(void) {
    // AUX detection via GPIO (DIP SW 7 must be ON)
    // When cable is plugged, the detect pin goes LOW
    #ifdef AUX_DETECT_GPIO
    return gpio_get_level(AUX_DETECT_GPIO) == 0;
    #else
    // If no detect pin, assume always connected when enabled
    return enabled;
    #endif
}

static int read_signal_level(void) {
    // Read audio level from ADC (simplified - actual implementation
    // would use I2S or codec's built-in level detection)
    uint32_t sum = 0;
    for (int i = 0; i < AUX_SIGNAL_SAMPLES; i++) {
        // This is a placeholder - actual signal level would come from codec
        sum += 0;  // adc1_get_raw() if ADC was connected to audio
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return sum / AUX_SIGNAL_SAMPLES * 100 / 4095;
}

// ============================================
// Monitoring task
// ============================================

static void aux_monitor_task(void *pvParameters) {
    bool was_plugged = false;
    int no_signal_count = 0;

    while (task_running) {
        if (!enabled) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        bool is_plugged = check_aux_plugged();

        if (is_plugged && !was_plugged) {
            // Cable just plugged in
            ESP_LOGI(TAG, "AUX cable connected");
            set_state(AUX_STATE_PLUGGED);
        } else if (!is_plugged && was_plugged) {
            // Cable unplugged
            ESP_LOGI(TAG, "AUX cable disconnected");
            set_state(AUX_STATE_UNPLUGGED);
            signal_level = 0;
        }

        was_plugged = is_plugged;

        // Check signal level if plugged
        if (is_plugged) {
            signal_level = read_signal_level();

            if (signal_level > AUX_SIGNAL_THRESHOLD) {
                if (current_state != AUX_STATE_ACTIVE) {
                    set_state(AUX_STATE_ACTIVE);
                }
                no_signal_count = 0;
            } else {
                no_signal_count++;
                // After 2 seconds of no signal, go back to plugged state
                if (no_signal_count > (2000 / AUX_DETECT_CHECK_INTERVAL_MS)) {
                    if (current_state == AUX_STATE_ACTIVE) {
                        set_state(AUX_STATE_PLUGGED);
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(AUX_DETECT_CHECK_INTERVAL_MS));
    }

    vTaskDelete(NULL);
}

// ============================================
// Public API
// ============================================

esp_err_t aux_input_init(void) {
    ESP_LOGI(TAG, "Initializing AUX input...");

    board_handle = audio_board_get_handle();
    if (board_handle == NULL) {
        ESP_LOGE(TAG, "Audio board not initialized");
        return ESP_FAIL;
    }

    // Configure AUX detect GPIO if available
    #ifdef AUX_DETECT_GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << AUX_DETECT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    #endif

    current_state = AUX_STATE_DISABLED;
    current_gain = 0;
    signal_level = 0;

    // Start monitoring task
    task_running = true;
    xTaskCreate(aux_monitor_task, "aux_monitor", 2048, NULL, 3, &aux_task_handle);

    ESP_LOGI(TAG, "AUX input initialized");
    return ESP_OK;
}

esp_err_t aux_input_deinit(void) {
    task_running = false;
    if (aux_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(300));
        aux_task_handle = NULL;
    }
    current_state = AUX_STATE_DISABLED;
    return ESP_OK;
}

esp_err_t aux_input_enable(void) {
    ESP_LOGI(TAG, "Enabling AUX input");

    if (board_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Configure codec to use LINE_IN (AUX) as input
    // ES8388 codec on LyraT has LINE_IN on IN2
    audio_hal_ctrl_codec(board_handle->audio_hal,
                         AUDIO_HAL_CODEC_MODE_LINE_IN,
                         AUDIO_HAL_CTRL_START);

    enabled = true;

    if (check_aux_plugged()) {
        set_state(AUX_STATE_PLUGGED);
    } else {
        set_state(AUX_STATE_UNPLUGGED);
    }

    return ESP_OK;
}

esp_err_t aux_input_disable(void) {
    ESP_LOGI(TAG, "Disabling AUX input");

    enabled = false;
    set_state(AUX_STATE_DISABLED);

    return ESP_OK;
}

esp_err_t aux_input_set_gain(int gain_db) {
    if (gain_db < -12) gain_db = -12;
    if (gain_db > 12) gain_db = 12;

    current_gain = gain_db;
    ESP_LOGI(TAG, "AUX gain set to: %d dB", gain_db);

    // Set codec input gain
    // This would need to be implemented via ES8388 register writes
    // audio_hal_set_volume() is for output, not input gain

    return ESP_OK;
}

int aux_input_get_gain(void) {
    return current_gain;
}

aux_state_t aux_input_get_state(void) {
    return current_state;
}

bool aux_input_is_connected(void) {
    return current_state >= AUX_STATE_PLUGGED;
}

bool aux_input_is_active(void) {
    return current_state == AUX_STATE_ACTIVE;
}

int aux_input_get_signal_level(void) {
    return signal_level;
}

void aux_input_register_callback(aux_state_callback_t callback) {
    state_callback = callback;
}
