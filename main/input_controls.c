/*
 * Input Controls Module
 * Handles touch buttons, physical buttons, and headphone detection
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/touch_pad.h"
#include "driver/gpio.h"

#include "input_controls.h"
#include "config.h"
#include "audio_player.h"
#include "alarm_manager.h"

static const char *TAG = "INPUT_CTRL";

// ============================================
// Configuration
// ============================================
#define TOUCH_THRESHOLD_PERCENT  80     // Touch detection threshold
#define LONG_PRESS_TIME_MS       1000   // Long press detection
#define DOUBLE_TAP_TIME_MS       300    // Double tap window
#define DEBOUNCE_TIME_MS         50     // Debounce time

// Touch pad mappings for ESP32-LyraT V4.3
#define TOUCH_PAD_PLAY      TOUCH_PAD_NUM9   // GPIO32
#define TOUCH_PAD_SET       TOUCH_PAD_NUM8   // GPIO33
#define TOUCH_PAD_VOL_UP    TOUCH_PAD_NUM7   // GPIO27
#define TOUCH_PAD_VOL_DOWN  TOUCH_PAD_NUM4   // GPIO13

// ============================================
// State variables
// ============================================
static audio_source_mode_t current_source = SOURCE_RADIO;
static headphone_state_t headphone_state = HEADPHONE_UNPLUGGED;
static bool button_pressed[TOUCH_BTN_MAX] = {false};
static uint32_t last_tap_time[TOUCH_BTN_MAX] = {0};

// Callbacks
static touch_button_callback_t touch_callback = NULL;
static mode_button_callback_t mode_callback = NULL;
static headphone_callback_t headphone_cb = NULL;

// Task handle
static TaskHandle_t input_task_handle = NULL;
static bool task_running = false;

// Touch thresholds (calibrated on init)
static uint16_t touch_threshold[TOUCH_BTN_MAX] = {0};

// Source names
static const char *source_names[] = {
    "Radio",
    "Bluetooth",
    "SD Card",
    "AUX"
};

// ============================================
// Touch pad mapping
// ============================================
static touch_pad_t get_touch_pad(touch_button_t btn) {
    switch (btn) {
        case TOUCH_BTN_PLAY:     return TOUCH_PAD_PLAY;
        case TOUCH_BTN_SET:      return TOUCH_PAD_SET;
        case TOUCH_BTN_VOL_UP:   return TOUCH_PAD_VOL_UP;
        case TOUCH_BTN_VOL_DOWN: return TOUCH_PAD_VOL_DOWN;
        default: return TOUCH_PAD_MAX;
    }
}

// ============================================
// Touch calibration
// ============================================
static void calibrate_touch_pads(void) {
    for (int i = 0; i < TOUCH_BTN_MAX; i++) {
        touch_pad_t pad = get_touch_pad(i);
        uint16_t val;
        touch_pad_read(pad, &val);
        touch_threshold[i] = val * TOUCH_THRESHOLD_PERCENT / 100;
        ESP_LOGI(TAG, "Touch pad %d calibrated: base=%d, threshold=%d",
                 i, val, touch_threshold[i]);
    }
}

// ============================================
// Check if touch pad is pressed
// ============================================
static bool is_touch_pressed(touch_button_t btn) {
    touch_pad_t pad = get_touch_pad(btn);
    uint16_t val;
    touch_pad_read(pad, &val);
    return val < touch_threshold[btn];
}

// ============================================
// Handle touch button event
// ============================================
static void handle_touch_event(touch_button_t btn, touch_event_t event) {
    ESP_LOGI(TAG, "Touch event: button=%d, event=%d", btn, event);

    // If alarm is active, any button press stops the alarm
    if (alarm_manager_is_alarm_active()) {
        if (event == TOUCH_EVENT_TAP || event == TOUCH_EVENT_LONG_PRESS) {
            if (btn == TOUCH_BTN_SET && event == TOUCH_EVENT_LONG_PRESS) {
                // Long press SET = snooze
                ESP_LOGI(TAG, "Snoozing alarm");
                alarm_manager_snooze();
            } else {
                // Any other button press = stop alarm
                ESP_LOGI(TAG, "Stopping alarm via touch button");
                alarm_manager_stop_alarm();
            }
            return;  // Don't process normal button actions
        }
    }

    // Default handling
    switch (btn) {
        case TOUCH_BTN_PLAY:
            if (event == TOUCH_EVENT_TAP) {
                player_status_t *status = audio_player_get_status();
                if (status->state == PLAYER_STATE_PLAYING) {
                    audio_player_pause();
                } else {
                    audio_player_resume();
                }
            }
            break;

        case TOUCH_BTN_SET:
            if (event == TOUCH_EVENT_TAP) {
                // Next station / track
            } else if (event == TOUCH_EVENT_LONG_PRESS) {
                // Previous station / track
            }
            break;

        case TOUCH_BTN_VOL_UP:
            if (event == TOUCH_EVENT_TAP || event == TOUCH_EVENT_LONG_PRESS) {
                int vol = audio_player_get_volume();
                audio_player_set_volume(vol + 5);
            }
            break;

        case TOUCH_BTN_VOL_DOWN:
            if (event == TOUCH_EVENT_TAP || event == TOUCH_EVENT_LONG_PRESS) {
                int vol = audio_player_get_volume();
                audio_player_set_volume(vol - 5);
            }
            break;

        default:
            break;
    }

    // Call user callback
    if (touch_callback) {
        touch_callback(btn, event);
    }
}

// ============================================
// Handle Mode button press
// ============================================
static void handle_mode_button(void) {
    // If alarm is active, Mode button stops it
    if (alarm_manager_is_alarm_active()) {
        ESP_LOGI(TAG, "Stopping alarm via Mode button");
        alarm_manager_stop_alarm();
        return;
    }

    audio_source_mode_t new_source = (current_source + 1) % SOURCE_MAX;
    ESP_LOGI(TAG, "Mode button: switching from %s to %s",
             source_names[current_source], source_names[new_source]);

    current_source = new_source;

    if (mode_callback) {
        mode_callback(new_source);
    }
}

// ============================================
// Headphone detection handler
// ============================================
static void check_headphone_state(void) {
    // GPIO19 is typically used for headphone detection on LyraT
    // LOW = headphones plugged in (active low)
    int level = gpio_get_level(HEADPHONE_DETECT_GPIO);
    headphone_state_t new_state = (level == 0) ? HEADPHONE_PLUGGED : HEADPHONE_UNPLUGGED;

    if (new_state != headphone_state) {
        headphone_state = new_state;
        ESP_LOGI(TAG, "Headphone %s",
                 new_state == HEADPHONE_PLUGGED ? "connected" : "disconnected");

        if (headphone_cb) {
            headphone_cb(new_state);
        }
    }
}

// ============================================
// Input processing task
// ============================================
static void input_controls_task(void *pvParameters) {
    uint32_t press_start_time[TOUCH_BTN_MAX] = {0};
    bool was_pressed[TOUCH_BTN_MAX] = {false};
    uint32_t last_mode_press = 0;
    bool mode_was_pressed = false;

    while (task_running) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Process touch buttons
        for (int i = 0; i < TOUCH_BTN_MAX; i++) {
            bool pressed = is_touch_pressed(i);
            button_pressed[i] = pressed;

            if (pressed && !was_pressed[i]) {
                // Button just pressed
                press_start_time[i] = now;
                was_pressed[i] = true;
            }
            else if (!pressed && was_pressed[i]) {
                // Button just released
                uint32_t press_duration = now - press_start_time[i];
                was_pressed[i] = false;

                if (press_duration >= LONG_PRESS_TIME_MS) {
                    handle_touch_event(i, TOUCH_EVENT_LONG_PRESS);
                } else {
                    // Check for double tap
                    if (now - last_tap_time[i] < DOUBLE_TAP_TIME_MS) {
                        handle_touch_event(i, TOUCH_EVENT_DOUBLE_TAP);
                        last_tap_time[i] = 0;
                    } else {
                        last_tap_time[i] = now;
                        // Delay to check for double tap
                        vTaskDelay(pdMS_TO_TICKS(DOUBLE_TAP_TIME_MS));
                        if (last_tap_time[i] != 0) {
                            handle_touch_event(i, TOUCH_EVENT_TAP);
                            last_tap_time[i] = 0;
                        }
                    }
                }
            }
            else if (pressed && was_pressed[i]) {
                // Button held - for volume repeat
                uint32_t press_duration = now - press_start_time[i];
                if (press_duration >= LONG_PRESS_TIME_MS) {
                    if (i == TOUCH_BTN_VOL_UP || i == TOUCH_BTN_VOL_DOWN) {
                        // Repeat volume change while held
                        handle_touch_event(i, TOUCH_EVENT_LONG_PRESS);
                        vTaskDelay(pdMS_TO_TICKS(100)); // Repeat rate
                        continue;
                    }
                }
            }
        }

        // Process Mode button (GPIO39)
        bool mode_pressed = (gpio_get_level(BUTTON_MODE_GPIO) == 0);
        if (mode_pressed && !mode_was_pressed) {
            if (now - last_mode_press > DEBOUNCE_TIME_MS) {
                handle_mode_button();
                last_mode_press = now;
            }
        }
        mode_was_pressed = mode_pressed;

        // Check headphone state
        check_headphone_state();

        vTaskDelay(pdMS_TO_TICKS(20));  // 50Hz polling
    }

    vTaskDelete(NULL);
}

// ============================================
// Public API
// ============================================

esp_err_t input_controls_init(void) {
    ESP_LOGI(TAG, "Initializing input controls...");

    // Initialize touch pad driver
    ESP_ERROR_CHECK(touch_pad_init());
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);

    // Configure touch pads
    touch_pad_config(TOUCH_PAD_PLAY, 0);
    touch_pad_config(TOUCH_PAD_SET, 0);
    touch_pad_config(TOUCH_PAD_VOL_UP, 0);
    touch_pad_config(TOUCH_PAD_VOL_DOWN, 0);

    // Start touch pad filter
    touch_pad_filter_start(10);

    // Wait for calibration data
    vTaskDelay(pdMS_TO_TICKS(100));
    calibrate_touch_pads();

    // Configure Mode button GPIO (already configured in main for factory reset)
    // GPIO39 is input-only, no pull-up needed

    // Configure headphone detection GPIO
    gpio_config_t hp_conf = {
        .pin_bit_mask = (1ULL << HEADPHONE_DETECT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&hp_conf);

    // Check initial headphone state
    check_headphone_state();

    // Start input processing task
    task_running = true;
    xTaskCreate(input_controls_task, "input_ctrl", 4096, NULL, 5, &input_task_handle);

    ESP_LOGI(TAG, "Input controls initialized");
    return ESP_OK;
}

esp_err_t input_controls_deinit(void) {
    task_running = false;
    if (input_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
        input_task_handle = NULL;
    }
    touch_pad_deinit();
    return ESP_OK;
}

void input_controls_register_touch_callback(touch_button_callback_t callback) {
    touch_callback = callback;
}

void input_controls_register_mode_callback(mode_button_callback_t callback) {
    mode_callback = callback;
}

void input_controls_register_headphone_callback(headphone_callback_t callback) {
    headphone_cb = callback;
}

audio_source_mode_t input_controls_get_current_source(void) {
    return current_source;
}

esp_err_t input_controls_set_source(audio_source_mode_t source) {
    if (source >= SOURCE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source != current_source) {
        ESP_LOGI(TAG, "Source changed: %s -> %s",
                 source_names[current_source], source_names[source]);
        current_source = source;

        if (mode_callback) {
            mode_callback(source);
        }
    }

    return ESP_OK;
}

const char *input_controls_get_source_name(audio_source_mode_t source) {
    if (source >= SOURCE_MAX) {
        return "Unknown";
    }
    return source_names[source];
}

headphone_state_t input_controls_get_headphone_state(void) {
    return headphone_state;
}

bool input_controls_is_headphone_connected(void) {
    return headphone_state == HEADPHONE_PLUGGED;
}

bool input_controls_is_button_pressed(touch_button_t button) {
    if (button >= TOUCH_BTN_MAX) {
        return false;
    }
    return button_pressed[button];
}
