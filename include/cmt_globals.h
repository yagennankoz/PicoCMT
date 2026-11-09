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

#define US_TO_PIO_TICKS(us) (us)

inline uint32_t pio_ticks_to_us(uint32_t pio_ticks) { return pio_ticks; }

// 録音PIO(cmt_rec.pio)が計測値に含めない時間 (High 1.5us / Low 2.0us) の補正値
#define REC_PIO_OVERHEAD_US 2

// RAWのパルスワードから実際のパルス幅(us)を求める
inline uint32_t raw_pulse_us(uint16_t raw_word) {
    return pio_ticks_to_us((uint32_t)(raw_word & 0x7FFF)) + REC_PIO_OVERHEAD_US;
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


extern int pio_rec_sm;
extern int pio_play_sm;

extern bool oled_need_refresh;
extern bool sd_card_inserted;
extern bool sd_card_mounted;
extern bool wifi_connected;
extern bool wifi_connect_failed;
// 接続に失敗した理由(エラー画面に出す)
enum WifiFailReason {
    WIFI_FAIL_NONE = 0,
    WIFI_FAIL_BAD_PASSWORD,  // APにパスワードを拒否された
    WIFI_FAIL_AP_NOT_FOUND,  // APが見つからない
    WIFI_FAIL_JOIN,          // 接続に失敗した(電波が弱い、APによってはパスワード違いもここ)
    WIFI_FAIL_NO_IP,         // つながったが、IPアドレスをもらえない(DHCP)
    WIFI_FAIL_SETTINGS,      // 保存した設定が読めない
};
extern WifiFailReason wifi_fail_reason;
extern int wifi_fail_link_status;  // 失敗したときのWi-Fiチップの状態(link)
extern bool wifi_connect_in_progress;
extern uint32_t wifi_connect_started_ms;
extern bool wifi_settings_saved;
extern bool wifi_user_enabled;
extern bool wifi_ui_blink_on;
extern bool wifi_save_result_success;
extern uint32_t wifi_save_result_until_ms;

extern char wifi_ssid[33];
extern char wifi_password[65];
extern char web_password[];
extern bool web_auth_enabled;
extern char web_password_edit[];
extern const char* settings_save_result_label;
extern int settings_save_return_cursor;
extern int wifi_config_field;
extern int wifi_config_pos;
extern int wifi_menu_cursor;
// 設定メニューの行(上からの並び)。WiFiの行はWi-Fi設定が無いときは選べない
enum SettingsRow {
    SET_ROW_REC_FMT = 0,
    SET_ROW_FILTER,
    SET_ROW_WIFI,
    SET_ROW_WIFI_CONFIG,
    SET_ROW_WEB_PASS,
    SET_ROW_USB,
    SET_ROW_COUNT
};
extern bool wifi_config_apply_requested;
extern bool wifi_connect_requested;
extern bool wifi_disconnect_requested;
extern char wifi_scan_ssids[][33];
extern int32_t wifi_scan_rssi[];
extern int wifi_scan_count;
extern int wifi_scan_cursor;
extern bool wifi_scan_requested;
extern bool wifi_scan_in_progress;

extern volatile uint32_t rec_last_sound_time_ms;
extern volatile uint32_t rec_start_time_ms;

extern RecFormat default_rec_fmt;
extern FileFilter file_filter;

// フィルタの名前(ALL、T88など)と、ファイル名がフィルタに合うか
const char* file_filter_name(int filter);
bool file_matches_filter(const char* name, int filter);

extern int wifi_opt_sel;
// WiFi Config画面で、保存したWi-Fi設定を消すか確かめている
extern bool wifi_clear_confirm;

// 変換メニュー:元のファイル、選んでいる形式、結果(0:まだ、1:成功、-1:失敗)、書いたファイル
extern char convert_src_path[128];
extern RecFormat convert_fmt;
extern int convert_result;
extern char convert_out_path[128];

// 電源を入れた後、PCとつながっているか(USBドライブにするかを聞くか)をまだ決めていない
extern bool usb_startup_pending;
// USBドライブの画面で、やめるか確かめている
extern bool usb_exit_confirm;

extern int cmt_selected_baud_rate;
extern int cas_selected_baud_rate;
