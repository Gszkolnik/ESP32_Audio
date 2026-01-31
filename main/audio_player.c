/*
 * Audio Player Module
 * Handles audio streaming and playback using ESP-ADF
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "audio_player.h"
#include "config.h"
#include "audio_settings.h"
#include "radio_stations.h"

#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "ringbuf.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "filter_resample.h"
#include "equalizer.h"
#include "board.h"
#include "esp_peripherals.h"
#include "periph_touch.h"
#include "periph_button.h"
#include "esp_crt_bundle.h"

static const char *TAG = "AUDIO_PLAYER";

// Pipeline i elementy
static audio_pipeline_handle_t pipeline = NULL;
static audio_element_handle_t http_stream = NULL;
static audio_element_handle_t decoder = NULL;
static audio_element_handle_t i2s_stream = NULL;
static audio_element_handle_t rsp_filter = NULL;
static audio_element_handle_t equalizer = NULL;
static audio_event_iface_handle_t evt = NULL;
static esp_periph_set_handle_t periph_set = NULL;

// EQ gain array for equalizer (stereo: 20 values, 10 per channel)
static int eq_gain[20] = {0};

// Stan odtwarzacza
static player_status_t player_status = {
    .state = PLAYER_STATE_IDLE,
    .source = AUDIO_SOURCE_NONE,
    .volume = DEFAULT_VOLUME,
    .muted = false,
    .current_url = "",
    .current_title = "",
    .current_artist = "",
};

// Callback
static player_state_callback_t state_callback = NULL;

// Audio board handle
static audio_board_handle_t board_handle = NULL;

// Task do obsługi zdarzeń
static TaskHandle_t event_task_handle = NULL;

// ============================================
// Pomocnicze funkcje
// ============================================

static void notify_state_change(void)
{
    if (state_callback) {
        state_callback(&player_status);
    }
}

static void set_state(player_state_t state)
{
    player_status.state = state;
    notify_state_change();
}

// Flaga do zapobiegania wielokrotnym reconnect
static bool reconnect_in_progress = false;

// Pre-buffering configuration
#define PREBUFFER_THRESHOLD_KB  128  // Start playback when 128KB buffered (~8s at 128kbps)
#define PREBUFFER_CHECK_MS      100  // Check buffer every 100ms
#define HTTP_BUFFER_SIZE_KB     256  // Total HTTP buffer size in KB

// Buffer monitoring
static int current_buffer_percent = 0;
static int prebuffer_counter = 0;
static TimerHandle_t prebuffer_timer = NULL;

#define PREBUFFER_TICKS  30  // 30 x 100ms = 3 seconds prebuffering

// Get buffer fill level (0-100%)
int audio_player_get_buffer_level(void)
{
    return current_buffer_percent;
}

// Get real buffer fill level from I2S input ringbuffer (MP3 output)
static int get_real_buffer_level(void)
{
    if (i2s_stream == NULL) return 0;

    ringbuf_handle_t rb = audio_element_get_input_ringbuf(i2s_stream);
    if (rb == NULL) return 0;

    int filled = rb_bytes_filled(rb);
    int total = rb_get_size(rb);

    if (total <= 0) return 0;
    return (filled * 100) / total;
}

// Pre-buffer timer callback - wait for buffer to fill, then start I2S
static void prebuffer_timer_callback(TimerHandle_t xTimer)
{
    // During buffering, count up and start I2S when buffer is full
    if (player_status.state == PLAYER_STATE_BUFFERING) {
        prebuffer_counter++;

        // Use real buffer level during buffering
        int real_level = get_real_buffer_level();
        current_buffer_percent = real_level > 0 ? real_level : (prebuffer_counter * 100) / PREBUFFER_TICKS;
        if (current_buffer_percent > 100) current_buffer_percent = 100;

        ESP_LOGI(TAG, "Buffering: %d%% (real: %d%%)", current_buffer_percent, real_level);

        if (prebuffer_counter >= PREBUFFER_TICKS) {
            ESP_LOGI(TAG, "Prebuffer complete, resuming I2S output");
            // Resume I2S - start playing from buffer
            audio_element_resume(i2s_stream, 0, portMAX_DELAY);
            set_state(PLAYER_STATE_PLAYING);
            current_buffer_percent = 100;
        }
    }
    // During playback, monitor real buffer level
    else if (player_status.state == PLAYER_STATE_PLAYING) {
        current_buffer_percent = get_real_buffer_level();
        prebuffer_counter = PREBUFFER_TICKS;

        // Warn if buffer is getting low (< 30%)
        if (current_buffer_percent < 30 && current_buffer_percent > 0) {
            ESP_LOGW(TAG, "Buffer low: %d%%", current_buffer_percent);
        }
    }
    // Reset on stop/idle
    else {
        prebuffer_counter = 0;
        current_buffer_percent = 0;
    }
}

// Jednorazowy task do zmiany stacji (wymaga większego stosu niż event task)
static void next_station_task(void *pvParameters)
{
    audio_player_play_next_station();
    vTaskDelete(NULL);
}

// Jednorazowy task do reconnect (nie blokuje event taska)
static void reconnect_task(void *pvParameters)
{
    const char *url = (const char *)pvParameters;
    if (url && strlen(url) > 0) {
        ESP_LOGI(TAG, "Reconnect task: reconnecting to %s", url);
        vTaskDelay(pdMS_TO_TICKS(500));  // Krótka pauza przed reconnect
        audio_player_play_url(url);
    }
    reconnect_in_progress = false;
    vTaskDelete(NULL);
}

// ============================================
// Task obsługi zdarzeń audio
// ============================================

static void audio_event_task(void *pvParameters)
{
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            continue;
        }

        // Zdarzenia HTTP stream (metadane, etc.)
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)http_stream &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {

            audio_element_info_t music_info = {0};
            audio_element_getinfo(http_stream, &music_info);
            ESP_LOGI(TAG, "Music info: sample_rate=%d, channels=%d, bits=%d",
                     music_info.sample_rates, music_info.channels, music_info.bits);

            // Konfiguracja filtra resampling jeśli potrzebna
            audio_element_set_music_info(i2s_stream, music_info.sample_rates,
                                         music_info.channels, music_info.bits);
        }

        // HTTP stream zakończył pobieranie - dla radia znaczy zerwane połączenie
        // Używamy osobnego taska żeby nie blokować event loop i web server
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)http_stream &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (int)msg.data == AEL_STATUS_STATE_FINISHED) {

            if (player_status.source == AUDIO_SOURCE_HTTP &&
                player_status.state == PLAYER_STATE_PLAYING &&
                strlen(player_status.current_url) > 0 &&
                !reconnect_in_progress) {
                ESP_LOGW(TAG, "HTTP stream ended, scheduling reconnect...");
                reconnect_in_progress = true;
                xTaskCreate(reconnect_task, "reconnect", 8192,
                           (void *)player_status.current_url, 5, NULL);
            }
        }

        // Zdarzenie zakończenia odtwarzania na I2S (fallback dla HTTP + inne źródła)
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)i2s_stream &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (int)msg.data == AEL_STATUS_STATE_FINISHED) {

            if (player_status.source == AUDIO_SOURCE_HTTP &&
                player_status.state == PLAYER_STATE_PLAYING &&
                strlen(player_status.current_url) > 0 &&
                !reconnect_in_progress) {
                // Fallback - reconnect jeśli HTTP handler nie złapał
                ESP_LOGW(TAG, "I2S finished, scheduling reconnect...");
                reconnect_in_progress = true;
                xTaskCreate(reconnect_task, "reconnect", 8192,
                           (void *)player_status.current_url, 5, NULL);
            } else if (player_status.source != AUDIO_SOURCE_HTTP) {
                ESP_LOGI(TAG, "Playback finished");
                set_state(PLAYER_STATE_STOPPED);
            }
        }

        // Zdarzenie błędu (AEL_STATUS_ERROR_* są w zakresie 1-7)
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (int)msg.data >= AEL_STATUS_ERROR_OPEN &&
            (int)msg.data <= AEL_STATUS_ERROR_UNKNOWN) {

            ESP_LOGE(TAG, "Playback error: %d", (int)msg.data);
            set_state(PLAYER_STATE_ERROR);
        }

        // Obsługa przycisków na płytce
        if ((msg.source_type == PERIPH_ID_TOUCH || msg.source_type == PERIPH_ID_BUTTON)
            && (msg.cmd == PERIPH_TOUCH_TAP || msg.cmd == PERIPH_BUTTON_PRESSED)) {
            if ((int)msg.data == get_input_play_id()) {
                ESP_LOGI(TAG, "Play/Pause button pressed");
                if (player_status.state == PLAYER_STATE_PLAYING) {
                    audio_player_pause();
                } else {
                    audio_player_resume();
                }
            } else if ((int)msg.data == get_input_volup_id()) {
                ESP_LOGI(TAG, "Volume Up button pressed");
                audio_player_set_volume(player_status.volume + 5);
            } else if ((int)msg.data == get_input_voldown_id()) {
                ESP_LOGI(TAG, "Volume Down button pressed");
                audio_player_set_volume(player_status.volume - 5);
            } else if ((int)msg.data == get_input_set_id()) {
                ESP_LOGI(TAG, "Set button pressed - next station");
                xTaskCreate(next_station_task, "next_st", 8192, NULL, 5, NULL);
            } else if ((int)msg.data == get_input_rec_id()) {
                ESP_LOGI(TAG, "Rec button pressed - next station");
                xTaskCreate(next_station_task, "next_st", 8192, NULL, 5, NULL);
            }
        }
    }
}

// ============================================
// Publiczne API
// ============================================

esp_err_t audio_player_init(void)
{
    ESP_LOGI(TAG, "Initializing audio player...");

    // Pobierz handle audio board
    board_handle = audio_board_get_handle();
    if (board_handle == NULL) {
        ESP_LOGE(TAG, "Board handle is NULL");
        return ESP_FAIL;
    }

    // Inicjalizacja peryferiów (przyciski, touch)
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    periph_set = esp_periph_set_init(&periph_cfg);

    // Przyciski dotykowe na LyraT (Play, Set, Vol+, Vol-)
    periph_touch_cfg_t touch_cfg = {
        .touch_mask = BIT(get_input_play_id()) |
                      BIT(get_input_set_id()) |
                      BIT(get_input_volup_id()) |
                      BIT(get_input_voldown_id()),
        .tap_threshold_percent = 70,
    };
    esp_periph_handle_t touch_periph = periph_touch_init(&touch_cfg);
    esp_periph_start(periph_set, touch_periph);

    // Przyciski GPIO na LyraT (Rec, Mode)
    periph_button_cfg_t btn_cfg = {
        .gpio_mask = (1ULL << get_input_rec_id()) | (1ULL << get_input_mode_id()),
    };
    esp_periph_handle_t button_periph = periph_button_init(&btn_cfg);
    esp_periph_start(periph_set, button_periph);

    // Konfiguracja HTTP stream
    // Note: HTTPS z pełnym certificate bundle wymaga zbyt dużo pamięci RAM
    // Dla strumieniów radiowych używamy HTTP lub HTTPS bez weryfikacji certyfikatu
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = true;
    http_cfg.task_stack = 8 * 1024;  // Increased stack for better network handling
    http_cfg.out_rb_size = 256 * 1024;  // 256KB buffer - ~16s at 128kbps (PSRAM)
    http_cfg.task_prio = 22;  // High priority for HTTP stream
    http_cfg.task_core = 0;  // Core 0 - together with WiFi for better network I/O
    // HTTPS: wyłącz weryfikację certyfikatów (oszczędza RAM)
    http_cfg.crt_bundle_attach = NULL;
    http_stream = http_stream_init(&http_cfg);

    // Konfiguracja dekodera MP3
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_cfg.task_stack = 8 * 1024;
    mp3_cfg.out_rb_size = 64 * 1024;  // 64KB output buffer (PSRAM)
    mp3_cfg.task_prio = 22;  // High priority for audio processing
    mp3_cfg.task_core = 0;   // Core 0 - isolated from WiFi/web on core 0
    decoder = mp3_decoder_init(&mp3_cfg);

    // Konfiguracja filtra resampling (44100 -> 48000)
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = 48000;
    rsp_cfg.src_ch = 2;
    rsp_cfg.dest_rate = 44100;
    rsp_cfg.dest_ch = 2;
    rsp_filter = rsp_filter_init(&rsp_cfg);

    // Inicjalizacja tablicy EQ gain z audio_settings
    audio_settings_t *settings = audio_settings_get();
    for (int i = 0; i < 10; i++) {
        // Konwersja z zakresu 0-24 (gdzie 12=0dB) na dB (-12 do +12)
        int db = (int)settings->bands[i] - 12;
        eq_gain[i] = db;       // Left channel
        eq_gain[i + 10] = db;  // Right channel
    }

    // Equalizer wyłączony - zużywa ~28% CPU
    // TODO: Zoptymalizować EQ lub użyć sprzętowego DSP
    equalizer = NULL;
    ESP_LOGI(TAG, "Equalizer disabled to save CPU");

    // Konfiguracja I2S stream (wyjście audio)
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.out_rb_size = 64 * 1024;  // 64KB ringbuffer (PSRAM)
    i2s_cfg.task_prio = 23;  // Maximum priority for realtime audio
    i2s_cfg.task_core = 1;  // Pin I2S to core 1 (isolated from WiFi on core 0)
    i2s_cfg.stack_in_ext = true;  // Use PSRAM for I2S task stack
    // Bufory DMA - bezpieczna konfiguracja
    i2s_cfg.chan_cfg.dma_desc_num = 8;     // 8 deskryptorów DMA
    i2s_cfg.chan_cfg.dma_frame_num = 1024; // 1024 ramki na deskryptor
    i2s_stream = i2s_stream_init(&i2s_cfg);

    // Tworzenie pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_cfg.rb_size = 128 * 1024;  // 128KB ringbuffer between elements (PSRAM)
    pipeline = audio_pipeline_init(&pipeline_cfg);
    if (pipeline == NULL) {
        ESP_LOGE(TAG, "Failed to create pipeline");
        return ESP_FAIL;
    }

    // Rejestracja elementów
    audio_pipeline_register(pipeline, http_stream, "http");
    audio_pipeline_register(pipeline, decoder, "mp3");
    audio_pipeline_register(pipeline, rsp_filter, "filter");
    if (equalizer) {
        audio_pipeline_register(pipeline, equalizer, "eq");
    }
    audio_pipeline_register(pipeline, i2s_stream, "i2s");

    // Łączenie elementów: http -> mp3 -> eq -> i2s (bez resamplera - oszczędność CPU)
    if (equalizer) {
        const char *link_tag[4] = {"http", "mp3", "eq", "i2s"};
        audio_pipeline_link(pipeline, &link_tag[0], 4);
        ESP_LOGI(TAG, "Pipeline: http -> mp3 -> eq -> i2s");
    } else {
        const char *link_tag[3] = {"http", "mp3", "i2s"};
        audio_pipeline_link(pipeline, &link_tag[0], 3);
        ESP_LOGI(TAG, "Pipeline: http -> mp3 -> i2s (no equalizer)");
    }

    // Konfiguracja event interface
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(pipeline, evt);
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(periph_set), evt);

    // Wczytaj zapisaną głośność z NVS
    player_status.volume = audio_settings_get_volume();
    ESP_LOGI(TAG, "Loaded saved volume: %d", player_status.volume);
    // Ustaw głośność na kodeku
    audio_hal_set_volume(board_handle->audio_hal, player_status.volume);

    // Uruchom task obsługi zdarzeń
    xTaskCreate(audio_event_task, "audio_event", 4096, NULL, 15, &event_task_handle);  // Increased priority

    ESP_LOGI(TAG, "Audio player initialized successfully");
    return ESP_OK;
}

esp_err_t audio_player_deinit(void)
{
    if (event_task_handle) {
        vTaskDelete(event_task_handle);
        event_task_handle = NULL;
    }

    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, http_stream);
    audio_pipeline_unregister(pipeline, decoder);
    audio_pipeline_unregister(pipeline, rsp_filter);
    audio_pipeline_unregister(pipeline, i2s_stream);

    audio_pipeline_remove_listener(pipeline);
    esp_periph_set_stop_all(periph_set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(periph_set), evt);
    audio_event_iface_destroy(evt);

    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_stream);
    audio_element_deinit(decoder);
    audio_element_deinit(rsp_filter);
    audio_element_deinit(i2s_stream);
    esp_periph_set_destroy(periph_set);

    return ESP_OK;
}

esp_err_t audio_player_play_url(const char *url)
{
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid URL (null or empty)");
        return ESP_ERR_INVALID_ARG;
    }

    if (pipeline == NULL) {
        ESP_LOGE(TAG, "Pipeline not initialized!");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Playing URL: %s", url);

    // Zatrzymaj obecne odtwarzanie
    ESP_LOGI(TAG, "Stopping current playback...");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);

    // Ustaw nowy URL
    ESP_LOGI(TAG, "Setting URL on http_stream...");
    audio_element_set_uri(http_stream, url);
    strncpy(player_status.current_url, url, sizeof(player_status.current_url) - 1);

    // Reset pipeline
    ESP_LOGI(TAG, "Resetting pipeline...");
    audio_pipeline_reset_ringbuffer(pipeline);
    audio_pipeline_reset_elements(pipeline);

    // Uruchom pipeline z pre-bufferingiem
    ESP_LOGI(TAG, "Starting pipeline with prebuffering...");

    esp_err_t ret = audio_pipeline_run(pipeline);
    
    // Pause I2S immediately after pipeline starts - buffer will fill
    if (ret == ESP_OK) {
        audio_element_pause(i2s_stream);
        ESP_LOGI(TAG, "I2S paused, filling buffer...");
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Pipeline started, monitoring buffer...");
        player_status.source = AUDIO_SOURCE_HTTP;
        set_state(PLAYER_STATE_BUFFERING);  // Start as buffering, timer will switch to playing

        // Zapisz URL dla autostartu
        audio_settings_set_last_url(url);

        // Create prebuffer timer if not exists
        if (prebuffer_timer == NULL) {
            prebuffer_timer = xTimerCreate("prebuf", pdMS_TO_TICKS(PREBUFFER_CHECK_MS),
                                           pdTRUE, NULL, prebuffer_timer_callback);
        }

        // Reset and start buffer monitoring
        prebuffer_counter = 0;
        current_buffer_percent = 0;
        xTimerStart(prebuffer_timer, 0);
    } else {
        ESP_LOGE(TAG, "Failed to start pipeline: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t audio_player_play_sdcard(const char *filepath)
{
    // TODO: Implementacja odtwarzania z karty SD
    ESP_LOGW(TAG, "SD card playback not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_player_stop(void)
{
    ESP_LOGI(TAG, "Stopping playback");

    esp_err_t ret = audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);

    if (ret == ESP_OK) {
        set_state(PLAYER_STATE_STOPPED);
    }

    return ret;
}

esp_err_t audio_player_pause(void)
{
    ESP_LOGI(TAG, "Pausing playback");

    esp_err_t ret = audio_pipeline_pause(pipeline);
    if (ret == ESP_OK) {
        set_state(PLAYER_STATE_PAUSED);
    }

    return ret;
}

esp_err_t audio_player_resume(void)
{
    ESP_LOGI(TAG, "Resuming playback");

    esp_err_t ret = audio_pipeline_resume(pipeline);
    if (ret == ESP_OK) {
        set_state(PLAYER_STATE_PLAYING);
    }

    return ret;
}

esp_err_t audio_player_play_next_station(void)
{
    uint8_t count = 0;
    radio_station_t *stations = radio_stations_get_all(&count);

    if (stations == NULL || count == 0) {
        ESP_LOGW(TAG, "No stations available");
        return ESP_ERR_NOT_FOUND;
    }

    // Find current station index
    int current_index = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(stations[i].url, player_status.current_url) == 0) {
            current_index = i;
            break;
        }
    }

    // Calculate next index (wrap around)
    int next_index = (current_index + 1) % count;

    ESP_LOGI(TAG, "Playing next station: %s", stations[next_index].name);
    return audio_player_play_url(stations[next_index].url);
}

// Timer do debounce aktualizacji głośności kodeka
static TimerHandle_t volume_timer = NULL;
static int pending_volume = -1;

static void volume_timer_callback(TimerHandle_t xTimer)
{
    if (pending_volume >= 0 && board_handle) {
        audio_hal_set_volume(board_handle->audio_hal, pending_volume);
        pending_volume = -1;
    }
}

esp_err_t audio_player_set_volume(int volume)
{
    // Ogranicz zakres
    if (volume < MIN_VOLUME) volume = MIN_VOLUME;
    if (volume > MAX_VOLUME) volume = MAX_VOLUME;

    player_status.volume = volume;

    // Zapisz głośność do NVS (już debounced w audio_settings)
    audio_settings_set_volume(volume);

    // Debounce aktualizacji kodeka - zapobiegaj mikro-przerwom przy przesuwaniu suwaka
    if (!player_status.muted && board_handle) {
        pending_volume = volume;

        // Utwórz timer jeśli nie istnieje
        if (volume_timer == NULL) {
            volume_timer = xTimerCreate("vol_timer", pdMS_TO_TICKS(50), pdFALSE, NULL, volume_timer_callback);
        }

        // Reset timer - aktualizacja kodeka nastąpi 50ms po ostatniej zmianie
        if (volume_timer) {
            xTimerReset(volume_timer, 0);
        }
    }

    notify_state_change();

    return ESP_OK;
}

int audio_player_get_volume(void)
{
    return player_status.volume;
}

esp_err_t audio_player_mute(bool mute)
{
    player_status.muted = mute;

    if (board_handle) {
        if (mute) {
            audio_hal_set_volume(board_handle->audio_hal, 0);
        } else {
            audio_hal_set_volume(board_handle->audio_hal, player_status.volume);
        }
    }

    ESP_LOGI(TAG, "Mute: %s", mute ? "ON" : "OFF");
    notify_state_change();

    return ESP_OK;
}

player_status_t *audio_player_get_status(void)
{
    return &player_status;
}

void audio_player_register_callback(player_state_callback_t callback)
{
    state_callback = callback;
}

// ============================================
// Equalizer Control
// ============================================

esp_err_t audio_player_set_eq_band(int band, int gain_db)
{
    if (equalizer == NULL) {
        ESP_LOGW(TAG, "Equalizer not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (band < 0 || band >= 10) {
        return ESP_ERR_INVALID_ARG;
    }
    if (gain_db < -13 || gain_db > 13) {
        gain_db = (gain_db < -13) ? -13 : 13;
    }

    // Update local array
    eq_gain[band] = gain_db;       // Left channel
    eq_gain[band + 10] = gain_db;  // Right channel

    // Apply to equalizer (true = same gain for both channels)
    esp_err_t ret = equalizer_set_gain_info(equalizer, band, gain_db, true);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "EQ band %d set to %+d dB", band, gain_db);
    }
    return ret;
}

esp_err_t audio_player_set_eq_all_bands(const int *gains_db)
{
    if (equalizer == NULL) {
        ESP_LOGW(TAG, "Equalizer not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < 10; i++) {
        int db = gains_db[i];
        if (db < -13) db = -13;
        if (db > 13) db = 13;

        eq_gain[i] = db;
        eq_gain[i + 10] = db;
        equalizer_set_gain_info(equalizer, i, db, true);
    }

    ESP_LOGI(TAG, "All EQ bands updated");
    return ESP_OK;
}

audio_element_handle_t audio_player_get_equalizer(void)
{
    return equalizer;
}
