/*
 * Battery Monitor Module
 * Monitors battery voltage, percentage, and charging status
 *
 * Note: ESP32-LyraT V4.3 does NOT have built-in battery voltage ADC.
 * This module requires hardware modification:
 * - Voltage divider (100k/100k) from BAT+ to GPIO34 (ADC1_CH6)
 * - Optional: CHRG/STDBY pins from AP5056 to GPIO for charge status
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "driver/gpio.h"

#include "battery_monitor.h"
#include "config.h"

static const char *TAG = "BATTERY";

// ============================================
// Configuration
// ============================================
#define BATTERY_CHECK_INTERVAL_MS   10000   // Check every 10 seconds
#define ADC_SAMPLES                 16      // Number of samples for averaging
#define VOLTAGE_DIVIDER_RATIO       2.0     // 100k/100k divider
#define ADC_VREF                    1100    // ADC reference voltage in mV

// Li-Ion battery voltage thresholds
#define BATTERY_VOLTAGE_MAX         4.2     // 100%
#define BATTERY_VOLTAGE_MIN         3.0     // 0%
#define BATTERY_VOLTAGE_LOW         3.4     // ~20%
#define BATTERY_VOLTAGE_CRITICAL    3.2     // ~10%

// ============================================
// State variables
// ============================================
static battery_status_t battery_status = {0};
static uint8_t low_threshold = 20;
static uint8_t critical_threshold = 10;
static TaskHandle_t monitor_task_handle = NULL;
static bool task_running = false;
static esp_adc_cal_characteristics_t adc_chars;
static bool adc_calibrated = false;

// Callbacks
static battery_status_callback_t status_callback = NULL;
static battery_low_callback_t low_callback = NULL;

// Previous state for detecting changes
static bool prev_low_state = false;
static bool prev_critical_state = false;
static battery_charge_state_t prev_charge_state = BATTERY_NOT_PRESENT;

// ============================================
// Voltage to percentage lookup table
// Based on typical Li-Ion discharge curve
// ============================================
static const struct {
    float voltage;
    uint8_t percentage;
} voltage_table[] = {
    {4.20, 100},
    {4.15, 95},
    {4.10, 90},
    {4.05, 85},
    {4.00, 80},
    {3.95, 75},
    {3.90, 70},
    {3.85, 65},
    {3.80, 60},
    {3.75, 55},
    {3.70, 50},
    {3.65, 45},
    {3.60, 40},
    {3.55, 35},
    {3.50, 30},
    {3.45, 25},
    {3.40, 20},
    {3.35, 15},
    {3.30, 10},
    {3.20, 5},
    {3.00, 0},
};

#define VOLTAGE_TABLE_SIZE (sizeof(voltage_table) / sizeof(voltage_table[0]))

// ============================================
// Helper functions
// ============================================

static uint8_t voltage_to_percentage(float voltage) {
    if (voltage >= voltage_table[0].voltage) {
        return 100;
    }
    if (voltage <= voltage_table[VOLTAGE_TABLE_SIZE - 1].voltage) {
        return 0;
    }

    // Linear interpolation between table entries
    for (int i = 0; i < VOLTAGE_TABLE_SIZE - 1; i++) {
        if (voltage >= voltage_table[i + 1].voltage) {
            float v_high = voltage_table[i].voltage;
            float v_low = voltage_table[i + 1].voltage;
            uint8_t p_high = voltage_table[i].percentage;
            uint8_t p_low = voltage_table[i + 1].percentage;

            float ratio = (voltage - v_low) / (v_high - v_low);
            return p_low + (uint8_t)(ratio * (p_high - p_low));
        }
    }

    return 0;
}

static float read_battery_voltage(void) {
    #ifdef BATTERY_ADC_CHANNEL
    uint32_t adc_reading = 0;

    // Multi-sample for noise reduction
    for (int i = 0; i < ADC_SAMPLES; i++) {
        adc_reading += adc1_get_raw(BATTERY_ADC_CHANNEL);
    }
    adc_reading /= ADC_SAMPLES;

    // Convert to voltage
    uint32_t voltage_mv;
    if (adc_calibrated) {
        voltage_mv = esp_adc_cal_raw_to_voltage(adc_reading, &adc_chars);
    } else {
        // Fallback calculation
        voltage_mv = (adc_reading * 3300) / 4095;
    }

    // Account for voltage divider
    float voltage = (voltage_mv / 1000.0) * VOLTAGE_DIVIDER_RATIO;

    return voltage;
    #else
    // No ADC configured - return simulated value or USB voltage
    return battery_status.usb_powered ? 4.2 : 3.7;
    #endif
}

static battery_charge_state_t read_charge_state(void) {
    #if defined(BATTERY_CHRG_GPIO) && defined(BATTERY_STDBY_GPIO)
    bool chrg = gpio_get_level(BATTERY_CHRG_GPIO);   // LOW = charging
    bool stdby = gpio_get_level(BATTERY_STDBY_GPIO); // LOW = full

    if (!chrg && stdby) {
        return BATTERY_CHARGING;
    } else if (chrg && !stdby) {
        return BATTERY_FULL;
    } else if (chrg && stdby) {
        return BATTERY_DISCHARGING;
    } else {
        return BATTERY_ERROR;  // Both LOW = error
    }
    #else
    // No charge status pins - estimate from voltage
    if (battery_status.usb_powered) {
        if (battery_status.percentage >= 100) {
            return BATTERY_FULL;
        }
        return BATTERY_CHARGING;
    }
    return BATTERY_DISCHARGING;
    #endif
}

static bool check_usb_power(void) {
    #ifdef USB_DETECT_GPIO
    return gpio_get_level(USB_DETECT_GPIO) == 1;
    #else
    // Assume USB powered if voltage is high and not discharging fast
    return battery_status.voltage >= 4.1;
    #endif
}

static void update_battery_status(void) {
    // Read voltage
    battery_status.voltage = read_battery_voltage();

    // Calculate percentage
    battery_status.percentage = voltage_to_percentage(battery_status.voltage);

    // Check USB power
    battery_status.usb_powered = check_usb_power();

    // Read charge state
    battery_status.charge_state = read_charge_state();

    // Update flags
    battery_status.low_battery = (battery_status.percentage <= low_threshold);
    battery_status.critical_battery = (battery_status.percentage <= critical_threshold);

    // Notify callbacks on state changes
    if (battery_status.low_battery && !prev_low_state) {
        ESP_LOGW(TAG, "Low battery: %d%%", battery_status.percentage);
        if (low_callback) {
            low_callback(battery_status.percentage);
        }
    }

    if (battery_status.critical_battery && !prev_critical_state) {
        ESP_LOGE(TAG, "Critical battery: %d%%", battery_status.percentage);
        if (low_callback) {
            low_callback(battery_status.percentage);
        }
    }

    if (battery_status.charge_state != prev_charge_state) {
        const char *state_str[] = {"Discharging", "Charging", "Full", "Not Present", "Error"};
        ESP_LOGI(TAG, "Charge state: %s", state_str[battery_status.charge_state]);
    }

    // Update previous states
    prev_low_state = battery_status.low_battery;
    prev_critical_state = battery_status.critical_battery;
    prev_charge_state = battery_status.charge_state;

    // Notify status callback
    if (status_callback) {
        status_callback(&battery_status);
    }
}

// ============================================
// Monitoring task
// ============================================

static void battery_monitor_task(void *pvParameters) {
    while (task_running) {
        update_battery_status();

        ESP_LOGD(TAG, "Battery: %.2fV, %d%%, %s",
                 battery_status.voltage,
                 battery_status.percentage,
                 battery_status.charge_state == BATTERY_CHARGING ? "charging" : "discharging");

        vTaskDelay(pdMS_TO_TICKS(BATTERY_CHECK_INTERVAL_MS));
    }

    vTaskDelete(NULL);
}

// ============================================
// Public API
// ============================================

esp_err_t battery_monitor_init(void) {
    ESP_LOGI(TAG, "Initializing battery monitor...");

    // Initialize ADC for voltage reading
    #ifdef BATTERY_ADC_CHANNEL
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_11);

    // Characterize ADC for accurate voltage reading
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, ADC_VREF, &adc_chars);

    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        ESP_LOGI(TAG, "ADC characterized using eFuse Vref");
        adc_calibrated = true;
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        ESP_LOGI(TAG, "ADC characterized using Two Point Value");
        adc_calibrated = true;
    } else {
        ESP_LOGW(TAG, "ADC characterized using Default Vref");
        adc_calibrated = false;
    }
    #endif

    // Configure charge status GPIOs if available
    #if defined(BATTERY_CHRG_GPIO) && defined(BATTERY_STDBY_GPIO)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BATTERY_CHRG_GPIO) | (1ULL << BATTERY_STDBY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    #endif

    // Initialize status
    memset(&battery_status, 0, sizeof(battery_status));
    battery_status.charge_state = BATTERY_NOT_PRESENT;

    // Initial reading
    update_battery_status();

    // Start monitoring task
    task_running = true;
    xTaskCreate(battery_monitor_task, "battery_mon", 2048, NULL, 2, &monitor_task_handle);

    ESP_LOGI(TAG, "Battery monitor initialized: %.2fV, %d%%",
             battery_status.voltage, battery_status.percentage);
    return ESP_OK;
}

esp_err_t battery_monitor_deinit(void) {
    task_running = false;
    if (monitor_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(BATTERY_CHECK_INTERVAL_MS + 100));
        monitor_task_handle = NULL;
    }
    return ESP_OK;
}

battery_status_t *battery_monitor_get_status(void) {
    return &battery_status;
}

float battery_monitor_get_voltage(void) {
    return battery_status.voltage;
}

uint8_t battery_monitor_get_percentage(void) {
    return battery_status.percentage;
}

battery_charge_state_t battery_monitor_get_charge_state(void) {
    return battery_status.charge_state;
}

bool battery_monitor_is_charging(void) {
    return battery_status.charge_state == BATTERY_CHARGING;
}

bool battery_monitor_is_usb_powered(void) {
    return battery_status.usb_powered;
}

bool battery_monitor_is_low(void) {
    return battery_status.low_battery;
}

esp_err_t battery_monitor_set_low_threshold(uint8_t percentage) {
    if (percentage > 50) percentage = 50;
    low_threshold = percentage;
    return ESP_OK;
}

esp_err_t battery_monitor_set_critical_threshold(uint8_t percentage) {
    if (percentage > 30) percentage = 30;
    critical_threshold = percentage;
    return ESP_OK;
}

void battery_monitor_register_status_callback(battery_status_callback_t callback) {
    status_callback = callback;
}

void battery_monitor_register_low_callback(battery_low_callback_t callback) {
    low_callback = callback;
}

esp_err_t battery_monitor_refresh(void) {
    update_battery_status();
    return ESP_OK;
}
