/*
 * AUX Input Module
 * Handles external audio input via 3.5mm jack
 */

#ifndef AUX_INPUT_H
#define AUX_INPUT_H

#include "esp_err.h"
#include <stdbool.h>

// ============================================
// AUX input states
// ============================================
typedef enum {
    AUX_STATE_DISABLED = 0,
    AUX_STATE_UNPLUGGED,
    AUX_STATE_PLUGGED,
    AUX_STATE_ACTIVE,       // Audio detected
} aux_state_t;

// ============================================
// Callbacks
// ============================================
typedef void (*aux_state_callback_t)(aux_state_t state);

// ============================================
// Initialization
// ============================================
esp_err_t aux_input_init(void);
esp_err_t aux_input_deinit(void);

// ============================================
// Control
// ============================================
esp_err_t aux_input_enable(void);
esp_err_t aux_input_disable(void);
esp_err_t aux_input_set_gain(int gain_db);  // -12 to +12 dB
int aux_input_get_gain(void);

// ============================================
// Status
// ============================================
aux_state_t aux_input_get_state(void);
bool aux_input_is_connected(void);
bool aux_input_is_active(void);
int aux_input_get_signal_level(void);  // 0-100

// ============================================
// Callbacks
// ============================================
void aux_input_register_callback(aux_state_callback_t callback);

#endif // AUX_INPUT_H
