/*
 * Tone Generator Module
 * Generates alarm tones using I2S DAC
 */

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "board.h"

#include "tone_generator.h"
#include "config.h"

static const char *TAG = "TONE_GEN";

// Stałe audio
#define SAMPLE_RATE     44100
#define BITS_PER_SAMPLE 16
#define PI              3.14159265358979323846

// Stan generatora
static bool is_playing = false;
static bool stop_requested = false;
static TaskHandle_t tone_task_handle = NULL;
static audio_board_handle_t board_handle = NULL;

// Informacje o tonach
static const alarm_tone_info_t tone_info[] = {
    { ALARM_TONE_BEEP,    "Beep",       "Prosty sygnał" },
    { ALARM_TONE_CLASSIC, "Klasyczny",  "Tradycyjny budzik" },
    { ALARM_TONE_GENTLE,  "Lagodny",    "Narastający ton" },
    { ALARM_TONE_MELODY,  "Melodia",    "Prosta melodyjka" },
    { ALARM_TONE_BIRD,    "Ptaki",      "Swiergot ptakow" },
    { ALARM_TONE_CHIME,   "Dzwonki",    "Delikatne dzwonki" },
    { ALARM_TONE_URGENT,  "Pilny",      "Szybki alarm" },
};

// Nuty (częstotliwości w Hz)
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_G5  784

// ============================================
// Generowanie sampli
// ============================================

static void generate_sine_wave(int16_t *buffer, size_t samples, uint16_t freq, int volume)
{
    float amplitude = (32767.0f * volume) / 100.0f;
    for (size_t i = 0; i < samples; i++) {
        float sample = amplitude * sinf(2.0f * PI * freq * i / SAMPLE_RATE);
        buffer[i * 2] = (int16_t)sample;      // Left
        buffer[i * 2 + 1] = (int16_t)sample;  // Right
    }
}

static void generate_silence(int16_t *buffer, size_t samples)
{
    memset(buffer, 0, samples * 4);  // stereo 16-bit
}

// Funkcja pomocnicza - odtwórz ton przez określony czas
static esp_err_t play_tone_ms(uint16_t freq, uint16_t duration_ms, int volume)
{
    if (stop_requested) return ESP_OK;

    size_t samples_per_chunk = 1024;
    size_t total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    int16_t *buffer = malloc(samples_per_chunk * 4);  // stereo 16-bit

    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t samples_played = 0;
    while (samples_played < total_samples && !stop_requested) {
        size_t chunk = (total_samples - samples_played < samples_per_chunk)
                       ? (total_samples - samples_played) : samples_per_chunk;

        if (freq > 0) {
            generate_sine_wave(buffer, chunk, freq, volume);
        } else {
            generate_silence(buffer, chunk);
        }

        // TODO: Implement proper I2S output through audio pipeline
        // For now, just delay to simulate playback time
        size_t bytes_written = chunk * 4;
        vTaskDelay(pdMS_TO_TICKS((chunk * 1000) / SAMPLE_RATE));

        samples_played += chunk;
    }

    free(buffer);
    return ESP_OK;
}

// ============================================
// Wzorce alarmów
// ============================================

static void play_beep_pattern(int volume)
{
    // Prosty beep: 500ms ton, 500ms cisza, powtarzaj
    while (!stop_requested) {
        play_tone_ms(NOTE_A4, 500, volume);
        play_tone_ms(0, 500, volume);
    }
}

static void play_classic_pattern(int volume)
{
    // Klasyczny budzik: beep-beep-beep, pauza
    while (!stop_requested) {
        for (int i = 0; i < 3 && !stop_requested; i++) {
            play_tone_ms(NOTE_A4, 150, volume);
            play_tone_ms(0, 100, volume);
        }
        play_tone_ms(0, 700, volume);
    }
}

static void play_gentle_pattern(int volume)
{
    // Łagodne budzenie: narastający ton
    int current_volume = 10;
    while (!stop_requested) {
        play_tone_ms(NOTE_C5, 1000, current_volume);
        play_tone_ms(0, 500, 0);

        if (current_volume < volume) {
            current_volume += 5;
            if (current_volume > volume) current_volume = volume;
        }
    }
}

static void play_melody_pattern(int volume)
{
    // Prosta melodyjka (powtarzana)
    const uint16_t melody[] = {
        NOTE_C4, NOTE_E4, NOTE_G4, NOTE_C5,
        NOTE_G4, NOTE_E4, NOTE_C4, 0
    };
    const uint16_t durations[] = {
        200, 200, 200, 400,
        200, 200, 400, 500
    };

    while (!stop_requested) {
        for (int i = 0; i < 8 && !stop_requested; i++) {
            play_tone_ms(melody[i], durations[i], volume);
        }
    }
}

