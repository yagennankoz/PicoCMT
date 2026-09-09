#pragma once

#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <SD.h>

#include "cmt_hw_config.h"
#include "cmt_pins.h"
#include "cmt_types.h"
#include "pico/util/queue.h"

// ============================================================================
// 基準周波数および Ticks 自動計算マクロ定義
// ============================================================================

#ifndef PIO_CLOCK_FREQ_HZ
#define PIO_CLOCK_FREQ_HZ 1000000UL
#endif

#ifndef T88_CLOCK_FREQ_HZ
#define T88_CLOCK_FREQ_HZ 4800UL
#endif

#define US_TO_PIO_TICKS(us) (us)
#define MS_TO_PIO_TICKS(ms) ((ms) * 1000UL)

inline uint32_t pio_ticks_to_us(uint32_t pio_ticks) { return pio_ticks; }

inline uint32_t pio_ticks_to_t88_ticks(uint32_t pio_ticks) {
    return (uint32_t)(((uint64_t)pio_ticks * T88_CLOCK_FREQ_HZ + 500000ULL) /
                      1000000ULL);
}

// --- 録音時の判定閾値 (マイクロ秒単位) ---
#define REC_VALID_PULSE_MIN_TICKS 80
#define REC_VALID_PULSE_MAX_TICKS 1500
#define REC_SILENCE_GAP_TICKS 10000

// ============================================================================
// 定数・パス定義
// ============================================================================
static char raw_rec_path[64] = "/RAW_TEMP.BIN";
static const char T88_SIGNATURE[24] = "PC-8801 Tape Image(T88)";
static const uint16_t T88_TAG_END = 0x0000;
static const uint16_t T88_TAG_VERSION = 0x0001;
static const uint16_t T88_TAG_BLANK = 0x0100;
static const uint16_t T88_TAG_DATA = 0x0101;
static const uint16_t T88_TAG_SPACE = 0x0102;
static const uint16_t T88_TAG_MARK = 0x0103;
static const uint16_t T88_VERSION_1_0 = 0x0100;

extern ButtonTracker buttons[NUM_BUTTONS];
extern volatile bool btn_trigger_clicked[NUM_BUTTONS];
extern volatile bool btn_trigger_repeat[NUM_BUTTONS];

extern Adafruit_SSD1306 oled;

extern int rec_menu_cursor;
extern char target_filename[128];
extern String current_path;
extern int current_cursor;
extern int scroll_offset;
extern int total_items;
extern FileItem file_list[MAX_FILES];

extern volatile SystemState current_state;
extern RecMode selected_rec_mode;
extern RemoteState current_remote_state;
extern uint32_t remote_stable_time;

extern volatile bool playback_permitted;
extern volatile bool recording_permitted;
extern bool manual_ok_override;
extern volatile bool playback_user_paused;

extern volatile bool recording_active;
extern volatile bool play_active;
extern volatile bool rec_paused;
extern volatile bool play_paused;
extern volatile bool rec_triggered;
extern File cmt_file;

extern volatile bool playback_started;

extern volatile uint32_t playback_base_time_ms;
extern volatile uint32_t playback_tag_start_system_ms;
extern volatile bool playback_time_valid;

extern queue_t cmt_data_queue;
extern queue_t cmt_play_queue;

extern int buffer_idx;

extern int pio_rec_sm;
extern int pio_play_sm;

extern bool oled_need_refresh;
extern bool sd_card_inserted;
extern bool sd_card_mounted;
extern bool wifi_connected;
extern bool ftp_server_running;
extern bool wifi_connect_failed;
extern bool wifi_connect_in_progress;
extern uint32_t wifi_connect_started_ms;
extern bool wifi_settings_saved;
extern bool wifi_user_enabled;
extern bool wifi_ui_blink_on;
extern bool wifi_save_result_success;
extern bool wifi_settings_pin_requested;
extern uint32_t wifi_save_result_until_ms;

extern char wifi_ssid[33];
extern char wifi_password[65];
extern char wifi_pin[5];
extern uint8_t wifi_session_key[32];
extern bool wifi_session_key_valid;
extern int wifi_config_field;
extern int wifi_config_pos;
extern int wifi_menu_cursor;
extern bool wifi_config_apply_requested;
extern bool wifi_connect_with_pin_requested;
extern bool wifi_disconnect_requested;
extern char wifi_scan_ssids[][33];
extern int32_t wifi_scan_rssi[];
extern int wifi_scan_count;
extern int wifi_scan_cursor;
extern bool wifi_scan_requested;
extern bool wifi_scan_in_progress;
extern int wifi_pin_failed_attempts;

extern volatile uint32_t rec_last_sound_time_ms;
extern volatile uint32_t rec_start_time_ms;

extern RecFormat default_rec_fmt;

extern int wifi_opt_sel;

extern int cmt_selected_baud_rate;
extern int cas_selected_baud_rate;
