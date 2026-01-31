/*
 * Piped Client Module
 * YouTube audio streaming via Piped API (privacy-friendly YouTube proxy)
 *
 * Piped API: https://docs.piped.video/docs/api-documentation/
 */

#ifndef PIPED_CLIENT_H
#define PIPED_CLIENT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// ============================================
// Configuration
// ============================================
#define PIPED_MAX_SEARCH_RESULTS    10
#define PIPED_MAX_TITLE_LEN         80
#define PIPED_MAX_ARTIST_LEN        40
#define PIPED_MAX_URL_LEN           512
#define PIPED_VIDEO_ID_LEN          12      // YouTube video ID is 11 chars + null

// ============================================
// Data structures
// ============================================

// Search result item
typedef struct {
    char video_id[PIPED_VIDEO_ID_LEN];
    char title[PIPED_MAX_TITLE_LEN];
    char artist[PIPED_MAX_ARTIST_LEN];      // Channel/uploader name
    uint32_t duration_seconds;
    uint32_t views;
    char thumbnail_url[PIPED_MAX_URL_LEN];
} piped_search_item_t;

// Search results
typedef struct {
    piped_search_item_t items[PIPED_MAX_SEARCH_RESULTS];
    uint8_t count;
    bool has_more;
    char next_page[256];
} piped_search_results_t;

// Audio stream info
typedef struct {
    char url[PIPED_MAX_URL_LEN];
    char mime_type[32];
    uint32_t bitrate;
    char quality[16];                       // e.g., "128kbps"
    char codec[16];                         // e.g., "mp4a.40.2"
} piped_audio_stream_t;

// Stream info (full response from /streams endpoint)
typedef struct {
    char video_id[PIPED_VIDEO_ID_LEN];
    char title[PIPED_MAX_TITLE_LEN];
    char artist[PIPED_MAX_ARTIST_LEN];
    uint32_t duration_seconds;
    char thumbnail_url[PIPED_MAX_URL_LEN];
    piped_audio_stream_t audio;             // Best audio stream
} piped_stream_info_t;

// Callback for async operations
typedef void (*piped_search_callback_t)(piped_search_results_t *results, esp_err_t err);
typedef void (*piped_stream_callback_t)(piped_stream_info_t *stream, esp_err_t err);

// ============================================
// Initialization
// ============================================

/**
 * Initialize Piped client
 * @param api_base_url Base URL of Piped API instance (e.g., "https://pipedapi.kavin.rocks")
 *                     If NULL, uses default instance
 */
esp_err_t piped_client_init(const char *api_base_url);

/**
 * Deinitialize and free resources
 */
esp_err_t piped_client_deinit(void);

/**
 * Set API instance URL (can be changed at runtime)
 */
esp_err_t piped_client_set_instance(const char *api_base_url);

/**
 * Get current API instance URL
 */
const char *piped_client_get_instance(void);

// ============================================
// Search
// ============================================

/**
 * Search for music/videos
 * @param query Search query string
 * @param filter Filter type: "music_songs", "videos", "music_videos", or NULL for all
 * @param results Pointer to results structure (will be filled)
 * @return ESP_OK on success
 */
esp_err_t piped_search(const char *query, const char *filter, piped_search_results_t *results);

/**
 * Get next page of search results
 */
esp_err_t piped_search_next_page(piped_search_results_t *results);

// ============================================
// Stream info
// ============================================

/**
 * Get stream info for a video
 * @param video_id YouTube video ID (11 characters)
 * @param stream Pointer to stream info structure (will be filled)
 * @return ESP_OK on success
 */
esp_err_t piped_get_stream(const char *video_id, piped_stream_info_t *stream);

/**
 * Get direct audio URL for playback
 * This is a convenience function that calls piped_get_stream and extracts the audio URL
 * @param video_id YouTube video ID
 * @param audio_url Buffer to store the audio URL
 * @param url_len Size of the buffer
 * @return ESP_OK on success
 */
esp_err_t piped_get_audio_url(const char *video_id, char *audio_url, size_t url_len);

// ============================================
// Playback helpers
// ============================================

/**
 * Search and play the first result
 * @param query Search query
 * @return ESP_OK if playback started
 */
esp_err_t piped_play_search(const char *query);

/**
 * Play by video ID
 * @param video_id YouTube video ID
 * @return ESP_OK if playback started
 */
esp_err_t piped_play_video(const char *video_id);

// ============================================
// Instance list
// ============================================

// Default Piped API instances (fallback list)
#define PIPED_INSTANCE_DEFAULT      "https://pipedapi.kavin.rocks"
#define PIPED_INSTANCE_BACKUP_1     "https://api.piped.yt"
#define PIPED_INSTANCE_BACKUP_2     "https://piped-api.garudalinux.org"

/**
 * Test if an instance is working
 * @param api_base_url Instance URL to test
 * @return ESP_OK if instance is responding
 */
esp_err_t piped_test_instance(const char *api_base_url);

/**
 * Find a working instance from the default list
 * @return ESP_OK if a working instance was found and set
 */
esp_err_t piped_find_working_instance(void);

#endif // PIPED_CLIENT_H
