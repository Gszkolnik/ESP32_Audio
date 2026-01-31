/*
 * Bluetooth A2DP Sink Module
 * Receives audio from phones/computers via Bluetooth
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "bluetooth_sink.h"
#include "config.h"

static const char *TAG = "BT_SINK";

// ============================================
// State variables
// ============================================
static bt_state_t current_state = BT_STATE_OFF;
static bt_playback_status_t playback_status = BT_PLAYBACK_STOPPED;
static bt_track_info_t track_info = {0};
static bt_device_info_t connected_device = {0};
static uint8_t bt_volume = 64;  // 0-127

// Callbacks
static bt_state_callback_t state_callback = NULL;
static bt_track_callback_t track_callback = NULL;
static bt_playback_callback_t playback_callback = NULL;

// Note: audio_board_handle_t removed - not needed for basic Bluetooth sink

// ============================================
// Helper functions
// ============================================

static void set_state(bt_state_t new_state) {
    if (new_state != current_state) {
        ESP_LOGI(TAG, "State changed: %d -> %d", current_state, new_state);
        current_state = new_state;
        if (state_callback) {
            state_callback(new_state);
        }
    }
}

static void set_playback_status(bt_playback_status_t status) {
    if (status != playback_status) {
        playback_status = status;
        if (playback_callback) {
            playback_callback(status);
        }
    }
}

// ============================================
// GAP (Generic Access Profile) callback
// ============================================

static void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Authentication success: %s",
                         param->auth_cmpl.device_name);
                strncpy(connected_device.name,
                        (char *)param->auth_cmpl.device_name,
                        sizeof(connected_device.name) - 1);
            } else {
                ESP_LOGE(TAG, "Authentication failed, status: %d",
                         param->auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_PIN_REQ_EVT:
            ESP_LOGI(TAG, "PIN request, using default: 0000");
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
            break;

#if (CONFIG_BT_SSP_ENABLED == true)
        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(TAG, "SSP confirm request, auto-accepting");
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "SSP passkey notify: %d", param->key_notif.passkey);
            break;
#endif

        default:
            break;
    }
}

// ============================================
// A2DP Sink callback
// ============================================

static void bt_a2dp_sink_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP connected");
                // Store device address
                snprintf(connected_device.address, sizeof(connected_device.address),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1],
                         param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3],
                         param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);
                set_state(BT_STATE_CONNECTED);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "A2DP disconnected");
                memset(&connected_device, 0, sizeof(connected_device));
                memset(&track_info, 0, sizeof(track_info));
                set_playback_status(BT_PLAYBACK_STOPPED);
                set_state(BT_STATE_DISCOVERABLE);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
                set_state(BT_STATE_CONNECTING);
            }
            break;

        case ESP_A2D_AUDIO_STATE_EVT:
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "A2DP audio streaming started");
                set_state(BT_STATE_STREAMING);
            } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED ||
                       param->audio_stat.state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
                ESP_LOGI(TAG, "A2DP audio streaming stopped");
                if (current_state == BT_STATE_STREAMING) {
                    set_state(BT_STATE_CONNECTED);
                }
            }
            break;

        case ESP_A2D_AUDIO_CFG_EVT:
            // Use sbc_info instead of deprecated sbc array
            ESP_LOGI(TAG, "A2DP audio config: sample_rate=%d, channels=%d",
                     param->audio_cfg.mcc.cie.sbc_info.samp_freq,
                     param->audio_cfg.mcc.cie.sbc_info.ch_mode);
            break;

        default:
            break;
    }
}

// ============================================
// A2DP data callback (audio data)
// ============================================

static void bt_a2dp_sink_data_callback(const uint8_t *data, uint32_t len) {
    // Audio data is automatically routed to I2S by ESP-ADF
    // This callback can be used for visualization or processing
}

// ============================================
// AVRCP Controller callback
// ============================================

static void bt_avrc_ct_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "AVRC connection state: %d", param->conn_stat.connected);
            if (param->conn_stat.connected) {
                // Request track info
                esp_avrc_ct_send_metadata_cmd(0, ESP_AVRC_MD_ATTR_TITLE |
                                                 ESP_AVRC_MD_ATTR_ARTIST |
                                                 ESP_AVRC_MD_ATTR_ALBUM |
                                                 ESP_AVRC_MD_ATTR_PLAYING_TIME);
            }
            break;

        case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
            ESP_LOGD(TAG, "AVRC passthrough response: key=%d, state=%d",
                     param->psth_rsp.key_code, param->psth_rsp.key_state);
            break;

        case ESP_AVRC_CT_METADATA_RSP_EVT:
            ESP_LOGI(TAG, "AVRC metadata: attr_id=%d", param->meta_rsp.attr_id);
            switch (param->meta_rsp.attr_id) {
                case ESP_AVRC_MD_ATTR_TITLE:
                    strncpy(track_info.title, (char *)param->meta_rsp.attr_text,
                            sizeof(track_info.title) - 1);
                    break;
                case ESP_AVRC_MD_ATTR_ARTIST:
                    strncpy(track_info.artist, (char *)param->meta_rsp.attr_text,
                            sizeof(track_info.artist) - 1);
                    break;
                case ESP_AVRC_MD_ATTR_ALBUM:
                    strncpy(track_info.album, (char *)param->meta_rsp.attr_text,
                            sizeof(track_info.album) - 1);
                    break;
                case ESP_AVRC_MD_ATTR_PLAYING_TIME:
                    track_info.duration_ms = atoi((char *)param->meta_rsp.attr_text);
                    break;
            }
            if (track_callback) {
                track_callback(&track_info);
            }
            break;

        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
                switch (param->change_ntf.event_parameter.playback) {
                    case ESP_AVRC_PLAYBACK_STOPPED:
                        set_playback_status(BT_PLAYBACK_STOPPED);
                        break;
                    case ESP_AVRC_PLAYBACK_PLAYING:
                        set_playback_status(BT_PLAYBACK_PLAYING);
                        break;
                    case ESP_AVRC_PLAYBACK_PAUSED:
                        set_playback_status(BT_PLAYBACK_PAUSED);
                        break;
                    case ESP_AVRC_PLAYBACK_FWD_SEEK:
                        set_playback_status(BT_PLAYBACK_FWD_SEEK);
                        break;
                    case ESP_AVRC_PLAYBACK_REV_SEEK:
                        set_playback_status(BT_PLAYBACK_REV_SEEK);
                        break;
                    default:
                        set_playback_status(BT_PLAYBACK_ERROR);
                        break;
                }
                // Re-register for notification
                esp_avrc_ct_send_register_notification_cmd(0,
                    ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
            } else if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
                // Request new track info
                esp_avrc_ct_send_metadata_cmd(0, ESP_AVRC_MD_ATTR_TITLE |
                                                 ESP_AVRC_MD_ATTR_ARTIST |
                                                 ESP_AVRC_MD_ATTR_ALBUM);
                esp_avrc_ct_send_register_notification_cmd(0,
                    ESP_AVRC_RN_TRACK_CHANGE, 0);
            }
            break;

        case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
            track_info.duration_ms = param->play_status_rsp.song_length;
            track_info.position_ms = param->play_status_rsp.song_position;
            break;

        default:
            break;
    }
}

// ============================================
// Public API
// ============================================

esp_err_t bluetooth_sink_init(const char *device_name) {
    ESP_LOGI(TAG, "Initializing Bluetooth A2DP Sink...");

    // Release BLE memory (we only use classic BT)
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    // Initialize BT controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    // Initialize Bluedroid
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // Set device name (use esp_bt_gap_set_device_name instead of deprecated esp_bt_dev_set_device_name)
    esp_bt_gap_set_device_name(device_name);

    // Register GAP callback
    esp_bt_gap_register_callback(bt_gap_callback);

    // Initialize A2DP sink
    esp_a2d_register_callback(bt_a2dp_sink_callback);
    esp_a2d_sink_register_data_callback(bt_a2dp_sink_data_callback);
    esp_a2d_sink_init();

    // Initialize AVRCP controller
    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(bt_avrc_ct_callback);

    // Set default SSP IO capability
#if (CONFIG_BT_SSP_ENABLED == true)
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    set_state(BT_STATE_IDLE);
    ESP_LOGI(TAG, "Bluetooth A2DP Sink initialized");
    return ESP_OK;
}

esp_err_t bluetooth_sink_deinit(void) {
    esp_avrc_ct_deinit();
    esp_a2d_sink_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    set_state(BT_STATE_OFF);
    return ESP_OK;
}

esp_err_t bluetooth_sink_start(void) {
    ESP_LOGI(TAG, "Starting Bluetooth (discoverable)...");

    // Set discoverable and connectable
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    set_state(BT_STATE_DISCOVERABLE);
    return ESP_OK;
}

esp_err_t bluetooth_sink_stop(void) {
    ESP_LOGI(TAG, "Stopping Bluetooth...");

    // Disconnect if connected
    if (current_state >= BT_STATE_CONNECTED) {
        bluetooth_sink_disconnect();
    }

    // Set non-discoverable
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    set_state(BT_STATE_IDLE);
    return ESP_OK;
}

esp_err_t bluetooth_sink_disconnect(void) {
    if (current_state < BT_STATE_CONNECTED) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Disconnecting...");
    // A2DP disconnect will be handled automatically
    esp_a2d_sink_disconnect(NULL);  // Disconnect current device
    return ESP_OK;
}

// AVRCP commands
esp_err_t bluetooth_sink_play(void) {
    return esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY,
                                            ESP_AVRC_PT_CMD_STATE_PRESSED);
}

esp_err_t bluetooth_sink_pause(void) {
    return esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE,
                                            ESP_AVRC_PT_CMD_STATE_PRESSED);
}

esp_err_t bluetooth_sink_stop_playback(void) {
    return esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_STOP,
                                            ESP_AVRC_PT_CMD_STATE_PRESSED);
}

esp_err_t bluetooth_sink_next(void) {
    return esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD,
                                            ESP_AVRC_PT_CMD_STATE_PRESSED);
}

esp_err_t bluetooth_sink_prev(void) {
    return esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD,
                                            ESP_AVRC_PT_CMD_STATE_PRESSED);
}

esp_err_t bluetooth_sink_fast_forward(void) {
    return esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FAST_FORWARD,
                                            ESP_AVRC_PT_CMD_STATE_PRESSED);
}

esp_err_t bluetooth_sink_rewind(void) {
    return esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_REWIND,
                                            ESP_AVRC_PT_CMD_STATE_PRESSED);
}

esp_err_t bluetooth_sink_set_volume(uint8_t volume) {
    if (volume > 127) volume = 127;
    bt_volume = volume;
    // AVRCP absolute volume (if supported by source device)
    esp_avrc_ct_send_set_absolute_volume_cmd(0, volume);
    return ESP_OK;
}

uint8_t bluetooth_sink_get_volume(void) {
    return bt_volume;
}

bt_state_t bluetooth_sink_get_state(void) {
    return current_state;
}

bt_playback_status_t bluetooth_sink_get_playback_status(void) {
    return playback_status;
}

const bt_track_info_t *bluetooth_sink_get_track_info(void) {
    return &track_info;
}

const bt_device_info_t *bluetooth_sink_get_connected_device(void) {
    return &connected_device;
}

bool bluetooth_sink_is_connected(void) {
    return current_state >= BT_STATE_CONNECTED;
}

bool bluetooth_sink_is_streaming(void) {
    return current_state == BT_STATE_STREAMING;
}

void bluetooth_sink_register_state_callback(bt_state_callback_t callback) {
    state_callback = callback;
}

void bluetooth_sink_register_track_callback(bt_track_callback_t callback) {
    track_callback = callback;
}

void bluetooth_sink_register_playback_callback(bt_playback_callback_t callback) {
    playback_callback = callback;
}
