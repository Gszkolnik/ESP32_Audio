/*
 * Input Controls Module
 * Handles touch buttons, physical buttons, and headphone detection
 */

#ifndef INPUT_CONTROLS_H
#define INPUT_CONTROLS_H

#include "esp_err.h"
#include <stdbool.h>

// ============================================
// Touch button IDs
// ============================================
typedef enum {
    TOUCH_BTN_PLAY = 0,
    TOUCH_BTN_SET,
    TOUCH_BTN_VOL_UP,
    TOUCH_BTN_VOL_DOWN,
    TOUCH_BTN_MAX
} touch_button_t;

// ============================================
// Touch button events
// ============================================
typedef enum {
    TOUCH_EVENT_TAP = 0,        // Single tap
    TOUCH_EVENT_LONG_PRESS,     // Long press (>1s)
    TOUCH_EVENT_DOUBLE_TAP,     // Double tap
} touch_event_t;

// ============================================
// Audio sources for Mode button
// ============================================
typedef enum {
    SOURCE_RADIO = 0,
    SOURCE_BLUETOOTH,
    SOURCE_SD_CARD,
    SOURCE_AUX,
    SOURCE_MAX
} audio_source_mode_t;

// ============================================
// Headphone state
// ============================================
typedef enum {
    HEADPHONE_UNPLUGGED = 0,
    HEADPHONE_PLUGGED,
} headphone_state_t;

// ============================================
// Callbacks
// ============================================
typedef void (*touch_button_callback_t)(touch_button_t button, touch_event_t event);
typedef void (*mode_button_callback_t)(audio_source_mode_t new_source);
typedef void (*headphone_callback_t)(headphone_state_t state);

// ============================================
// Initialization
// ============================================
esp_err_t input_controls_init(void);
esp_err_t input_controls_deinit(void);

// ============================================
// Callbacks registration
// ============================================
void input_controls_register_touch_callback(touch_button_callback_t callback);
void input_controls_register_mode_callback(mode_button_callback_t callback);
void input_controls_register_headphone_callback(headphone_callback_t callback);

// ============================================
// Source management
// ============================================
audio_source_mode_t input_controls_get_current_source(void);
esp_err_t input_controls_set_source(audio_source_mode_t source);
const char *input_controls_get_source_name(audio_source_mode_t source);

// ============================================
// Headphone state
// ============================================
headphone_state_t input_controls_get_headphone_state(void);
bool input_controls_is_headphone_connected(void);

// ============================================
// Button states (for UI feedback)
// ============================================
bool input_controls_is_button_pressed(touch_button_t button);

#endif // INPUT_CONTROLS_H
