/*
 * SD Card Player Module
 * Plays audio files (MP3, FLAC, WAV) from microSD card
 */

#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "sdcard_player.h"
#include "audio_player.h"
#include "config.h"

static const char *TAG = "SD_PLAYER";

// ============================================
// Constants
// ============================================
#define SD_MOUNT_POINT      "/sdcard"
#define MAX_PLAYLIST_SIZE   500
#define SUPPORTED_EXTENSIONS ".mp3.flac.wav.ogg.aac.m4a"

// ============================================
// State variables
// ============================================
static sd_player_status_t player_status = {0};
static sd_file_info_t *playlist = NULL;
static int playlist_count = 0;
static int playlist_capacity = 0;
static bool card_mounted = false;
static sdmmc_card_t *card = NULL;

// Callbacks
static sd_state_callback_t state_callback = NULL;
static sd_track_callback_t track_callback = NULL;

// ============================================
// Helper functions
// ============================================

static bool is_audio_file(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (ext == NULL) return false;

    char ext_lower[16];
    int i = 0;
    while (ext[i] && i < 15) {
        ext_lower[i] = tolower((unsigned char)ext[i]);
        i++;
    }
    ext_lower[i] = '\0';

    return strstr(SUPPORTED_EXTENSIONS, ext_lower) != NULL;
}

static void notify_state_change(void) {
    if (state_callback) {
        state_callback(&player_status);
    }
}

static void notify_track_change(void) {
    if (track_callback) {
        track_callback(&player_status.current_file);
    }
}

// ============================================
// SD Card mounting
// ============================================

static esp_err_t mount_sdcard(void) {
    if (card_mounted) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting SD card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Use SDMMC host (4-bit mode for LyraT)
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;  // 1-bit mode (DIP switch default)
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                             &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    card_mounted = true;
    ESP_LOGI(TAG, "SD card mounted: %s, %lluMB",
             card->cid.name, ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));

    return ESP_OK;
}

static esp_err_t unmount_sdcard(void) {
    if (!card_mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
    card_mounted = false;
    card = NULL;
    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

// ============================================
// Playlist management
// ============================================

static esp_err_t playlist_ensure_capacity(int needed) {
    if (playlist_capacity >= needed) {
        return ESP_OK;
    }

    int new_capacity = (needed + 50) > MAX_PLAYLIST_SIZE ? MAX_PLAYLIST_SIZE : (needed + 50);
    sd_file_info_t *new_playlist = realloc(playlist, new_capacity * sizeof(sd_file_info_t));
    if (new_playlist == NULL) {
        return ESP_ERR_NO_MEM;
    }

    playlist = new_playlist;
    playlist_capacity = new_capacity;
    return ESP_OK;
}

static void shuffle_playlist(void) {
    if (playlist_count < 2) return;

    for (int i = playlist_count - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        sd_file_info_t temp = playlist[i];
        playlist[i] = playlist[j];
        playlist[j] = temp;
    }
}

// ============================================
// Public API
// ============================================

esp_err_t sdcard_player_init(void) {
    ESP_LOGI(TAG, "Initializing SD card player...");

    memset(&player_status, 0, sizeof(player_status));
    player_status.state = SD_STATE_IDLE;
    player_status.play_mode = SD_PLAY_MODE_NORMAL;

    esp_err_t ret = mount_sdcard();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available");
        return ESP_OK;  // Not an error - card may be inserted later
    }

    ESP_LOGI(TAG, "SD card player initialized");
    return ESP_OK;
}

esp_err_t sdcard_player_deinit(void) {
    sdcard_player_stop();
    sdcard_player_clear_playlist();
    unmount_sdcard();
    return ESP_OK;
}

bool sdcard_player_is_card_inserted(void) {
    if (!card_mounted) {
        mount_sdcard();
    }
    return card_mounted;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
esp_err_t sdcard_player_scan_directory(const char *path, sd_file_info_t **files, int *count) {
    if (!card_mounted) {
        esp_err_t ret = mount_sdcard();
        if (ret != ESP_OK) return ret;
    }

    char full_path[300];
    if (path[0] != '/') {
        snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s%s", SD_MOUNT_POINT, path);
    }

    DIR *dir = opendir(full_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", full_path);
        return ESP_FAIL;
    }

    // Count entries first
    int entry_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;  // Skip hidden files
        if (entry->d_type == DT_DIR || is_audio_file(entry->d_name)) {
            entry_count++;
        }
    }

    // Allocate array
    *files = malloc(entry_count * sizeof(sd_file_info_t));
    if (*files == NULL) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    // Read entries
    rewinddir(dir);
    int idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < entry_count) {
        if (entry->d_name[0] == '.') continue;

        bool is_dir = (entry->d_type == DT_DIR);
        if (!is_dir && !is_audio_file(entry->d_name)) continue;

        sd_file_info_t *file = &(*files)[idx];
        memset(file, 0, sizeof(sd_file_info_t));

        strncpy(file->filename, entry->d_name, sizeof(file->filename) - 1);
        snprintf(file->filepath, sizeof(file->filepath), "%s/%s", path, entry->d_name);
        file->is_directory = is_dir;

        // Get file size for regular files
        if (!is_dir) {
            char stat_path[300];
            snprintf(stat_path, sizeof(stat_path), "%s/%s", full_path, entry->d_name);
            struct stat st;
            if (stat(stat_path, &st) == 0) {
                file->file_size = st.st_size;
            }

            // Extract title from filename (remove extension)
            strncpy(file->title, entry->d_name, sizeof(file->title) - 1);
            char *dot = strrchr(file->title, '.');
            if (dot) *dot = '\0';
        }

        idx++;
    }

    closedir(dir);
    *count = idx;

    ESP_LOGI(TAG, "Scanned directory %s: %d items", path, idx);
    return ESP_OK;
}
#pragma GCC diagnostic pop