static void play_bird_pattern(int volume)
{
    // Świergot ptaków (szybkie tryle)
    while (!stop_requested) {
        // Pierwszy ptak
        for (int i = 0; i < 4 && !stop_requested; i++) {
            play_tone_ms(NOTE_E5, 50, volume);
            play_tone_ms(NOTE_G5, 50, volume);
        }
        play_tone_ms(0, 300, 0);

        // Drugi ptak
        for (int i = 0; i < 3 && !stop_requested; i++) {
            play_tone_ms(NOTE_D5, 80, volume);
            play_tone_ms(NOTE_E5, 80, volume);
        }
        play_tone_ms(0, 500, 0);
    }
}

static void play_chime_pattern(int volume)
{
    // Dzwonki - akordy
    const uint16_t chimes[] = {
        NOTE_C5, NOTE_E5, NOTE_G5, NOTE_C5
    };

    while (!stop_requested) {
        for (int i = 0; i < 4 && !stop_requested; i++) {
            play_tone_ms(chimes[i], 500, volume);
            play_tone_ms(0, 100, 0);
        }
        play_tone_ms(0, 1000, 0);
    }
}

static void play_urgent_pattern(int volume)
{
    // Pilny alarm - szybki i głośny
    while (!stop_requested) {
        play_tone_ms(NOTE_A4, 100, volume);
        play_tone_ms(NOTE_E5, 100, volume);
    }
}

// ============================================
// Task odtwarzania
// ============================================

typedef struct {
    alarm_tone_t tone;
    int volume;
} tone_task_params_t;

static void tone_play_task(void *pvParameters)
{
    tone_task_params_t *params = (tone_task_params_t *)pvParameters;
    alarm_tone_t tone = params->tone;
    int volume = params->volume;
    free(params);

    is_playing = true;
    stop_requested = false;

    ESP_LOGI(TAG, "Playing tone: %s (volume: %d)", tone_info[tone].name, volume);

    switch (tone) {
        case ALARM_TONE_BEEP:
            play_beep_pattern(volume);
            break;
        case ALARM_TONE_CLASSIC:
            play_classic_pattern(volume);
            break;
        case ALARM_TONE_GENTLE:
            play_gentle_pattern(volume);
            break;
        case ALARM_TONE_MELODY:
            play_melody_pattern(volume);
            break;
        case ALARM_TONE_BIRD:
            play_bird_pattern(volume);
            break;
        case ALARM_TONE_CHIME:
            play_chime_pattern(volume);
            break;
        case ALARM_TONE_URGENT:
            play_urgent_pattern(volume);
            break;
        default:
            play_beep_pattern(volume);
            break;
    }

    is_playing = false;
    tone_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============================================
// Publiczne API
// ============================================

esp_err_t tone_generator_init(void)
{
    ESP_LOGI(TAG, "Initializing tone generator...");

    board_handle = audio_board_get_handle();
    if (board_handle == NULL) {
        ESP_LOGE(TAG, "Audio board not initialized");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Tone generator initialized");
    return ESP_OK;
}

esp_err_t tone_generator_deinit(void)
{
    tone_generator_stop();
    return ESP_OK;
}

esp_err_t tone_generator_play(alarm_tone_t tone, int volume)
{
    if (tone >= ALARM_TONE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // Zatrzymaj poprzedni ton
    if (is_playing) {
        tone_generator_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Parametry dla taska
    tone_task_params_t *params = malloc(sizeof(tone_task_params_t));
    if (params == NULL) {
        return ESP_ERR_NO_MEM;
    }
    params->tone = tone;
    params->volume = volume;

    // Uruchom task odtwarzania
    BaseType_t ret = xTaskCreate(tone_play_task, "tone_play", 4096, params, 5, &tone_task_handle);
    if (ret != pdPASS) {
        free(params);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t tone_generator_stop(void)
{
    if (is_playing) {
        ESP_LOGI(TAG, "Stopping tone");
        stop_requested = true;

        // Czekaj na zakończenie taska
        int timeout = 20;  // 2 sekundy
        while (is_playing && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout--;
        }

        if (is_playing && tone_task_handle != NULL) {
            vTaskDelete(tone_task_handle);
            tone_task_handle = NULL;
            is_playing = false;
        }
    }

    return ESP_OK;
}

bool tone_generator_is_playing(void)
{
    return is_playing;
}

const alarm_tone_info_t *tone_generator_get_info(alarm_tone_t tone)
{
    if (tone >= ALARM_TONE_MAX) {
        return NULL;
    }
    return &tone_info[tone];
}

const alarm_tone_info_t *tone_generator_get_all(uint8_t *count)
{
    *count = ALARM_TONE_MAX;
    return tone_info;
}

esp_err_t tone_generator_beep(uint16_t frequency, uint16_t duration_ms, int volume)
{
    if (board_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return play_tone_ms(frequency, duration_ms, volume);
}
