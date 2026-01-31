#ifndef AUDIO_SETTINGS_H
#define AUDIO_SETTINGS_H

#include "esp_err.h"

// Zakres EQ: -12dB do +12dB (wartości 0-24, gdzie 12 = 0dB)
#define EQ_MIN      0
#define EQ_MAX      24
#define EQ_CENTER   12
#define EQ_BANDS    10

// Własne presety użytkownika
#define CUSTOM_PRESETS_MAX  3
#define CUSTOM_PRESET_NAME_LEN 16

// Autostart - max URL length
#define LAST_URL_MAX_LEN 256

// Częstotliwości środkowe pasm (Hz)
// 31, 62, 125, 250, 500, 1k, 2k, 4k, 8k, 16k
typedef enum {
    EQ_BAND_31HZ = 0,
    EQ_BAND_62HZ,
    EQ_BAND_125HZ,
    EQ_BAND_250HZ,
    EQ_BAND_500HZ,
    EQ_BAND_1KHZ,
    EQ_BAND_2KHZ,
    EQ_BAND_4KHZ,
    EQ_BAND_8KHZ,
    EQ_BAND_16KHZ,
} eq_band_t;

// Preset equalizera
typedef enum {
    EQ_PRESET_FLAT = 0,     // Płaski
    EQ_PRESET_ROCK,         // Rock
    EQ_PRESET_POP,          // Pop
    EQ_PRESET_JAZZ,         // Jazz
    EQ_PRESET_CLASSICAL,    // Klasyczna
    EQ_PRESET_BASS_BOOST,   // Wzmocnienie basów
    EQ_PRESET_VOCAL,        // Wokal
    EQ_PRESET_ELECTRONIC,   // Elektronika
    EQ_PRESET_ACOUSTIC,     // Akustyczna
    EQ_PRESET_CUSTOM,       // Własne ustawienia
    EQ_PRESET_MAX
} eq_preset_t;

// Własny preset użytkownika
typedef struct {
    bool used;                              // Czy slot jest używany
    char name[CUSTOM_PRESET_NAME_LEN];      // Nazwa presetu
    uint8_t bands[EQ_BANDS];                // Wartości EQ
} custom_preset_t;

// Struktura ustawień audio
typedef struct {
    // Głośność (0-100)
    uint8_t volume;

    // 10-pasmowy equalizer (0-24, 12 = 0dB)
    // Pasma: 31Hz, 62Hz, 125Hz, 250Hz, 500Hz, 1kHz, 2kHz, 4kHz, 8kHz, 16kHz
    uint8_t bands[EQ_BANDS];

    // Balans (-100 do +100, 0 = środek)
    int8_t balance;

    // Efekty
    bool bass_boost;        // Wzmocnienie basów
    bool loudness;          // Loudness (wzmocnienie przy niskiej głośności)
    bool stereo_wide;       // Poszerzenie stereo

    // Aktywny preset (-1 = custom user preset, 0+ = built-in)
    int8_t preset;

    // Aktywny własny preset (-1 = brak)
    int8_t custom_preset;

    // Własne presety użytkownika (3 sloty)
    custom_preset_t custom_presets[CUSTOM_PRESETS_MAX];

    // Autostart z ostatnią stacją
    bool autostart;
    char last_url[LAST_URL_MAX_LEN];
} audio_settings_t;

// Informacja o presecie
typedef struct {
    eq_preset_t type;
    const char *name;
    uint8_t bands[EQ_BANDS];  // Wartości dla każdego pasma
} eq_preset_info_t;

// Informacja o paśmie
typedef struct {
    eq_band_t band;
    uint16_t frequency;     // Hz
    const char *label;      // "31", "62", "125", etc.
} eq_band_info_t;

// Inicjalizacja
esp_err_t audio_settings_init(void);

// Ustawienia EQ
esp_err_t audio_settings_set_band(eq_band_t band, uint8_t level);
esp_err_t audio_settings_set_all_bands(const uint8_t *levels);
uint8_t audio_settings_get_band(eq_band_t band);

// Informacje o pasmach
const eq_band_info_t *audio_settings_get_band_info(eq_band_t band);
const eq_band_info_t *audio_settings_get_all_bands_info(void);

// Balans
esp_err_t audio_settings_set_balance(int8_t balance);

// Efekty
esp_err_t audio_settings_set_bass_boost(bool enable);
esp_err_t audio_settings_set_loudness(bool enable);
esp_err_t audio_settings_set_stereo_wide(bool enable);

// Presety wbudowane
esp_err_t audio_settings_apply_preset(eq_preset_t preset);
const eq_preset_info_t *audio_settings_get_presets(uint8_t *count);

// Własne presety użytkownika
esp_err_t audio_settings_save_custom_preset(uint8_t slot, const char *name);
esp_err_t audio_settings_load_custom_preset(uint8_t slot);
esp_err_t audio_settings_delete_custom_preset(uint8_t slot);
const custom_preset_t *audio_settings_get_custom_preset(uint8_t slot);

// Głośność
esp_err_t audio_settings_set_volume(uint8_t volume);
uint8_t audio_settings_get_volume(void);

// Autostart
esp_err_t audio_settings_set_autostart(bool enabled);
bool audio_settings_get_autostart(void);
esp_err_t audio_settings_set_last_url(const char *url);
const char *audio_settings_get_last_url(void);

// Pobieranie aktualnych ustawień
audio_settings_t *audio_settings_get(void);

// Zapis/odczyt z NVS
esp_err_t audio_settings_save(void);   // Debounced save - won't block audio
esp_err_t audio_settings_flush(void);  // Immediate save - use before shutdown
esp_err_t audio_settings_load(void);

// Reset do domyślnych
esp_err_t audio_settings_reset(void);

#endif // AUDIO_SETTINGS_H
