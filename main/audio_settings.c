/*
 * Audio Settings Module
 * 10-band Equalizer, balance, and audio effects for ES8388 codec
 */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "board.h"
#include "audio_hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "audio_settings.h"
#include "audio_player.h"
#include "equalizer.h"
#include "config.h"

static const char *TAG = "AUDIO_SET";

// Debounced save - avoid blocking audio with flash writes
static TimerHandle_t save_timer = NULL;
static bool settings_dirty = false;
#define SAVE_DEBOUNCE_MS 1000  // Save 1 second after last change

// Informacje o pasmach częstotliwości
static const eq_band_info_t band_info[EQ_BANDS] = {
    { EQ_BAND_31HZ,  31,    "31"  },
    { EQ_BAND_62HZ,  62,    "62"  },
    { EQ_BAND_125HZ, 125,   "125" },
    { EQ_BAND_250HZ, 250,   "250" },
    { EQ_BAND_500HZ, 500,   "500" },
    { EQ_BAND_1KHZ,  1000,  "1k"  },
    { EQ_BAND_2KHZ,  2000,  "2k"  },
    { EQ_BAND_4KHZ,  4000,  "4k"  },
    { EQ_BAND_8KHZ,  8000,  "8k"  },
    { EQ_BAND_16KHZ, 16000, "16k" },
};

// Definicje presetów (10 pasm: 31, 62, 125, 250, 500, 1k, 2k, 4k, 8k, 16k)
// Wartości 0-24, gdzie 12 = 0dB
static const eq_preset_info_t presets[] = {
    { EQ_PRESET_FLAT,       "Flat",
      { 12, 12, 12, 12, 12, 12, 12, 12, 12, 12 } },

    { EQ_PRESET_ROCK,       "Rock",
      { 15, 14, 10, 9, 11, 13, 15, 15, 14, 14 } },

    { EQ_PRESET_POP,        "Pop",
      { 10, 11, 13, 15, 15, 14, 12, 11, 12, 12 } },

    { EQ_PRESET_JAZZ,       "Jazz",
      { 14, 13, 11, 13, 10, 12, 12, 13, 14, 14 } },

    { EQ_PRESET_CLASSICAL,  "Classical",
      { 12, 12, 12, 12, 12, 10, 9, 9, 11, 13 } },

    { EQ_PRESET_BASS_BOOST, "Bass+",
      { 18, 17, 15, 13, 12, 12, 12, 12, 12, 12 } },

    { EQ_PRESET_VOCAL,      "Vocal",
      { 9, 10, 12, 14, 16, 16, 15, 13, 11, 10 } },

    { EQ_PRESET_ELECTRONIC, "Electronic",
      { 16, 15, 12, 10, 11, 10, 12, 14, 15, 16 } },

    { EQ_PRESET_ACOUSTIC,   "Acoustic",
      { 13, 13, 12, 12, 13, 13, 12, 12, 13, 12 } },

    { EQ_PRESET_CUSTOM,     "Custom",
      { 12, 12, 12, 12, 12, 12, 12, 12, 12, 12 } },
};

// Aktualne ustawienia
static audio_settings_t settings = {
    .volume = 50,
    .bands = { 12, 12, 12, 12, 12, 12, 12, 12, 12, 12 },
    .balance = 0,
    .bass_boost = false,
    .loudness = false,
    .stereo_wide = false,
    .preset = EQ_PRESET_FLAT,
    .custom_preset = -1,
    .custom_presets = {
        { .used = false, .name = "", .bands = {12,12,12,12,12,12,12,12,12,12} },
        { .used = false, .name = "", .bands = {12,12,12,12,12,12,12,12,12,12} },
        { .used = false, .name = "", .bands = {12,12,12,12,12,12,12,12,12,12} },
    },
    .autostart = false,
    .last_url = "",
};

// NVS handle
static nvs_handle_t audio_nvs_handle;

// Audio board handle
static audio_board_handle_t board_handle = NULL;

// Forward declaration
static esp_err_t audio_settings_save_internal(void);

// Timer callback - performs actual save after debounce period
static void save_timer_callback(TimerHandle_t xTimer)
{
    if (settings_dirty) {
        ESP_LOGI(TAG, "Debounced save triggered");
        audio_settings_save_internal();
        settings_dirty = false;
    }
}

// Schedule a debounced save (resets timer on each call)
static void schedule_save(void)
{
    settings_dirty = true;
    if (save_timer != NULL) {
        // Reset the timer - save will happen SAVE_DEBOUNCE_MS after last change
        xTimerReset(save_timer, 0);
    }
}

// ============================================
// Wewnętrzne funkcje
// ============================================

