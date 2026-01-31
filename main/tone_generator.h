#ifndef TONE_GENERATOR_H
#define TONE_GENERATOR_H

#include "esp_err.h"

// Typy wbudowanych dźwięków budzika
typedef enum {
    ALARM_TONE_BEEP = 0,        // Prosty beep
    ALARM_TONE_CLASSIC,         // Klasyczny budzik (beep-beep-beep)
    ALARM_TONE_GENTLE,          // Łagodne budzenie (narastający ton)
    ALARM_TONE_MELODY,          // Prosta melodia
    ALARM_TONE_BIRD,            // Ptaki (świergot)
    ALARM_TONE_CHIME,           // Dzwonki
    ALARM_TONE_URGENT,          // Pilny (szybki, głośny)
    ALARM_TONE_MAX
} alarm_tone_t;

// Struktura opisująca ton
typedef struct {
    alarm_tone_t type;
    const char *name;
    const char *description;
} alarm_tone_info_t;

// Inicjalizacja generatora tonów
esp_err_t tone_generator_init(void);
esp_err_t tone_generator_deinit(void);

// Odtwarzanie tonu
esp_err_t tone_generator_play(alarm_tone_t tone, int volume);
esp_err_t tone_generator_stop(void);
bool tone_generator_is_playing(void);

// Informacje o tonach
const alarm_tone_info_t *tone_generator_get_info(alarm_tone_t tone);
const alarm_tone_info_t *tone_generator_get_all(uint8_t *count);

// Generowanie pojedynczego tonu (częstotliwość w Hz, czas w ms)
esp_err_t tone_generator_beep(uint16_t frequency, uint16_t duration_ms, int volume);

#endif // TONE_GENERATOR_H