esp_err_t sdcard_player_free_file_list(sd_file_info_t *files, int count) {
    if (files) {
        free(files);
    }
    return ESP_OK;
}

esp_err_t sdcard_player_get_card_info(uint64_t *total_bytes, uint64_t *free_bytes) {
    if (!card_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    FATFS *fs;
    DWORD fre_clust;
    if (f_getfree("0:", &fre_clust, &fs) != FR_OK) {
        return ESP_FAIL;
    }

    uint64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
    uint64_t free_sectors = fre_clust * fs->csize;

    *total_bytes = total_sectors * 512;
    *free_bytes = free_sectors * 512;

    return ESP_OK;
}

esp_err_t sdcard_player_play_file(const char *filepath) {
    if (!card_mounted) {
        esp_err_t ret = mount_sdcard();
        if (ret != ESP_OK) return ret;
    }

    char full_path[300];
    if (strncmp(filepath, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) == 0) {
        strncpy(full_path, filepath, sizeof(full_path) - 1);
    } else if (filepath[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", SD_MOUNT_POINT, filepath);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, filepath);
    }

    ESP_LOGI(TAG, "Playing file: %s", full_path);

    // Update current file info
    memset(&player_status.current_file, 0, sizeof(sd_file_info_t));
    strncpy(player_status.current_file.filepath, filepath,
            sizeof(player_status.current_file.filepath) - 1);

    // Extract filename
    const char *filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : filepath;
    strncpy(player_status.current_file.filename, filename,
            sizeof(player_status.current_file.filename) - 1);

    // Extract title
    strncpy(player_status.current_file.title, filename,
            sizeof(player_status.current_file.title) - 1);
    char *dot = strrchr(player_status.current_file.title, '.');
    if (dot) *dot = '\0';

    // Use audio_player to play
    esp_err_t ret = audio_player_play_sdcard(full_path);
    if (ret == ESP_OK) {
        player_status.state = SD_STATE_PLAYING;
        player_status.position_ms = 0;
        notify_state_change();
        notify_track_change();
    } else {
        player_status.state = SD_STATE_ERROR;
        notify_state_change();
    }

    return ret;
}

esp_err_t sdcard_player_play_directory(const char *dirpath) {
    sdcard_player_clear_playlist();

    sd_file_info_t *files;
    int count;
    esp_err_t ret = sdcard_player_scan_directory(dirpath, &files, &count);
    if (ret != ESP_OK) return ret;

    // Add audio files to playlist
    for (int i = 0; i < count; i++) {
        if (!files[i].is_directory) {
            sdcard_player_add_to_playlist(files[i].filepath);
        }
    }

    sdcard_player_free_file_list(files, count);

    if (playlist_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    // Shuffle if needed
    if (player_status.play_mode == SD_PLAY_MODE_SHUFFLE) {
        shuffle_playlist();
    }

    // Start playing first track
    player_status.playlist_index = 0;
    player_status.playlist_total = playlist_count;

    return sdcard_player_play_file(playlist[0].filepath);
}

esp_err_t sdcard_player_play_index(int index) {
    if (index < 0 || index >= playlist_count) {
        return ESP_ERR_INVALID_ARG;
    }

    player_status.playlist_index = index;
    return sdcard_player_play_file(playlist[index].filepath);
}

esp_err_t sdcard_player_stop(void) {
    audio_player_stop();
    player_status.state = SD_STATE_STOPPED;
    player_status.position_ms = 0;
    notify_state_change();
    return ESP_OK;
}

esp_err_t sdcard_player_pause(void) {
    if (player_status.state != SD_STATE_PLAYING) {
        return ESP_OK;
    }

    audio_player_pause();
    player_status.state = SD_STATE_PAUSED;
    notify_state_change();
    return ESP_OK;
}

esp_err_t sdcard_player_resume(void) {
    if (player_status.state != SD_STATE_PAUSED) {
        return ESP_OK;
    }

    audio_player_resume();
    player_status.state = SD_STATE_PLAYING;
    notify_state_change();
    return ESP_OK;
}

esp_err_t sdcard_player_next(void) {
    if (playlist_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int next_index = player_status.playlist_index + 1;

    if (next_index >= playlist_count) {
        if (player_status.play_mode == SD_PLAY_MODE_REPEAT_ALL) {
            next_index = 0;
            if (player_status.play_mode == SD_PLAY_MODE_SHUFFLE) {
                shuffle_playlist();
            }
        } else {
            // End of playlist
            sdcard_player_stop();
            return ESP_OK;
        }
    }

    return sdcard_player_play_index(next_index);
}

esp_err_t sdcard_player_prev(void) {
    if (playlist_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // If more than 3 seconds into track, restart current track
    if (player_status.position_ms > 3000) {
        return sdcard_player_play_index(player_status.playlist_index);
    }

    int prev_index = player_status.playlist_index - 1;
    if (prev_index < 0) {
        if (player_status.play_mode == SD_PLAY_MODE_REPEAT_ALL) {
            prev_index = playlist_count - 1;
        } else {
            prev_index = 0;
        }
    }

    return sdcard_player_play_index(prev_index);
}

esp_err_t sdcard_player_seek(uint32_t position_ms) {
    // Seeking would require ESP-ADF pipeline manipulation
    // For now, just update position for UI
    player_status.position_ms = position_ms;
    return ESP_OK;
}

esp_err_t sdcard_player_set_play_mode(sd_play_mode_t mode) {
    player_status.play_mode = mode;
    ESP_LOGI(TAG, "Play mode set to: %d", mode);

    if (mode == SD_PLAY_MODE_SHUFFLE && playlist_count > 1) {
        // Save current track
        sd_file_info_t current = playlist[player_status.playlist_index];
        shuffle_playlist();
        // Move current track to front
        for (int i = 0; i < playlist_count; i++) {
            if (strcmp(playlist[i].filepath, current.filepath) == 0) {
                sd_file_info_t temp = playlist[0];
                playlist[0] = playlist[i];
                playlist[i] = temp;
                break;
            }
        }
        player_status.playlist_index = 0;
    }

    return ESP_OK;
}

sd_play_mode_t sdcard_player_get_play_mode(void) {
    return player_status.play_mode;
}

esp_err_t sdcard_player_add_to_playlist(const char *filepath) {
    if (playlist_count >= MAX_PLAYLIST_SIZE) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = playlist_ensure_capacity(playlist_count + 1);
    if (ret != ESP_OK) return ret;

    sd_file_info_t *file = &playlist[playlist_count];
    memset(file, 0, sizeof(sd_file_info_t));

    strncpy(file->filepath, filepath, sizeof(file->filepath) - 1);

    const char *filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : filepath;
    strncpy(file->filename, filename, sizeof(file->filename) - 1);

    strncpy(file->title, filename, sizeof(file->title) - 1);
    char *dot = strrchr(file->title, '.');
    if (dot) *dot = '\0';

    playlist_count++;
    player_status.playlist_total = playlist_count;

    return ESP_OK;
}

esp_err_t sdcard_player_clear_playlist(void) {
    if (playlist) {
        free(playlist);
        playlist = NULL;
    }
    playlist_count = 0;
    playlist_capacity = 0;
    player_status.playlist_index = 0;
    player_status.playlist_total = 0;
    return ESP_OK;
}

int sdcard_player_get_playlist_count(void) {
    return playlist_count;
}

sd_player_status_t *sdcard_player_get_status(void) {
    return &player_status;
}

sd_player_state_t sdcard_player_get_state(void) {
    return player_status.state;
}

void sdcard_player_register_state_callback(sd_state_callback_t callback) {
    state_callback = callback;
}

void sdcard_player_register_track_callback(sd_track_callback_t callback) {
    track_callback = callback;
}
