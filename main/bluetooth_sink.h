/*
 * Bluetooth A2DP Sink Module
 * Receives audio from phones/computers via Bluetooth
 */

#ifndef BLUETOOTH_SINK_H
#define BLUETOOTH_SINK_H

#include "esp_err.h"
#include <stdbool.h>

// ============================================
// Bluetooth connection states
// ============================================
typedef enum {
    BT_STATE_OFF = 0,
    BT_STATE_IDLE,
    BT_STATE_DISCOVERABLE,
    BT_STATE_CONNECTING,
    BT_STATE_CONNECTED,
    BT_STATE_STREAMING,
} bt_state_t;

// ============================================
// AVRCP playback status
// ============================================
typedef enum {
    BT_PLAYBACK_STOPPED = 0,
    BT_PLAYBACK_PLAYING,
    BT_PLAYBACK_PAUSED,
    BT_PLAYBACK_FWD_SEEK,
    BT_PLAYBACK_REV_SEEK,
    BT_PLAYBACK_ERROR,
} bt_playback_status_t;

// ============================================
// Track metadata
// ============================================
typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    uint32_t duration_ms;
    uint32_t position_ms;
} bt_track_info_t;

// ============================================
// Connected device info
// ============================================
typedef struct {
    char name[64];
    char address[18];   // "XX:XX:XX:XX:XX:XX"
    int rssi;
} bt_device_info_t;

// ============================================
// Callbacks
// ============================================
typedef void (*bt_state_callback_t)(bt_state_t state);
typedef void (*bt_track_callback_t)(bt_track_info_t *track);
typedef void (*bt_playback_callback_t)(bt_playback_status_t status);

// ============================================
// Initialization
// ============================================
esp_err_t bluetooth_sink_init(const char *device_name);
esp_err_t bluetooth_sink_deinit(void);

// ============================================
// Control
// ============================================
esp_err_t bluetooth_sink_start(void);          // Start and make discoverable
esp_err_t bluetooth_sink_stop(void);           // Stop Bluetooth
esp_err_t bluetooth_sink_disconnect(void);     // Disconnect current device

// ============================================
// AVRCP commands (remote control)
// ============================================
esp_err_t bluetooth_sink_play(void);
esp_err_t bluetooth_sink_pause(void);
esp_err_t bluetooth_sink_stop_playback(void);
esp_err_t bluetooth_sink_next(void);
esp_err_t bluetooth_sink_prev(void);
esp_err_t bluetooth_sink_fast_forward(void);
esp_err_t bluetooth_sink_rewind(void);

// ============================================
// Volume control
// ============================================
esp_err_t bluetooth_sink_set_volume(uint8_t volume);  // 0-127
uint8_t bluetooth_sink_get_volume(void);

// ============================================
// State queries
// ============================================
bt_state_t bluetooth_sink_get_state(void);
bt_playback_status_t bluetooth_sink_get_playback_status(void);
const bt_track_info_t *bluetooth_sink_get_track_info(void);
const bt_device_info_t *bluetooth_sink_get_connected_device(void);
bool bluetooth_sink_is_connected(void);
bool bluetooth_sink_is_streaming(void);

// ============================================
// Callbacks
// ============================================
void bluetooth_sink_register_state_callback(bt_state_callback_t callback);
void bluetooth_sink_register_track_callback(bt_track_callback_t callback);
void bluetooth_sink_register_playback_callback(bt_playback_callback_t callback);

#endif // BLUETOOTH_SINK_H
