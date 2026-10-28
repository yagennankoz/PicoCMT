#include "cmt_ui_input.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>

#include "cmt_core1.h"
#include "cmt_globals.h"
#include "cmt_hw_config.h"
#include "cmt_network.h"
#include "cmt_network_config.h"
#include "cmt_pins.h"
#include "cmt_storage_record.h"
#include "cmt_types.h"
#include "cmt_ui.h"
#include "pico/util/queue.h"

extern bool start_playback_file(const char* filename);
extern bool jump_playback_to_blank(int direction);
extern bool start_raw_recording();
extern void stop_playback_and_return_idle();
extern void update_system_run_permissions();
extern bool load_directory(const String& path);
extern bool load_wifi_settings_from_flash();
extern void complete_playback_and_stay_on_screen();

// パスワードに使える文字 (WPA のパスフレーズに使える印字可能な ASCII 0x20-0x7E すべて)
static const char WIFI_CHARSET[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
static_assert(sizeof(WIFI_CHARSET) - 1 == 0x7E - 0x20 + 1, "WIFI_CHARSET must cover 0x20-0x7E");

static uint32_t wifi_edit_last_repeat_ms[NUM_BUTTONS] = {0};
static bool wifi_menu_wait_stop_release = false;

// Webパスワード入力画面で編集中のパスワード
char web_password_edit[WEB_PASSWORD_MAX_LEN + 1] = {0};

static void build_selected_item_abs_path(char* out, size_t out_size,
                                         const char* item_name) {
    if (out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (item_name == NULL || item_name[0] == '\0') {
        return;
    }

    if (item_name[0] == '/') {
        snprintf(out, out_size, "%s", item_name);
        return;
    }

    if (current_path == "/") {
        snprintf(out, out_size, "/%s", item_name);
    } else {
        snprintf(out, out_size, "%s/%s", current_path.c_str(), item_name);
    }
}

static bool consume_wifi_edit_button(int idx) {
    uint32_t now = millis();

    if (btn_trigger_clicked[idx]) {
        btn_trigger_clicked[idx] = false;
        wifi_edit_last_repeat_ms[idx] = now;
        return true;
    }

    if (btn_trigger_repeat[idx]) {
        btn_trigger_repeat[idx] = false;
        wifi_edit_last_repeat_ms[idx] = now;
        return true;
    }

    if (!buttons[idx].current_stable_state ||
        buttons[idx].pressed_duration_ms == 0) {
        return false;
    }

    uint32_t held_ms = now - buttons[idx].pressed_duration_ms;
    if (held_ms < WIFI_INPUT_HOLD_START_MS) {
        return false;
    }

    if ((now - wifi_edit_last_repeat_ms[idx]) < REPEAT_MS) {
        return false;
    }

    wifi_edit_last_repeat_ms[idx] = now;
    return true;
}

static void get_active_wifi_buffer(char** buf, int* max_len) {
    *buf = wifi_password;
    *max_len = 64;
}

static char rotate_wifi_char(char current, int step) {
    int charset_len = (int)strlen(WIFI_CHARSET);
    int idx = 0;

    for (int i = 0; i < charset_len; i++) {
        if (WIFI_CHARSET[i] == current) {
            idx = i;
            break;
        }
    }

    idx += step;
    if (idx < 0) {
        idx = charset_len - 1;
    }
    if (idx >= charset_len) {
        idx = 0;
    }
    return WIFI_CHARSET[idx];
}

// 文字列の編集 (UP/DOWN: 文字を変える, LEFT/RIGHT: 桁を移動)。
// 途中に空き ('\0') ができると以降が切れてしまうので、文字のない位置より先へは進めない。
// LEFT で先頭より前に出ようとしたら true
static bool edit_text_buffer(char* buf, int max_len) {
    if (wifi_config_pos < 0) {
        wifi_config_pos = 0;
    }
    if (wifi_config_pos >= max_len) {
        wifi_config_pos = max_len - 1;
    }

    if (consume_wifi_edit_button(IDX_UP)) {
        buf[wifi_config_pos] = rotate_wifi_char(buf[wifi_config_pos], +1);
        oled_need_refresh = true;
    }
    if (consume_wifi_edit_button(IDX_DOWN)) {
        buf[wifi_config_pos] = rotate_wifi_char(buf[wifi_config_pos], -1);
        oled_need_refresh = true;
    }
    if (btn_trigger_clicked[IDX_RIGHT]) {
        btn_trigger_clicked[IDX_RIGHT] = false;
        if (wifi_config_pos < (max_len - 1) && buf[wifi_config_pos] != '\0') {
            wifi_config_pos++;
        }
        oled_need_refresh = true;
    }
    if (btn_trigger_clicked[IDX_LEFT]) {
        btn_trigger_clicked[IDX_LEFT] = false;
        oled_need_refresh = true;
        if (wifi_config_pos > 0) {
            wifi_config_pos--;
        } else {
            return true;
        }
    }
    return false;
}

static void build_default_record_filename() {
    char candidate[sizeof(target_filename)] = {0};
    const char* ext_list[] = {".T88", ".CMT", ".CAS", ".P6", ".T77", ".P6T"};
    static_assert(sizeof(ext_list) / sizeof(ext_list[0]) == FMT_MAX, "ext_list");

    if (default_rec_fmt >= FMT_MAX) {
        default_rec_fmt = FMT_T88;
    }
    const char* ext = ext_list[default_rec_fmt];

    for (int index = 1; index <= 999; index++) {
        if (current_path == "/") {
            snprintf(candidate, sizeof(candidate), "/NEWREC%03d%s", index, ext);
        } else {
            snprintf(candidate, sizeof(candidate), "%s/NEWREC%03d%s",
                     current_path.c_str(), index, ext);
        }

        if (!SD.exists(candidate)) {
            snprintf(target_filename, sizeof(target_filename), "%s", candidate);
            return;
        }
    }

    if (current_path == "/") {
        snprintf(target_filename, sizeof(target_filename), "/NEWREC999%s", ext);
    } else {
        snprintf(target_filename, sizeof(target_filename), "%s/NEWREC999%s",
                 current_path.c_str(), ext);
    }
}

void handle_file_browser_input() {
    if (current_state == STATE_PLAY_STANDBY || play_active) {
        return;
    }

    bool is_cmt_selected = false;
    bool is_cas_selected = false;
    bool is_p6_selected = false;
    if (total_items > 0 && !file_list[current_cursor].is_dir) {
        const char* ext = strrchr(file_list[current_cursor].name, '.');
        if (ext) {
            if (strcasecmp(ext, ".CMT") == 0) {
                is_cmt_selected = true;
            }
            if (strcasecmp(ext, ".CAS") == 0) {
                is_cas_selected = true;
            }
            if (strcasecmp(ext, ".P6") == 0) {
                is_p6_selected = true;
            }
        }
    }

    if ((is_cmt_selected || is_cas_selected || is_p6_selected) &&
        (btn_trigger_clicked[IDX_LEFT] || btn_trigger_clicked[IDX_RIGHT] ||
         btn_trigger_repeat[IDX_LEFT] || btn_trigger_repeat[IDX_RIGHT])) {
        btn_trigger_clicked[IDX_LEFT] = false;
        btn_trigger_clicked[IDX_RIGHT] = false;
        btn_trigger_repeat[IDX_LEFT] = false;
        btn_trigger_repeat[IDX_RIGHT] = false;

        // P6 と CMT は設定を共用
        if (is_cmt_selected || is_p6_selected) {
            cmt_selected_baud_rate =
                (cmt_selected_baud_rate == 1200) ? 600 : 1200;
        } else if (is_cas_selected) {
            cas_selected_baud_rate =
                (cas_selected_baud_rate == 1200) ? 2400 : 1200;
        }
        oled_need_refresh = true;
    }

    if (btn_trigger_repeat[IDX_STOP]) {
        btn_trigger_repeat[IDX_STOP] = false;
        wifi_menu_cursor = wifi_settings_saved ? 0 : 1;
        // 0:CLEAR, 1:OFF, 2:ON
        wifi_opt_sel = wifi_user_enabled ? 2 : 1;
        wifi_menu_wait_stop_release = true;
        current_state = STATE_WIFI_MENU;
        reset_and_ignore_pressed_buttons();
        oled_need_refresh = true;
        return;
    }

    if (btn_trigger_clicked[IDX_UP] || btn_trigger_repeat[IDX_UP]) {
        btn_trigger_clicked[IDX_UP] = false;
        btn_trigger_repeat[IDX_UP] = false;

        if (current_cursor > 0) {
            current_cursor--;
            if (current_cursor < scroll_offset) {
                scroll_offset--;
            }
            oled_need_refresh = true;
        }
    }

    if (btn_trigger_clicked[IDX_DOWN] || btn_trigger_repeat[IDX_DOWN]) {
        btn_trigger_clicked[IDX_DOWN] = false;
        btn_trigger_repeat[IDX_DOWN] = false;

        if (current_cursor < total_items - 1) {
            current_cursor++;
            if (current_cursor - scroll_offset >= 4) {
                scroll_offset++;
            }
            oled_need_refresh = true;
        }
    }

    if (btn_trigger_clicked[IDX_CANCEL] || btn_trigger_clicked[IDX_LEFT]) {
        btn_trigger_clicked[IDX_CANCEL] = false;
        btn_trigger_clicked[IDX_LEFT] = false;

        if (current_path != "/") {
            int last_slash = current_path.lastIndexOf('/');
            if (last_slash <= 0) {
                current_path = "/";
            } else {
                current_path.remove(last_slash);
            }

            current_cursor = 0;
            scroll_offset = 0;

            load_directory(current_path);
            oled_need_refresh = true;
        }
    }

    if (btn_trigger_clicked[IDX_PLAY] || btn_trigger_clicked[IDX_OK] ||
        btn_trigger_clicked[IDX_RIGHT]) {
        bool is_ok_requested =
            btn_trigger_clicked[IDX_OK] || btn_trigger_clicked[IDX_RIGHT];

        btn_trigger_clicked[IDX_PLAY] = false;
        btn_trigger_clicked[IDX_OK] = false;
        btn_trigger_clicked[IDX_RIGHT] = false;

        if (total_items > 0) {
            FileItem selected = file_list[current_cursor];

            if (selected.is_dir) {
                if (is_ok_requested) {
                    if (current_path == "/") {
                        current_path = "/" + String(selected.name);
                    } else {
                        current_path += "/";
                        current_path += String(selected.name);
                    }

                    current_cursor = 0;
                    scroll_offset = 0;

                    load_directory(current_path);
                    oled_need_refresh = true;
                }
            } else {
                char selected_path[MAX_NAME_LEN] = {0};
                build_selected_item_abs_path(
                    selected_path, sizeof(selected_path), selected.name);
                if (start_playback_file(selected_path)) {
                    current_state = STATE_PLAY_STANDBY;
                    reset_and_ignore_pressed_buttons();
                    update_system_run_permissions();
                    oled_need_refresh = true;
                }
            }
        }
    }

    if (btn_trigger_clicked[IDX_REC]) {
        btn_trigger_clicked[IDX_REC] = false;

        bool should_show_rec_menu = false;

        // ファイルが選択されている場合のみ、追記メニューを表示するか判断する
        if (total_items > 0 && !file_list[current_cursor].is_dir) {
            should_show_rec_menu = is_appendable_filename(file_list[current_cursor].name);
        }

        if (should_show_rec_menu) {
            // 追記可能なファイルなので、録音メニューを表示
            build_selected_item_abs_path(target_filename,
                                         sizeof(target_filename),
                                         file_list[current_cursor].name);
            rec_menu_cursor = 0;
            current_state = STATE_REC_MENU;
            oled_need_refresh = true;
        } else {
            // 新規録音しかできないので、メニューをスキップして即開始
            build_default_record_filename();
            selected_rec_mode = MODE_NEW;
            if (start_raw_recording()) {
                current_state = STATE_REC_STANDBY;
                rec_paused = true;
                manual_ok_override = false;
                reset_and_ignore_pressed_buttons();
                oled_need_refresh = true;
            } else {
                current_state = STATE_IDLE;
                oled_need_refresh = true;
            }
        }
        return;
    }
}

void handle_rec_menu_input() {
    int menu_max_cursor = 1;

    if (btn_trigger_clicked[IDX_UP] || btn_trigger_clicked[IDX_LEFT]) {
        btn_trigger_clicked[IDX_UP] = false;
        btn_trigger_clicked[IDX_LEFT] = false;
        if (rec_menu_cursor > 0) {
            rec_menu_cursor--;
            oled_need_refresh = true;
        }
    }

    if (btn_trigger_clicked[IDX_DOWN] || btn_trigger_clicked[IDX_RIGHT]) {
        btn_trigger_clicked[IDX_DOWN] = false;
        btn_trigger_clicked[IDX_RIGHT] = false;
        if (rec_menu_cursor < menu_max_cursor) {
            rec_menu_cursor++;
            oled_need_refresh = true;
        }
    }

    if (btn_trigger_clicked[IDX_CANCEL] || btn_trigger_clicked[IDX_STOP]) {
        btn_trigger_clicked[IDX_CANCEL] = false;
        btn_trigger_clicked[IDX_STOP] = false;
        current_state = STATE_IDLE;
        oled_need_refresh = true;
    }

    if (btn_trigger_clicked[IDX_OK] || btn_trigger_clicked[IDX_RIGHT]) {
        btn_trigger_clicked[IDX_OK] = false;
        btn_trigger_clicked[IDX_RIGHT] = false;

        selected_rec_mode = (rec_menu_cursor == 0) ? MODE_NEW : MODE_APPEND;

        if (selected_rec_mode == MODE_NEW) {
            build_default_record_filename();
        }

        if (start_raw_recording()) {
            current_state = STATE_REC_STANDBY;
            rec_paused = true;
            manual_ok_override = false;
            reset_and_ignore_pressed_buttons();
            oled_need_refresh = true;
        } else {
            current_state = STATE_IDLE;
            oled_need_refresh = true;
        }
    }
}

void handle_wifi_menu_input() {
    if (wifi_menu_wait_stop_release) {
        btn_trigger_clicked[IDX_STOP] = false;
        btn_trigger_repeat[IDX_STOP] = false;
        if (buttons[IDX_STOP].current_stable_state) {
            return;
        }
        wifi_menu_wait_stop_release = false;
    }

    int min_cursor = wifi_settings_saved ? 0 : 1;
    int max_cursor = 3;

    if (wifi_menu_cursor < min_cursor) {
        wifi_menu_cursor = min_cursor;
    }
    if (wifi_menu_cursor > max_cursor) {
        wifi_menu_cursor = max_cursor;
    }

    if (btn_trigger_clicked[IDX_STOP] || btn_trigger_clicked[IDX_CANCEL]) {
        btn_trigger_clicked[IDX_STOP] = false;
        btn_trigger_clicked[IDX_CANCEL] = false;
        current_state = STATE_IDLE;
        reset_and_ignore_pressed_buttons();
        oled_need_refresh = true;
        return;
    }

    if (btn_trigger_clicked[IDX_UP]) {
        btn_trigger_clicked[IDX_UP] = false;
        if (wifi_menu_cursor > min_cursor) {
            wifi_menu_cursor--;
            oled_need_refresh = true;
        }
    }

    if (btn_trigger_clicked[IDX_DOWN]) {
        btn_trigger_clicked[IDX_DOWN] = false;
        if (wifi_menu_cursor < max_cursor) {
            wifi_menu_cursor++;
            oled_need_refresh = true;
        }
    }

    if (wifi_menu_cursor == 0) {
        if (btn_trigger_clicked[IDX_LEFT] || btn_trigger_repeat[IDX_LEFT]) {
            btn_trigger_clicked[IDX_LEFT] = false;
            btn_trigger_repeat[IDX_LEFT] = false;
            if (wifi_opt_sel > 0) {
                wifi_opt_sel--;
            }
            oled_need_refresh = true;
        }
        if (btn_trigger_clicked[IDX_RIGHT] || btn_trigger_repeat[IDX_RIGHT]) {
            btn_trigger_clicked[IDX_RIGHT] = false;
            btn_trigger_repeat[IDX_RIGHT] = false;
            if (wifi_opt_sel < 2) {
                wifi_opt_sel++;
            }
            oled_need_refresh = true;
        }
    } else if (wifi_menu_cursor == 2) {
        // Web の認証の ON/OFF (その場で保存する)
        if (btn_trigger_clicked[IDX_LEFT] || btn_trigger_clicked[IDX_RIGHT]) {
            bool enable = btn_trigger_clicked[IDX_RIGHT];
            btn_trigger_clicked[IDX_LEFT] = false;
            btn_trigger_clicked[IDX_RIGHT] = false;
            if (enable != web_auth_enabled) {
                save_web_auth_enabled(enable);
            }
            oled_need_refresh = true;
        }
        btn_trigger_repeat[IDX_LEFT] = false;
        btn_trigger_repeat[IDX_RIGHT] = false;
    } else if (wifi_menu_cursor == 3) {
        if (btn_trigger_clicked[IDX_LEFT] || btn_trigger_repeat[IDX_LEFT]) {
            btn_trigger_clicked[IDX_LEFT] = false;
            btn_trigger_repeat[IDX_LEFT] = false;
            if (default_rec_fmt > 0) {
                default_rec_fmt = (RecFormat)(default_rec_fmt - 1);
            } else {
                default_rec_fmt = (RecFormat)(FMT_MAX - 1);  // 最後へループ
            }
            save_rec_format_to_eeprom();
            oled_need_refresh = true;
        }
        if (btn_trigger_clicked[IDX_RIGHT] || btn_trigger_repeat[IDX_RIGHT]) {
            btn_trigger_clicked[IDX_RIGHT] = false;
            btn_trigger_repeat[IDX_RIGHT] = false;
            default_rec_fmt = (RecFormat)((default_rec_fmt + 1) % FMT_MAX);
            save_rec_format_to_eeprom();
            oled_need_refresh = true;
        }
    }

    if (wifi_menu_cursor == 1) {
        if (!btn_trigger_clicked[IDX_OK] && !btn_trigger_repeat[IDX_RIGHT]) {
            return;
        }
        btn_trigger_clicked[IDX_OK] = false;
        btn_trigger_repeat[IDX_RIGHT] = false;
    } else {
        if (!btn_trigger_clicked[IDX_OK]) {
            return;
        }
        btn_trigger_clicked[IDX_OK] = false;
    }

    if (wifi_menu_cursor == 0) {
        if (wifi_opt_sel == 0) {
            memset(wifi_password, 0, sizeof(wifi_password));
            memset(wifi_ssid, 0, sizeof(wifi_ssid));

            clear_wifi_settings_in_eeprom();

            wifi_settings_saved = false;
            wifi_user_enabled = false;
            wifi_disconnect_requested = true;
            wifi_menu_cursor = 1;
            wifi_opt_sel = 1;
            oled_need_refresh = true;
            return;
        } else if (wifi_opt_sel == 1) {
            if (wifi_user_enabled) {
                wifi_user_enabled = false;
                wifi_connect_requested = false;
                wifi_disconnect_requested = true;
            }
            save_wifi_auto_connect(false);
            oled_need_refresh = true;
            return;
        } else if (wifi_opt_sel == 2) {
            if (!wifi_user_enabled) {
                wifi_user_enabled = true;
                wifi_connect_requested = true;
                oled_need_refresh = true;
            }
            save_wifi_auto_connect(true);
            return;
        }
    }

    if (wifi_menu_cursor == 1) {
        // 保存されている設定を読み直して編集する (未保存なら空から)
        if (!wifi_settings_saved || !load_wifi_settings_from_flash()) {
            memset(wifi_ssid, 0, sizeof(wifi_ssid));
            memset(wifi_password, 0, sizeof(wifi_password));
        }
        wifi_config_field = 0;
        wifi_config_pos = 0;
        wifi_scan_requested = true;
        current_state = STATE_WIFI_CONFIG;
        oled_need_refresh = true;
        return;
    }

    if (wifi_menu_cursor == 2) {
        memset(web_password_edit, 0, sizeof(web_password_edit));
        strncpy(web_password_edit, web_password, WEB_PASSWORD_MAX_LEN);
        wifi_config_pos = 0;
        current_state = STATE_WEB_PASS_CONFIG;
        reset_and_ignore_pressed_buttons();
        oled_need_refresh = true;
    }
}

void handle_web_pass_config_input() {
    if (btn_trigger_clicked[IDX_STOP] || btn_trigger_clicked[IDX_CANCEL]) {
        current_state = STATE_WIFI_MENU;
        reset_and_ignore_pressed_buttons();
        oled_need_refresh = true;
        return;
    }

    if (btn_trigger_clicked[IDX_OK] || btn_trigger_clicked[IDX_REC]) {
        web_password_edit[WEB_PASSWORD_MAX_LEN] = '\0';
        // この入力方式では文字を消せないので、末尾の空白は消した扱いにする
        // (すべて空白なら空になり、初期値に戻る)
        for (int i = (int)strlen(web_password_edit) - 1;
             i >= 0 && web_password_edit[i] == ' '; i--) {
            web_password_edit[i] = '\0';
        }
        wifi_save_result_success = save_web_password(web_password_edit);
        settings_save_result_label = "Web password";
        settings_save_return_cursor = 2;
        wifi_save_result_until_ms = millis() + WIFI_SAVE_STATUS_DISPLAY_MS;
        current_state = STATE_WIFI_SAVE_RESULT;
        reset_and_ignore_pressed_buttons();
        oled_need_refresh = true;
        return;
    }

    edit_text_buffer(web_password_edit, WEB_PASSWORD_MAX_LEN);
}

void handle_wifi_config_input() {
    if (btn_trigger_clicked[IDX_STOP]) {
        btn_trigger_clicked[IDX_STOP] = false;
        btn_trigger_clicked[IDX_OK] = false;
        btn_trigger_clicked[IDX_RIGHT] = false;
        btn_trigger_repeat[IDX_RIGHT] = false;
        btn_trigger_clicked[IDX_REC] = false;
        btn_trigger_repeat[IDX_REC] = false;
        current_state = STATE_WIFI_MENU;
        reset_and_ignore_pressed_buttons();
        oled_need_refresh = true;
        return;
    }

    if (btn_trigger_clicked[IDX_REC]) {
        btn_trigger_clicked[IDX_REC] = false;
        btn_trigger_repeat[IDX_REC] = false;

        if (wifi_ssid[0] != '\0') {
            wifi_ssid[32] = '\0';
            wifi_password[64] = '\0';
            wifi_config_apply_requested = true;
            oled_need_refresh = true;
            return;
        }
    }

    if (wifi_config_field == 0) {
        if (btn_trigger_clicked[IDX_STOP] || btn_trigger_clicked[IDX_CANCEL]) {
            btn_trigger_clicked[IDX_STOP] = false;
            btn_trigger_clicked[IDX_CANCEL] = false;
            btn_trigger_clicked[IDX_OK] = false;
            btn_trigger_clicked[IDX_RIGHT] = false;
            btn_trigger_repeat[IDX_RIGHT] = false;
            btn_trigger_clicked[IDX_REC] = false;
            btn_trigger_repeat[IDX_REC] = false;
            current_state = STATE_WIFI_MENU;
            reset_and_ignore_pressed_buttons();
            oled_need_refresh = true;
            return;
        }

        if (btn_trigger_clicked[IDX_UP] || btn_trigger_repeat[IDX_UP]) {
            btn_trigger_clicked[IDX_UP] = false;
            btn_trigger_repeat[IDX_UP] = false;
            if (wifi_scan_cursor > 0) {
                wifi_scan_cursor--;
                oled_need_refresh = true;
            }
        }

        if (btn_trigger_clicked[IDX_DOWN] || btn_trigger_repeat[IDX_DOWN]) {
            btn_trigger_clicked[IDX_DOWN] = false;
            btn_trigger_repeat[IDX_DOWN] = false;
            if (wifi_scan_cursor < (wifi_scan_count - 1)) {
                wifi_scan_cursor++;
                oled_need_refresh = true;
            }
        }

        if (btn_trigger_clicked[IDX_OK]) {
            btn_trigger_clicked[IDX_OK] = false;
            if (wifi_scan_count > 0) {
                // パスワードは空から入力する (この入力方式では文字を消せないため)
                memset(wifi_password, 0, sizeof(wifi_password));
                snprintf(wifi_ssid, 33, "%s",
                         wifi_scan_ssids[wifi_scan_cursor]);
                wifi_config_field = 1;
                wifi_config_pos = 0;
                oled_need_refresh = true;
            }
        }

        return;
    }

    char* active_buf = NULL;
    int max_len = 0;
    get_active_wifi_buffer(&active_buf, &max_len);

    if (btn_trigger_clicked[IDX_CANCEL]) {
        btn_trigger_clicked[IDX_CANCEL] = false;
        wifi_config_field = 0;
        wifi_config_pos = 0;
        oled_need_refresh = true;
        return;
    }

    // パスワード欄で OK を押したら保存する
    if (btn_trigger_clicked[IDX_OK]) {
        btn_trigger_clicked[IDX_OK] = false;
        wifi_ssid[32] = '\0';
        wifi_password[64] = '\0';
        wifi_config_apply_requested = true;
        oled_need_refresh = true;
        return;
    }

    if (edit_text_buffer(active_buf, max_len)) {
        wifi_config_field = 0;
    }
}

void handle_wifi_connect_error_input() {
    if (!btn_trigger_clicked[IDX_OK]) {
        return;
    }

    reset_and_ignore_pressed_buttons();

    wifi_connect_failed = false;
    current_state = STATE_WIFI_MENU;
    oled_need_refresh = true;
}

static void execute_safe_playback_seek(bool seek_prev) {
    char temp_filename[MAX_NAME_LEN];
    snprintf(temp_filename, sizeof(temp_filename), "%s", target_filename);
    int direction = seek_prev ? -1 : 1;

    SystemState previous_play_state = current_state;

    if (jump_playback_to_blank(direction)) {
        if (previous_play_state == STATE_PLAY_STANDBY) {
            play_active = true;
            play_paused = true;
            playback_permitted = false;
            request_playback_shutdown();
            current_state = STATE_PLAY_STANDBY;
        } else {
            current_state = STATE_PLAYING;
        }
    } else {
        play_active = false;
        playback_permitted = false;
        request_playback_shutdown();
        delay(20);

        uint16_t dummy;
        while (queue_try_remove(&cmt_play_queue, &dummy));
        reset_core1_playback_buffers();

        if (cmt_file) {
            cmt_file.close();
        }

        if (direction > 0) {
            manual_ok_override = false;
            playback_user_paused = false;
            current_state = STATE_PLAY_STANDBY;
        } else {
            if (start_playback_file(temp_filename)) {
                if (previous_play_state == STATE_PLAY_STANDBY) {
                    play_active = true;
                    play_paused = true;
                    playback_permitted = false;
                    request_playback_shutdown();
                    current_state = STATE_PLAY_STANDBY;
                } else {
                    current_state = STATE_PLAYING;
                }
            } else {
                current_state = STATE_IDLE;
            }
        }
    }
    oled_need_refresh = true;
}

void handle_ui_buttons_in_standby() {
    if (current_state == STATE_PLAY_STANDBY && play_active) {
        if (btn_trigger_clicked[IDX_LEFT] || btn_trigger_clicked[IDX_RIGHT]) {
            bool seek_prev = btn_trigger_clicked[IDX_LEFT];
            btn_trigger_clicked[IDX_LEFT] = false;
            btn_trigger_clicked[IDX_RIGHT] = false;

            execute_safe_playback_seek(seek_prev);
            return;
        }
    }

    if (btn_trigger_clicked[IDX_OK] || btn_trigger_clicked[IDX_PLAY] ||
        btn_trigger_clicked[IDX_REC]) {
        btn_trigger_clicked[IDX_OK] = false;
        btn_trigger_clicked[IDX_PLAY] = false;
        btn_trigger_clicked[IDX_REC] = false;

        if (current_state == STATE_PLAY_STANDBY) {
            if (playback_user_paused) {
                playback_user_paused = false;
                manual_ok_override = true;
                update_system_run_permissions();
                oled_need_refresh = true;
                return;
            }
            if (!play_active) {
                stop_playback_and_return_idle();
                reset_and_ignore_pressed_buttons();
                manual_ok_override = false;
                oled_need_refresh = true;
                return;
            }
            if (play_paused) {
                manual_ok_override = true;
                update_system_run_permissions();
                oled_need_refresh = true;
                return;
            }
        }
        if (current_state == STATE_REC_STANDBY || rec_paused) {
            manual_ok_override = true;
            update_system_run_permissions();
            oled_need_refresh = true;
        }
    }

    if (btn_trigger_clicked[IDX_CANCEL] || btn_trigger_clicked[IDX_STOP]) {
        btn_trigger_clicked[IDX_CANCEL] = false;
        btn_trigger_clicked[IDX_STOP] = false;

        if (current_state == STATE_PLAY_STANDBY) {
            stop_playback_and_return_idle();
            reset_and_ignore_pressed_buttons();
            manual_ok_override = false;
            playback_user_paused = false;
            oled_need_refresh = true;
            return;
        }
        if (current_state == STATE_REC_STANDBY) {
            reset_and_ignore_pressed_buttons();
            rec_paused = true;
            manual_ok_override = false;
            current_state = STATE_IDLE;
            oled_need_refresh = true;
            return;
        }
        if (manual_ok_override) {
            manual_ok_override = false;
            update_system_run_permissions();
            oled_need_refresh = true;
        }
    }
}

void handle_playback_active_inputs() {
    if (current_state != STATE_PLAYING) {
        return;
    }

    if (btn_trigger_clicked[IDX_LEFT] || btn_trigger_clicked[IDX_RIGHT]) {
        bool seek_prev = btn_trigger_clicked[IDX_LEFT];
        btn_trigger_clicked[IDX_LEFT] = false;
        btn_trigger_clicked[IDX_RIGHT] = false;

        execute_safe_playback_seek(seek_prev);
    }
}