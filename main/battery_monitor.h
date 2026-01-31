/*
 * Battery Monitor Module
 * Monitors battery voltage, percentage, and charging status
 */

#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include "esp_err.h"
#include <stdbool.h>

// ============================================
// Charging states
// ============================================
typedef enum {
    BATTERY_DISCHARGING = 0,
    BATTERY_CHARGING,
    BATTERY_FULL,
    BATTERY_NOT_PRESENT,
    BATTERY_ERROR,
} battery_charge_state_t;

// ============================================
// Battery status structure
// ============================================
typedef struct {
    float voltage;              // Voltage in V (e.g., 3.7)
    uint8_t percentage;         // 0-100%
    battery_charge_state_t charge_state;
    bool low_battery;           // True if < 20%
    bool critical_battery;      // True if < 10%
    bool usb_powered;           // True if USB is connected
} battery_status_t;

// ============================================
// Callbacks
// ============================================
typedef void (*battery_status_callback_t)(battery_status_t *status);
typedef void (*battery_low_callback_t)(uint8_t percentage);

// ============================================
// Initialization
// ============================================
esp_err_t battery_monitor_init(void);
esp_err_t battery_monitor_deinit(void);

// ============================================
// Status queries
// ============================================
battery_status_t *battery_monitor_get_status(void);
float battery_monitor_get_voltage(void);
uint8_t battery_monitor_get_percentage(void);
battery_charge_state_t battery_monitor_get_charge_state(void);
bool battery_monitor_is_charging(void);
bool battery_monitor_is_usb_powered(void);
bool battery_monitor_is_low(void);

// ============================================
// Configuration
// ============================================
esp_err_t battery_monitor_set_low_threshold(uint8_t percentage);
esp_err_t battery_monitor_set_critical_threshold(uint8_t percentage);

// ============================================
// Callbacks
// ============================================
void battery_monitor_register_status_callback(battery_status_callback_t callback);
void battery_monitor_register_low_callback(battery_low_callback_t callback);

// ============================================
// Manual refresh
// ============================================
esp_err_t battery_monitor_refresh(void);

#endif // BATTERY_MONITOR_H