static esp_err_t apply_eq_to_codec(void)
{
    ESP_LOGI(TAG, "Applying 10-band EQ:");

    // Convert from 0-24 range (12=0dB) to dB (-12 to +12)
    int gains_db[EQ_BANDS];
    for (int i = 0; i < EQ_BANDS; i++) {
        gains_db[i] = (int)settings.bands[i] - 12;
        ESP_LOGI(TAG, "  %5s Hz: %+3d dB", band_info[i].label, gains_db[i]);
    }

    // Apply to audio pipeline equalizer
    esp_err_t ret = audio_player_set_eq_all_bands(gains_db);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not apply EQ to pipeline (equalizer may not be ready)");
    }

    return ESP_OK;
}

static esp_err_t apply_balance_to_codec(void)
{
    // Balance is implemented by adjusting L/R channel gains in equalizer
    // Balance -100 = full left, 0 = center, +100 = full right

    // Calculate attenuation for each channel (0 to -12 dB)
    int left_atten = 0;   // dB attenuation for left
    int right_atten = 0;  // dB attenuation for right

    if (settings.balance < 0) {
        // Balance to left - attenuate right channel
        right_atten = (settings.balance * 12) / 100;  // -12 to 0 dB
    } else if (settings.balance > 0) {
        // Balance to right - attenuate left channel
        left_atten = -(settings.balance * 12) / 100;  // -12 to 0 dB
    }

    ESP_LOGI(TAG, "Balance: %d (L atten=%d dB, R atten=%d dB)",
             settings.balance, left_atten, right_atten);

    // Apply balance through equalizer by adjusting all bands with offset
    audio_element_handle_t eq = audio_player_get_equalizer();
    if (eq == NULL) {
        ESP_LOGW(TAG, "Equalizer not available for balance");
        return ESP_ERR_INVALID_STATE;
    }

    // Re-apply all EQ bands with balance offset
    for (int i = 0; i < EQ_BANDS; i++) {
        int base_db = (int)settings.bands[i] - 12;

        // Left channel (index 0-9)
        int left_db = base_db + left_atten;
        if (left_db < -13) left_db = -13;
        if (left_db > 13) left_db = 13;
        equalizer_set_gain_info(eq, i, left_db, false);

        // Right channel (index 10-19)
        int right_db = base_db + right_atten;
        if (right_db < -13) right_db = -13;
        if (right_db > 13) right_db = 13;
        equalizer_set_gain_info(eq, i + 10, right_db, false);
    }

    return ESP_OK;
}

// ============================================
// Publiczne API
// ============================================

esp_err_t audio_settings_init(void)
{
    ESP_LOGI(TAG, "Initializing audio settings (10-band EQ)...");

    board_handle = audio_board_get_handle();

    esp_err_t ret = nvs_open("audio_settings", NVS_READWRITE, &audio_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS");
        return ret;
    }

    // Create debounce timer for deferred saving (prevents flash writes blocking audio)
    save_timer = xTimerCreate("save_timer", pdMS_TO_TICKS(SAVE_DEBOUNCE_MS),
                              pdFALSE,  // One-shot timer
                              NULL, save_timer_callback);
    if (save_timer == NULL) {
        ESP_LOGW(TAG, "Failed to create save timer, will use direct saves");
    }

    audio_settings_load();
    apply_eq_to_codec();
    apply_balance_to_codec();

    ESP_LOGI(TAG, "Audio settings initialized");
    return ESP_OK;
}

