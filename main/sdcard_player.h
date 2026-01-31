/*
 * SD Card Player Module
 * Plays audio files (MP3, FLAC, WAV) from microSD card
 */

#ifndef SDCARD_PLAYER_H
#define SDCARD_PLAYER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// ============================================
// Playback modes
// ============================================
typedef enum {
    SD_PLAY_MODE_NORMAL = 0,    // Play once, stop at end
    SD_PLAY_MODE_REPEAT_ONE,    // Repeat current track
    SD_PLAY_MODE_REPEAT_ALL,    // Repeat entire playlist
    SD_PLAY_MODE_SHUFFLE,       // Shuffle playlist
} sd_play_mode_t;

// ============================================
// File info structure
// ============================================
typedef struct {
    char filename[128];
    char filepath[256];
    char title[128];
    char artist[128];
    char album[128];
    uint32_t duration_ms;
    uint32_t file_size;
    bool is_directory;
} sd_file_info_t;

// ============================================
// Player state
// ============================================
typedef enum {
    SD_STATE_IDLE = 0,
    SD_STATE_PLAYING,
    SD_STATE_PAUSED,
    SD_STATE_STOPPED,
    SD_STATE_ERROR,
} sd_player_state_t;

// ============================================
// Current track info
// ============================================
typedef struct {
    sd_player_state_t state;
    sd_file_info_t current_file;
    uint32_t position_ms;
    int playlist_index;
    int playlist_total;
    sd_play_mode_t play_mode;
} sd_player_status_t;

// ============================================
// Callbacks
// ============================================
typedef void (*sd_state_callback_t)(sd_player_status_t *status);
typedef void (*sd_track_callback_t)(sd_file_info_t *track);

// ============================================
// Initialization
// ============================================
esp_err_t sdcard_player_init(void);
esp_err_t sdcard_player_deinit(void);
bool sdcard_player_is_card_inserted(void);

// ============================================
// File system operations
// ============================================
esp_err_t sdcard_player_scan_directory(const char *path, sd_file_info_t **files, int *count);
esp_err_t sdcard_player_free_file_list(sd_file_info_t *files, int count);
esp_err_t sdcard_player_get_card_info(uint64_t *total_bytes, uint64_t *free_bytes);

// ============================================
// Playback control
// ============================================
esp_err_t sdcard_player_play_file(const char *filepath);
esp_err_t sdcard_player_play_directory(const char *dirpath);
esp_err_t sdcard_player_play_index(int index);  // Play from playlist
esp_err_t sdcard_player_stop(void);
esp_err_t sdcard_player_pause(void);
esp_err_t sdcard_player_resume(void);
esp_err_t sdcard_player_next(void);
esp_err_t sdcard_player_prev(void);
esp_err_t sdcard_player_seek(uint32_t position_ms);

// ============================================
// Playlist management
// ============================================
esp_err_t sdcard_player_set_play_mode(sd_play_mode_t mode);
sd_play_mode_t sdcard_player_get_play_mode(void);
esp_err_t sdcard_player_add_to_playlist(const char *filepath);
esp_err_t sdcard_player_clear_playlist(void);
int sdcard_player_get_playlist_count(void);

// ============================================
// Status
// ============================================
sd_player_status_t *sdcard_player_get_status(void);
sd_player_state_t sdcard_player_get_state(void);

// ============================================
// Callbacks
// ============================================
void sdcard_player_register_state_callback(sd_state_callback_t callback);
void sdcard_player_register_track_callback(sd_track_callback_t callback);

#endif // SDCARD_PLAYER_H
