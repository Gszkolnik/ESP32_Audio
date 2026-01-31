/*
 * Bluetooth A2DP Source Module Implementation
 * Streams audio to external Bluetooth speakers/headphones
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "bluetooth_source.h"
#include "config.h"

static const char *TAG = "BT_SOURCE";

// Ring buffer for audio data
#define BT_SOURCE_RINGBUF_SIZE (8 * 1024)
static RingbufHandle_t s_ringbuf = NULL;

// Status and state
static bt_source_status_t s_status = {0};
static SemaphoreHandle_t s_status_mutex = NULL;

// Callbacks
static bt_source_state_callback_t s_state_callback = NULL;
static bt_source_discovery_callback_t s_discovery_callback = NULL;

// Connection management
static esp_bd_addr_t s_peer_bda = {0};
static bool s_a2dp_connected = false;
static bool s_avrc_connected = false;
static bool s_initialized = false;

// Audio data callback timer
static int32_t s_audio_data_cb(uint8_t *data, int32_t len);

// ============================================
// Helper functions
// ============================================

static void set_state(bt_source_state_t state) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.state = state;
        xSemaphoreGive(s_status_mutex);
    }

    ESP_LOGI(TAG, "State changed: %s", bt_source_state_to_str(state));

    if (s_state_callback) {
        s_state_callback(state, s_status.connected_device.name);
    }
}

__attribute__((unused))
static void set_error(const char *msg) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(s_status.error_msg, msg, sizeof(s_status.error_msg) - 1);
        s_status.state = BT_SOURCE_STATE_ERROR;
        xSemaphoreGive(s_status_mutex);
    }
    ESP_LOGE(TAG, "Error: %s", msg);
}

static void add_discovered_device(esp_bt_gap_cb_param_t *param) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    // Extract RSSI from properties if available
    int32_t rssi = 0;
    if (param->disc_res.prop) {
        for (int j = 0; j < param->disc_res.num_prop; j++) {
            if (param->disc_res.prop[j].type == ESP_BT_GAP_DEV_PROP_RSSI) {
                rssi = *(int8_t *)param->disc_res.prop[j].val;
                break;
            }
        }
    }

    // Check if device already exists
    for (int i = 0; i < s_status.device_count; i++) {
        if (memcmp(s_status.devices[i].bda, param->disc_res.bda, 6) == 0) {
            // Update RSSI and name if available
            s_status.devices[i].rssi = rssi;
            if (param->disc_res.prop) {
                for (int j = 0; j < param->disc_res.num_prop; j++) {
                    if (param->disc_res.prop[j].type == ESP_BT_GAP_DEV_PROP_EIR) {
                        uint8_t *eir = param->disc_res.prop[j].val;
                        uint8_t *name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, NULL);
                        if (!name) {
                            name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, NULL);
                        }
                        if (name) {
                            strncpy(s_status.devices[i].name, (char *)name, BT_SOURCE_DEVICE_NAME_LEN - 1);
                        }
                    }
                }
            }
            xSemaphoreGive(s_status_mutex);
            return;
        }
    }

    // Add new device
    if (s_status.device_count < BT_SOURCE_MAX_DEVICES) {
        bt_source_device_t *dev = &s_status.devices[s_status.device_count];
        memcpy(dev->bda, param->disc_res.bda, 6);
        dev->rssi = rssi;
        dev->name[0] = '\0';
        dev->is_audio_sink = false;

        // Parse properties
        if (param->disc_res.prop) {
            for (int j = 0; j < param->disc_res.num_prop; j++) {
                esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[j];

                if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME && prop->val) {
                    strncpy(dev->name, (char *)prop->val, BT_SOURCE_DEVICE_NAME_LEN - 1);
                }
                else if (prop->type == ESP_BT_GAP_DEV_PROP_COD) {
                    uint32_t cod = *(uint32_t *)prop->val;
                    // Check if device is an audio device (headphones, speaker, etc.)
                    uint32_t major = (cod >> 8) & 0x1F;
                    if (major == 0x04) { // Audio/Video
                        dev->is_audio_sink = true;
                    }
                }
                else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR) {
                    uint8_t *eir = prop->val;
                    uint8_t *name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, NULL);
                    if (!name) {
                        name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, NULL);
                    }
                    if (name && dev->name[0] == '\0') {
                        strncpy(dev->name, (char *)name, BT_SOURCE_DEVICE_NAME_LEN - 1);
                    }
                }
            }
        }

        // Generate name from BDA if not found
        if (dev->name[0] == '\0') {
            bt_source_bda_to_str(dev->bda, dev->name);
        }

        s_status.device_count++;

        char bda_str[18];
        bt_source_bda_to_str(dev->bda, bda_str);
        ESP_LOGI(TAG, "Found device: %s [%s] RSSI: %ld, Audio: %s",
                 dev->name, bda_str, (long)dev->rssi, dev->is_audio_sink ? "Yes" : "No");

        // Notify discovery callback
        if (s_discovery_callback) {
            s_discovery_callback(dev);
        }
    }

    xSemaphoreGive(s_status_mutex);
}

// ============================================
// GAP Callback
// ============================================

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            // Device discovered - log raw BDA
            char raw_bda[18];
            sprintf(raw_bda, "%02X:%02X:%02X:%02X:%02X:%02X",
                    param->disc_res.bda[0], param->disc_res.bda[1],
                    param->disc_res.bda[2], param->disc_res.bda[3],
                    param->disc_res.bda[4], param->disc_res.bda[5]);
            ESP_LOGI(TAG, "GAP: Device found [%s], props: %d", raw_bda, param->disc_res.num_prop);
            add_discovered_device(param);
            break;
        }

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                ESP_LOGI(TAG, "Discovery stopped, found %d devices", s_status.device_count);
                if (s_status.state == BT_SOURCE_STATE_DISCOVERING) {
                    set_state(BT_SOURCE_STATE_IDLE);
                }
            } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
                ESP_LOGI(TAG, "Discovery started - looking for Bluetooth Classic devices");
                ESP_LOGI(TAG, "Make sure target device is in PAIRING MODE!");
            }
            break;

        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Authentication complete: %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGE(TAG, "Authentication failed: %d", param->auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_PIN_REQ_EVT:
            ESP_LOGI(TAG, "PIN request - using default 0000");
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(TAG, "Confirmation request for code: %lu", (unsigned long)param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "Passkey notify: %lu", (unsigned long)param->key_notif.passkey);
            break;

        case ESP_BT_GAP_KEY_REQ_EVT:
            ESP_LOGI(TAG, "Passkey request");
            break;

        default:
            ESP_LOGD(TAG, "GAP event: %d", event);
            break;
    }
}

// ============================================
// A2DP Source Callback
// ============================================

static void bt_a2dp_source_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP connected");
                s_a2dp_connected = true;
                memcpy(s_peer_bda, param->conn_stat.remote_bda, 6);

                // Update connected device info
                if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    memcpy(s_status.connected_device.bda, s_peer_bda, 6);
                    // Find name from discovered devices
                    for (int i = 0; i < s_status.device_count; i++) {
                        if (memcmp(s_status.devices[i].bda, s_peer_bda, 6) == 0) {
                            strncpy(s_status.connected_device.name, s_status.devices[i].name,
                                    BT_SOURCE_DEVICE_NAME_LEN - 1);
                            break;
                        }
                    }
                    xSemaphoreGive(s_status_mutex);
                }

                set_state(BT_SOURCE_STATE_CONNECTED);
            }
            else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "A2DP disconnected");
                s_a2dp_connected = false;
                set_state(BT_SOURCE_STATE_IDLE);
            }
            else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
                ESP_LOGI(TAG, "A2DP connecting...");
                set_state(BT_SOURCE_STATE_CONNECTING);
            }
            else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTING) {
                ESP_LOGI(TAG, "A2DP disconnecting...");
                set_state(BT_SOURCE_STATE_DISCONNECTING);
            }
            break;

        case ESP_A2D_AUDIO_STATE_EVT:
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "Audio streaming started");
                set_state(BT_SOURCE_STATE_STREAMING);
            }
            else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED ||
                     param->audio_stat.state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
                ESP_LOGI(TAG, "Audio streaming stopped");
                if (s_a2dp_connected) {
                    set_state(BT_SOURCE_STATE_CONNECTED);
                }
            }
            break;

        case ESP_A2D_AUDIO_CFG_EVT:
            ESP_LOGI(TAG, "Audio config: sample_rate=%d",
                     param->audio_cfg.mcc.cie.sbc_info.samp_freq);
            break;

        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            ESP_LOGD(TAG, "Media control ACK: cmd=%d, status=%d",
                     param->media_ctrl_stat.cmd, param->media_ctrl_stat.status);
            break;

        default:
            ESP_LOGD(TAG, "A2DP event: %d", event);
            break;
    }
}

// ============================================
// Audio Data Callback (called by BT stack)
// ============================================

static int32_t s_audio_data_cb(uint8_t *data, int32_t len) {
    if (s_ringbuf == NULL || data == NULL || len <= 0) {
        // Return silence
        memset(data, 0, len);
        return len;
    }

    size_t bytes_read = 0;
    uint8_t *buf = xRingbufferReceiveUpTo(s_ringbuf, &bytes_read, 0, len);

    if (buf && bytes_read > 0) {
        memcpy(data, buf, bytes_read);
        vRingbufferReturnItem(s_ringbuf, buf);

        // Fill remaining with silence
        if (bytes_read < len) {
            memset(data + bytes_read, 0, len - bytes_read);
        }
        return len;
    }

    // No data available - return silence
    memset(data, 0, len);
    return len;
}

// ============================================
// AVRCP Controller Callback
// ============================================

static void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            if (param->conn_stat.connected) {
                ESP_LOGI(TAG, "AVRC connected");
                s_avrc_connected = true;
            } else {
                ESP_LOGI(TAG, "AVRC disconnected");
                s_avrc_connected = false;
            }
            break;

        case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
            ESP_LOGD(TAG, "AVRC passthrough response: key=0x%x, state=%d",
                     param->psth_rsp.key_code, param->psth_rsp.key_state);
            break;

        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            ESP_LOGD(TAG, "AVRC notify event: %d", param->change_ntf.event_id);
            break;

        case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
            ESP_LOGI(TAG, "AVRC remote features: 0x%lx", (unsigned long)param->rmt_feats.feat_mask);
            break;

        default:
            ESP_LOGD(TAG, "AVRC event: %d", event);
            break;
    }
}

// ============================================
// Public API Implementation
// ============================================

esp_err_t bt_source_init(void) {
    ESP_LOGI(TAG, "Initializing Bluetooth A2DP Source");

    // Create mutex
    if (s_status_mutex == NULL) {
        s_status_mutex = xSemaphoreCreateMutex();
        if (s_status_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Create ring buffer for audio
    if (s_ringbuf == NULL) {
        s_ringbuf = xRingbufferCreate(BT_SOURCE_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
        if (s_ringbuf == NULL) {
            ESP_LOGE(TAG, "Failed to create ring buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    // Initialize status
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = BT_SOURCE_STATE_IDLE;
    s_status.volume = 100;

    // Release BT classic memory for BLE if needed
    esp_err_t ret;

    // Initialize Bluetooth controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize Bluedroid
    ret = esp_bluedroid_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set device name
    esp_bt_gap_set_device_name(DEVICE_NAME "-Source");

    // Register GAP callback
    ret = esp_bt_gap_register_callback(bt_gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP source init failed: %s", esp_err_to_name(ret));
        return ret;
    }


    // Initialize A2DP Source (after AVRC)
    ret = esp_a2d_register_callback(bt_a2dp_source_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_register_data_callback(s_audio_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP data callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_init();

    // Initialize A2DP Source (after AVRC)
    ret = esp_a2d_register_callback(bt_a2dp_source_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_register_data_callback(s_audio_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP data callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_init();

    // Initialize A2DP Source (after AVRC)
    ret = esp_a2d_register_callback(bt_a2dp_source_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_register_data_callback(s_audio_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP data callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_init();

    // Initialize A2DP Source (after AVRC)
    ret = esp_a2d_register_callback(bt_a2dp_source_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_register_data_callback(s_audio_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP data callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_init();

    // Initialize A2DP Source (after AVRC)
    ret = esp_a2d_register_callback(bt_a2dp_source_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_register_data_callback(s_audio_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP data callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_init();

    // Initialize A2DP Source (after AVRC)
    ret = esp_a2d_register_callback(bt_a2dp_source_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_register_data_callback(s_audio_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP data callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_init();

    // Initialize A2DP Source (after AVRC)
    ret = esp_a2d_register_callback(bt_a2dp_source_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_register_data_callback(s_audio_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP data callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_init();
    }    // Initialize A2DP Source (after AVRC)
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AVRC init failed: %s", esp_err_to_name(ret));
    }

    // Set discoverability and connectability
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    s_initialized = true;
    ESP_LOGI(TAG, "Bluetooth A2DP Source initialized");
    return ESP_OK;
}

esp_err_t bt_source_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing Bluetooth Source");

    if (s_a2dp_connected) {
        bt_source_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_a2d_source_deinit();
    esp_avrc_ct_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    if (s_ringbuf) {
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
    }

    s_initialized = false;
    set_state(BT_SOURCE_STATE_IDLE);

    return ESP_OK;
}

esp_err_t bt_source_start_discovery(uint8_t duration_sec) {
    if (duration_sec == 0) {
        duration_sec = 10;
    }

    ESP_LOGI(TAG, "Starting device discovery for %d seconds", duration_sec);

    // Clear previous devices
    bt_source_clear_devices();

    set_state(BT_SOURCE_STATE_DISCOVERING);

    // Start discovery - looking for audio devices
    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                                duration_sec, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start discovery failed: %s", esp_err_to_name(ret));
        set_state(BT_SOURCE_STATE_IDLE);
        return ret;
    }

    return ESP_OK;
}

esp_err_t bt_source_stop_discovery(void) {
    ESP_LOGI(TAG, "Stopping device discovery");

    esp_err_t ret = esp_bt_gap_cancel_discovery();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stop discovery failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t bt_source_connect(const uint8_t *bda) {
    if (bda == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_a2dp_connected) {
        ESP_LOGW(TAG, "Already connected, disconnect first");
        return ESP_ERR_INVALID_STATE;
    }

    // Stop discovery if running
    if (s_status.state == BT_SOURCE_STATE_DISCOVERING) {
        bt_source_stop_discovery();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    char bda_str[18];
    bt_source_bda_to_str(bda, bda_str);
    ESP_LOGI(TAG, "Connecting to %s", bda_str);

    set_state(BT_SOURCE_STATE_CONNECTING);

    esp_err_t ret = esp_a2d_source_connect((uint8_t *)bda);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Connect failed: %s", esp_err_to_name(ret));
        set_state(BT_SOURCE_STATE_IDLE);
        return ret;
    }

    return ESP_OK;
}

esp_err_t bt_source_connect_by_index(uint8_t device_index) {
    if (device_index >= s_status.device_count) {
        ESP_LOGE(TAG, "Invalid device index: %d", device_index);
        return ESP_ERR_INVALID_ARG;
    }

    return bt_source_connect(s_status.devices[device_index].bda);
}

esp_err_t bt_source_disconnect(void) {
    if (!s_a2dp_connected) {
        ESP_LOGW(TAG, "Not connected");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Disconnecting");

    set_state(BT_SOURCE_STATE_DISCONNECTING);

    esp_err_t ret = esp_a2d_source_disconnect(s_peer_bda);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Disconnect failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

const bt_source_status_t *bt_source_get_status(void) {
    return &s_status;
}

bt_source_state_t bt_source_get_state(void) {
    return s_status.state;
}

bool bt_source_is_initialized(void) {
    return s_initialized;
}

bool bt_source_is_connected(void) {
    return s_a2dp_connected;
}

bool bt_source_is_streaming(void) {
    return s_status.state == BT_SOURCE_STATE_STREAMING;
}

esp_err_t bt_source_set_volume(uint8_t volume) {
    if (volume > 127) {
        volume = 127;
    }

    s_status.volume = volume;

    // Send volume change via AVRCP if connected
    if (s_avrc_connected) {
        // AVRCP volume is 0-127, first arg is transaction label
        esp_avrc_ct_send_set_absolute_volume_cmd(0, volume);
    }

    return ESP_OK;
}

uint8_t bt_source_get_volume(void) {
    return s_status.volume;
}

void bt_source_register_state_callback(bt_source_state_callback_t callback) {
    s_state_callback = callback;
}

void bt_source_register_discovery_callback(bt_source_discovery_callback_t callback) {
    s_discovery_callback = callback;
}

uint8_t bt_source_get_discovered_devices(bt_source_device_t *devices, uint8_t max_devices) {
    if (devices == NULL || max_devices == 0) {
        return 0;
    }

    uint8_t count = 0;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = (s_status.device_count < max_devices) ? s_status.device_count : max_devices;
        memcpy(devices, s_status.devices, count * sizeof(bt_source_device_t));
        xSemaphoreGive(s_status_mutex);
    }

    return count;
}

void bt_source_clear_devices(void) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.device_count = 0;
        memset(s_status.devices, 0, sizeof(s_status.devices));
        xSemaphoreGive(s_status_mutex);
    }
}

int bt_source_write_audio(const uint8_t *data, size_t len) {
    if (s_ringbuf == NULL || data == NULL || len == 0) {
        return 0;
    }

    if (!s_a2dp_connected) {
        return 0;
    }

    // Send to ring buffer
    if (xRingbufferSend(s_ringbuf, data, len, pdMS_TO_TICKS(10)) == pdTRUE) {
        return len;
    }

    return 0;
}

const char *bt_source_state_to_str(bt_source_state_t state) {
    switch (state) {
        case BT_SOURCE_STATE_IDLE:          return "idle";
        case BT_SOURCE_STATE_DISCOVERING:   return "discovering";
        case BT_SOURCE_STATE_CONNECTING:    return "connecting";
        case BT_SOURCE_STATE_CONNECTED:     return "connected";
        case BT_SOURCE_STATE_STREAMING:     return "streaming";
        case BT_SOURCE_STATE_DISCONNECTING: return "disconnecting";
        case BT_SOURCE_STATE_ERROR:         return "error";
        default:                            return "unknown";
    }
}

void bt_source_bda_to_str(const uint8_t *bda, char *str) {
    if (bda && str) {
        sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
                bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    }
}

esp_err_t bt_source_str_to_bda(const char *str, uint8_t *bda) {
    if (str == NULL || bda == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned int tmp[6];
    if (sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5]) != 6) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < 6; i++) {
        bda[i] = (uint8_t)tmp[i];
    }

    return ESP_OK;
}