esp_err_t audio_settings_set_band(eq_band_t band, uint8_t level)
{
    if (band >= EQ_BANDS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (level > EQ_MAX) level = EQ_MAX;

    settings.bands[band] = level;
    settings.preset = EQ_PRESET_CUSTOM;

    int gain_db = (int)level - 12;
    ESP_LOGI(TAG, "EQ Band %s: %+d dB", band_info[band].label, gain_db);

    // Apply single band to equalizer (more efficient than all bands)
    audio_player_set_eq_band(band, gain_db);

    return ESP_OK;
}

esp_err_t audio_settings_set_all_bands(const uint8_t *levels)
{
    for (int i = 0; i < EQ_BANDS; i++) {
        settings.bands[i] = (levels[i] > EQ_MAX) ? EQ_MAX : levels[i];
    }
    settings.preset = EQ_PRESET_CUSTOM;

    return apply_eq_to_codec();
}

uint8_t audio_settings_get_band(eq_band_t band)
{
    if (band >= EQ_BANDS) {
        return EQ_CENTER;
    }
    return settings.bands[band];
}

const eq_band_info_t *audio_settings_get_band_info(eq_band_t band)
{
    if (band >= EQ_BANDS) {
        return NULL;
    }
    return &band_info[band];
}

const eq_band_info_t *audio_settings_get_all_bands_info(void)
{
    return band_info;
}

esp_err_t audio_settings_set_balance(int8_t balance)
{
    if (balance < -100) balance = -100;
    if (balance > 100) balance = 100;
    settings.balance = balance;
    return apply_balance_to_codec();
}

esp_err_t audio_settings_set_bass_boost(bool enable)
{
    settings.bass_boost = enable;
    ESP_LOGI(TAG, "Bass boost: %s", enable ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t audio_settings_set_loudness(bool enable)
{
    settings.loudness = enable;
    ESP_LOGI(TAG, "Loudness: %s", enable ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t audio_settings_set_stereo_wide(bool enable)
{
    settings.stereo_wide = enable;
    ESP_LOGI(TAG, "Stereo wide: %s", enable ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t audio_settings_apply_preset(eq_preset_t preset)
{
    if (preset >= EQ_PRESET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const eq_preset_info_t *p = &presets[preset];
    memcpy(settings.bands, p->bands, EQ_BANDS);
    settings.preset = preset;

    ESP_LOGI(TAG, "Applied preset: %s", p->name);

    return apply_eq_to_codec();
}

const eq_preset_info_t *audio_settings_get_presets(uint8_t *count)
{
    *count = EQ_PRESET_MAX;
    return presets;
}

audio_settings_t *audio_settings_get(void)
{
    return &settings;
}

// Internal save function - actually writes to flash (can block!)
static esp_err_t audio_settings_save_internal(void)
{
    ESP_LOGI(TAG, "Saving audio settings to flash...");

    nvs_set_u8(audio_nvs_handle, "volume", settings.volume);
    nvs_set_blob(audio_nvs_handle, "eq_bands", settings.bands, EQ_BANDS);
    nvs_set_i8(audio_nvs_handle, "balance", settings.balance);
    nvs_set_u8(audio_nvs_handle, "bass_boost", settings.bass_boost ? 1 : 0);
    nvs_set_u8(audio_nvs_handle, "loudness", settings.loudness ? 1 : 0);
    nvs_set_u8(audio_nvs_handle, "stereo_wide", settings.stereo_wide ? 1 : 0);
    nvs_set_i8(audio_nvs_handle, "preset", settings.preset);
    nvs_set_i8(audio_nvs_handle, "custom_preset", settings.custom_preset);

    // Zapisz własne presety
    for (int i = 0; i < CUSTOM_PRESETS_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "cpreset%d", i);
        nvs_set_blob(audio_nvs_handle, key, &settings.custom_presets[i], sizeof(custom_preset_t));
    }

    // Zapisz autostart
    nvs_set_u8(audio_nvs_handle, "autostart", settings.autostart ? 1 : 0);
    nvs_set_str(audio_nvs_handle, "last_url", settings.last_url);

    return nvs_commit(audio_nvs_handle);
}

// Public save - uses debounced timer to avoid blocking audio
esp_err_t audio_settings_save(void)
{
    schedule_save();
    return ESP_OK;
}

// Flush - immediate save, use before shutdown
esp_err_t audio_settings_flush(void)
{
    if (save_timer != NULL) {
        xTimerStop(save_timer, 0);
    }
    if (settings_dirty) {
        settings_dirty = false;
        return audio_settings_save_internal();
    }
    return ESP_OK;
}

esp_err_t audio_settings_load(void)
{
    ESP_LOGI(TAG, "Loading audio settings...");

    size_t size = EQ_BANDS;
    nvs_get_blob(audio_nvs_handle, "eq_bands", settings.bands, &size);

    uint8_t val;
    int8_t sval;

    if (nvs_get_u8(audio_nvs_handle, "volume", &val) == ESP_OK) settings.volume = val;
    if (nvs_get_i8(audio_nvs_handle, "balance", &sval) == ESP_OK) settings.balance = sval;
    if (nvs_get_u8(audio_nvs_handle, "bass_boost", &val) == ESP_OK) settings.bass_boost = val;
    if (nvs_get_u8(audio_nvs_handle, "loudness", &val) == ESP_OK) settings.loudness = val;
    if (nvs_get_u8(audio_nvs_handle, "stereo_wide", &val) == ESP_OK) settings.stereo_wide = val;
    if (nvs_get_i8(audio_nvs_handle, "preset", &sval) == ESP_OK) settings.preset = sval;
    if (nvs_get_i8(audio_nvs_handle, "custom_preset", &sval) == ESP_OK) settings.custom_preset = sval;

    // Wczytaj własne presety
    for (int i = 0; i < CUSTOM_PRESETS_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "cpreset%d", i);
        size = sizeof(custom_preset_t);
        nvs_get_blob(audio_nvs_handle, key, &settings.custom_presets[i], &size);
    }

    // Wczytaj autostart
    if (nvs_get_u8(audio_nvs_handle, "autostart", &val) == ESP_OK) settings.autostart = val;
    size = LAST_URL_MAX_LEN;
    nvs_get_str(audio_nvs_handle, "last_url", settings.last_url, &size);

    return ESP_OK;
}

esp_err_t audio_settings_reset(void)
{
    ESP_LOGI(TAG, "Resetting audio settings to defaults...");

    settings.volume = 50;
    for (int i = 0; i < EQ_BANDS; i++) {
        settings.bands[i] = EQ_CENTER;
    }
    settings.balance = 0;
    settings.bass_boost = false;
    settings.loudness = false;
    settings.stereo_wide = false;
    settings.preset = EQ_PRESET_FLAT;
    settings.custom_preset = -1;

    // Reset własnych presetów
    for (int i = 0; i < CUSTOM_PRESETS_MAX; i++) {
        settings.custom_presets[i].used = false;
        settings.custom_presets[i].name[0] = '\0';
        for (int j = 0; j < EQ_BANDS; j++) {
            settings.custom_presets[i].bands[j] = EQ_CENTER;
        }
    }

    apply_eq_to_codec();
    apply_balance_to_codec();

    return audio_settings_save();
}

// ============================================
// Własne presety użytkownika
// ============================================

esp_err_t audio_settings_save_custom_preset(uint8_t slot, const char *name)
{
    if (slot >= CUSTOM_PRESETS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    settings.custom_presets[slot].used = true;
    strncpy(settings.custom_presets[slot].name, name, CUSTOM_PRESET_NAME_LEN - 1);
    settings.custom_presets[slot].name[CUSTOM_PRESET_NAME_LEN - 1] = '\0';
    memcpy(settings.custom_presets[slot].bands, settings.bands, EQ_BANDS);

    ESP_LOGI(TAG, "Saved custom preset %d: %s", slot, name);

    return audio_settings_save();
}

esp_err_t audio_settings_load_custom_preset(uint8_t slot)
{
    if (slot >= CUSTOM_PRESETS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!settings.custom_presets[slot].used) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(settings.bands, settings.custom_presets[slot].bands, EQ_BANDS);
    settings.preset = -1;  // Custom
    settings.custom_preset = slot;

    ESP_LOGI(TAG, "Loaded custom preset %d: %s", slot, settings.custom_presets[slot].name);

    apply_eq_to_codec();
    return audio_settings_save();
}

esp_err_t audio_settings_delete_custom_preset(uint8_t slot)
{
    if (slot >= CUSTOM_PRESETS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    settings.custom_presets[slot].used = false;
    settings.custom_presets[slot].name[0] = '\0';
    for (int i = 0; i < EQ_BANDS; i++) {
        settings.custom_presets[slot].bands[i] = EQ_CENTER;
    }

    if (settings.custom_preset == slot) {
        settings.custom_preset = -1;
    }

    ESP_LOGI(TAG, "Deleted custom preset %d", slot);

    return audio_settings_save();
}

const custom_preset_t *audio_settings_get_custom_preset(uint8_t slot)
{
    if (slot >= CUSTOM_PRESETS_MAX) {
        return NULL;
    }
    return &settings.custom_presets[slot];
}

// ============================================
// Głośność
// ============================================

esp_err_t audio_settings_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    settings.volume = volume;
    ESP_LOGI(TAG, "Volume set to: %d", volume);
    return audio_settings_save();
}

uint8_t audio_settings_get_volume(void)
{
    return settings.volume;
}

// ============================================
// Autostart
// ============================================

esp_err_t audio_settings_set_autostart(bool enabled)
{
    settings.autostart = enabled;
    ESP_LOGI(TAG, "Autostart: %s", enabled ? "ON" : "OFF");
    return audio_settings_save();
}

bool audio_settings_get_autostart(void)
{
    return settings.autostart;
}

esp_err_t audio_settings_set_last_url(const char *url)
{
    if (url == NULL) {
        settings.last_url[0] = '\0';
    } else {
        strncpy(settings.last_url, url, LAST_URL_MAX_LEN - 1);
        settings.last_url[LAST_URL_MAX_LEN - 1] = '\0';
    }
    ESP_LOGI(TAG, "Last URL saved: %s", settings.last_url);
    return audio_settings_save();
}

const char *audio_settings_get_last_url(void)
{
    return settings.last_url;
}
