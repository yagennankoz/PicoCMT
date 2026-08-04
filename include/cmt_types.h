#pragma once

#include <Arduino.h>

#define MAX_FILES 64
#define MAX_NAME_LEN 128

typedef enum {
    STATE_IDLE,
    STATE_REC_MENU,
    STATE_REC_STANDBY,
    STATE_PLAY_STANDBY,
    STATE_RECORDING,
    STATE_PLAYING,
    STATE_WIFI_MENU,
    STATE_WIFI_CONFIG,
    STATE_WIFI_PIN_CONNECT,
    STATE_WIFI_SAVE_RESULT,
    STATE_WIFI_CONNECT_ERROR
} SystemState;
typedef enum { MODE_NEW, MODE_APPEND } RecMode;
typedef enum { REMOTE_OFF, REMOTE_ON } RemoteState;
enum {
    IDX_REC,
    IDX_PLAY,
    IDX_STOP,
    IDX_OK,
    IDX_CANCEL,
    IDX_UP,
    IDX_DOWN,
    IDX_LEFT,
    IDX_RIGHT
};

typedef enum {
    FMT_T88 = 0,
    FMT_CMT,
    FMT_CAS,
    //    FMT_T77,
    //    FMT_P6,
    FMT_MAX
} RecFormat;

typedef struct {
    char name[MAX_NAME_LEN];
    bool is_dir;
} FileItem;

typedef struct {
    bool current_stable_state;
    bool last_raw_state;
    uint32_t state_changed_ms;
    uint32_t pressed_duration_ms;
    uint32_t last_repeat_ms;
    bool is_long_pressed;
    bool ignore_until_release;
} ButtonTracker;

typedef struct {
    bool play_active;
    bool play_paused;
    bool playback_t88_mode;
    bool playback_t88_end_reached;
    bool playback_source_exhausted;
    uint16_t playback_tag_id;
    uint32_t playback_tag_ticks_remaining;
    bool playback_data_tag_active;
    uint32_t playback_data_bytes_remaining;
    uint32_t playback_data_extra_skip;
    uint16_t playback_data_actual_type;
    bool playback_data_bit_active;
    uint32_t file_position;
    uint32_t file_size;
    uint32_t queue_level;
    uint32_t queue_free_space;
    uint32_t queue_progress_age_ms;
    bool core1_play_session_initialized;
    uint32_t core1_play_swap_count;
    uint16_t core1_play_last_t88_data;
    bool core1_play_using_buffer_a;
    bool core1_pio_enabled;
    bool core1_dma_busy;
    uint8_t core1_gpio_level;
    char playback_log[24];
} PlaybackStatus;