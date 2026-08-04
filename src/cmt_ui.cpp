#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <WiFi.h>

#include "cmt_globals.h"
#include "cmt_hw_config.h"
#include "cmt_network_config.h"
#include "cmt_pins.h"
#include "cmt_storage.h"
#include "cmt_types.h"
#include "hardware/watchdog.h"

static const uint button_pins[NUM_BUTTONS] = {
    BTN_REC_PIN, BTN_PLAY_PIN, BTN_STOP_PIN, BTN_OK_PIN,   BTN_CANCEL_PIN,
    BTN_UP_PIN,  BTN_DOWN_PIN, BTN_LEFT_PIN, BTN_RIGHT_PIN};

static const uint8_t folder_icon_8x8[] = {0x3C, 0x7E, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0x00};

static const uint8_t wifi_icon_8x8[] = {
    0b00111100,  // ..####..
    0b01000010,  // .#....#.
    0b10011001,  // #..##..#
    0b00100100,  // ..#..#..
    0b00000000,  // ........
    0b00011000,  // ...##...
    0b00011000,  // ...##...
    0b00000000   // ........
};

// 16x16 Reel Bitmaps (3コマのアニメーションパターン)
static const uint8_t PROGMEM reel_bmp_0[] = {
    0b00000111, 0b11100000, 0b00011000, 0b00011000, 0b00100000, 0b00000100,
    0b01000001, 0b10000010, 0b01000001, 0b10100010, 0b10000001, 0b10000001,
    0b10000001, 0b10000001, 0b10000001, 0b10000001, 0b10000001, 0b10000001,
    0b10000001, 0b10000001, 0b10000001, 0b10000001, 0b01000001, 0b10000010,
    0b01000001, 0b10000010, 0b00100000, 0b00000100, 0b00011000, 0b00011000,
    0b00000111, 0b11100000};

static const uint8_t PROGMEM reel_bmp_1[] = {
    0b00000111, 0b11100000, 0b00011000, 0b00011000, 0b00100000, 0b00000100,
    0b01000000, 0b00000010, 0b01001000, 0b00000010, 0b10001100, 0b00000001,
    0b10000110, 0b00000001, 0b10000011, 0b00000001, 0b10000001, 0b10000001,
    0b10000000, 0b11000001, 0b10000000, 0b01100001, 0b01000000, 0b00110010,
    0b01000000, 0b00100010, 0b00100000, 0b00000100, 0b00011000, 0b00011000,
    0b00000111, 0b11100000};

static const uint8_t PROGMEM reel_bmp_2[] = {
    0b00000111, 0b11100000, 0b00011000, 0b00011000, 0b00100000, 0b00000100,
    0b01000000, 0b00000010, 0b01000000, 0b00000010, 0b10000000, 0b00000001,
    0b10000000, 0b00000001, 0b10011111, 0b11111001, 0b10011111, 0b11111001,
    0b10000000, 0b00000001, 0b10000000, 0b00000001, 0b01000000, 0b00000010,
    0b01000000, 0b00000010, 0b00100000, 0b00000100, 0b00011000, 0b00011000,
    0b00000111, 0b11100000};

static const uint8_t PROGMEM reel_bmp_3[] = {
    0b00000111, 0b11100000, 0b00011000, 0b00011000, 0b00100000, 0b00000100,
    0b01000000, 0b00000010, 0b01000000, 0b00010010, 0b10000000, 0b00110001,
    0b10000000, 0b01100001, 0b10000000, 0b11000001, 0b10000001, 0b10000001,
    0b10000011, 0b00000001, 0b10000110, 0b00000001, 0b01001100, 0b00000010,
    0b01000100, 0b00000010, 0b00100000, 0b00000100, 0b00011000, 0b00011000,
    0b00000111, 0b11100000};

static const uint8_t* reel_bmps[4] = {reel_bmp_0, reel_bmp_1, reel_bmp_2,
                                      reel_bmp_3};

/**
 * @brief カセットテープのアニメーションを描画する共通関数
 * @param x 描画位置X
 * @param y 描画位置Y
 * @param is_playing trueならリールが回転する
 */
static void draw_cassette_animation(int x, int y, bool is_playing) {
    static uint32_t anim_last_ms = 0;
    static int current_frame = 0;
    static int current_frame2 = 2;
    uint32_t now = millis();

    if (is_playing) {
        if (now - anim_last_ms > 100) {  // 100msごとにコマ送り
            current_frame = (current_frame + 1) % 4;
            current_frame2 = (current_frame + 3) % 4;
            anim_last_ms = now;
        }
    }

    int cas_w = 46;
    int cas_h = 24;

    oled.drawLine(x + 10, y + 19, x + 34, y + 19, SSD1306_WHITE);

    // リール部分のビットマップ描画 (16x16)
    oled.drawBitmap(x + 3, y + 4, reel_bmps[current_frame], 16, 16,
                    SSD1306_WHITE);
    oled.drawBitmap(x + 26, y + 4, reel_bmps[current_frame2], 16, 16,
                    SSD1306_WHITE);
}

static void draw_menu_footer(const char* left_label, const char* right_label) {
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.drawLine(0, 54, 127, 54, SSD1306_WHITE);
    oled.setCursor(0, 56);
    oled.print("L:");
    oled.print(left_label);
    oled.print("  R:");
    oled.println(right_label);
}

void draw_marquee_text(int x, int y, int max_chars, const char* text,
                       bool scroll_enabled) {
    static char last_scrolled_text[128] = {0};
    static uint32_t text_start_ms = 0;

    oled.setTextWrap(false);
    int text_len = (int)strlen(text);

    if (!scroll_enabled || text_len <= max_chars) {
        oled.setCursor(x, y);
        oled.print(text);
        return;
    }

    if (strcmp(last_scrolled_text, text) != 0) {
        snprintf(last_scrolled_text, sizeof(last_scrolled_text), "%s", text);
        text_start_ms = millis();
    }

    int max_offset = text_len - max_chars;
    if (max_offset < 0) {
        max_offset = 0;
    }

    uint32_t hold_start_ms = 1200;
    uint32_t step_ms = 220;
    uint32_t hold_end_ms = 1200;

    uint32_t scroll_duration = max_offset * step_ms;
    uint32_t cycle_ms = hold_start_ms + scroll_duration + hold_end_ms;

    uint32_t elapsed = (millis() - text_start_ms) % cycle_ms;

    int offset = 0;
    if (elapsed < hold_start_ms) {
        offset = 0;
    } else if (elapsed < hold_start_ms + scroll_duration) {
        offset = (elapsed - hold_start_ms) / step_ms;
    } else {
        offset = max_offset;
    }

    char line[40];
    int visible_len = max_chars;
    if (visible_len > (int)sizeof(line) - 1) {
        visible_len = (int)sizeof(line) - 1;
    }
    for (int i = 0; i < visible_len; i++) {
        line[i] = text[offset + i];
    }
    line[visible_len] = '\0';

    oled.setCursor(x, y);
    oled.print(line);
}

void draw_conversion_progress(int percent) {
    watchdog_update();
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextWrap(false);

    oled.setCursor(0, 0);
    oled.println("Converting...");

    oled.setCursor(0, 16);
    oled.print(target_filename);

    int bar_x = 0;
    int bar_y = 36;
    int bar_w = 128;
    int bar_h = 10;
    oled.drawRect(bar_x, bar_y, bar_w, bar_h, SSD1306_WHITE);

    int fill_w = (percent * (bar_w - 2)) / 100;
    if (fill_w > 0) {
        if (fill_w > bar_w - 2) {
            fill_w = bar_w - 2;
        }
        oled.fillRect(bar_x + 1, bar_y + 1, fill_w, bar_h - 2, SSD1306_WHITE);
    }

    oled.setCursor(56, 52);
    oled.print(percent);
    oled.print("%");

    oled.display();
}

static void render_playback_screen_to_oled() {
    PlaybackStatus status = {};
    get_playback_status(&status);

    const char* status_text = "STANDBY";
    if (status.playback_t88_end_reached && !status.play_active) {
        status_text = "COMPLETE";
    } else if (playback_user_paused) {
        status_text = "PAUSED";
    } else if (current_state == STATE_PLAYING) {
        status_text = "PLAYING";
    }

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextWrap(false);

    draw_marquee_text(0, 0, 21, target_filename, true);

    oled.setCursor(0, 10);
    oled.print("Status: ");
    oled.println(status_text);

    const int bar_x = 0;
    const int bar_y = 24;
    const int bar_w = 128;
    const int bar_h = 10;
    oled.drawRect(bar_x, bar_y, bar_w, bar_h, SSD1306_WHITE);
    int fill_w = 0;
    if (status.playback_t88_end_reached && !status.play_active) {
        fill_w = bar_w - 2;
    } else if (!playback_started) {
        fill_w = 0;
    } else if (status.file_size > 0) {
        fill_w = (int)(((uint64_t)(bar_w - 2) * status.file_position) /
                       status.file_size);
        if (fill_w < 0) {
            fill_w = 0;
        }
        if (fill_w > bar_w - 2) {
            fill_w = bar_w - 2;
        }
    }
    oled.fillRect(bar_x + 1, bar_y + 1, fill_w, bar_h - 2, SSD1306_WHITE);

    bool is_playing = (current_state == STATE_PLAYING && !playback_user_paused);
    draw_cassette_animation(0, 38, is_playing);

    oled.display();
}

void render_wifi_menu_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    int min_cursor = wifi_settings_saved ? 0 : 1;
    if (wifi_menu_cursor < min_cursor) {
        wifi_menu_cursor = min_cursor;
    }

    oled.setCursor(0, 0);
    oled.println("--- SETTINGS ---");

    int y0 = 10;
    oled.setCursor(0, y0);
    oled.print(wifi_menu_cursor == 0 ? ">" : " ");
    oled.print(" WiFi: ");
    if (wifi_settings_saved) {
        if (wifi_opt_sel == 0) {
            oled.println("CLEAR");
        } else if (wifi_opt_sel == 1) {
            oled.println("OFF");
        } else if (wifi_opt_sel == 2) {
            oled.println("ON");
        }
    } else {
        oled.println("DISABLED");
    }

    int y1 = 20;
    oled.setCursor(0, y1);
    oled.print(wifi_menu_cursor == 1 ? ">" : " ");
    oled.println(" WiFi Config");

    int y2 = 30;
    oled.setCursor(0, y2);
    oled.print(wifi_menu_cursor == 2 ? ">" : " ");
    oled.print(" Rec Fmt: ");
    const char* fmt_names[] = {"T88", "CMT", "CAS"};  // CASを追加

    // 安全装置: 万が一未定義のフォーマットが設定されていた場合はT88に戻す
    if (default_rec_fmt >= 3) {
        default_rec_fmt = FMT_T88;
    }
    oled.println(fmt_names[default_rec_fmt]);

    oled.setCursor(0, 44);
    oled.print("IP: ");
    if (wifi_connect_in_progress) {
        if (wifi_ui_blink_on) {
            oled.print("Connecting..");
        }
    } else if (wifi_connected) {
        oled.print(WiFi.localIP());
    } else {
        oled.print("Disconnected");
    }

    oled.setCursor(0, 54);
    oled.print("ID:");
    oled.print(FTP_USERNAME);
    oled.print(" PW:");
    oled.print(FTP_PASSWORD);

    oled.display();
}

void init_ui_buttons() {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
    }
}

void reset_and_ignore_pressed_buttons() {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        btn_trigger_clicked[i] = false;
        btn_trigger_repeat[i] = false;
        if (buttons[i].current_stable_state) {
            buttons[i].ignore_until_release = true;
        }
    }
}

void scan_ui_buttons() {
    uint32_t now = millis();
    for (int i = 0; i < NUM_BUTTONS; i++) {
        bool raw_pressed = (gpio_get(button_pins[i]) == 0);
        if (raw_pressed != buttons[i].last_raw_state) {
            buttons[i].state_changed_ms = now;
            buttons[i].last_raw_state = raw_pressed;
        } else {
            if ((now - buttons[i].state_changed_ms) >= DEBOUNCE_MS) {
                if (raw_pressed != buttons[i].current_stable_state) {
                    buttons[i].current_stable_state = raw_pressed;

                    if (buttons[i].current_stable_state) {
                        if (!buttons[i].ignore_until_release) {
                            btn_trigger_clicked[i] = true;
                        }
                        buttons[i].pressed_duration_ms = now;
                        buttons[i].is_long_pressed = false;
                    } else {
                        buttons[i].pressed_duration_ms = 0;
                        buttons[i].is_long_pressed = false;
                        buttons[i].ignore_until_release = false;
                    }
                }
            }
        }

        if (buttons[i].current_stable_state &&
            buttons[i].pressed_duration_ms > 0) {
            if (!buttons[i].ignore_until_release) {
                uint32_t held_time = now - buttons[i].pressed_duration_ms;
                if (held_time >= LONG_PRESS_MS) {
                    buttons[i].is_long_pressed = true;
                    if ((now - buttons[i].last_repeat_ms) >= REPEAT_MS) {
                        btn_trigger_repeat[i] = true;
                        buttons[i].last_repeat_ms = now;
                    }
                }
            }
        }
    }
}

void render_rec_menu_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(0, 0);
    oled.println("--- REC OPTION ---");

    oled.setCursor(0, 12);
    oled.print("File: ");
    draw_marquee_text(36, 12, 15, target_filename, true);

    const char* ext = strrchr(target_filename, '.');
    bool is_appendable = false;
    if (ext) {
        if (strcasecmp(ext, ".T88") == 0 || strcasecmp(ext, ".T77") == 0 ||
            strcasecmp(ext, ".CAS") == 0) {
            is_appendable = true;
        }
    }

    const char* menu_items[] = {" Create New File", " Append to File"};
    int menu_count = is_appendable ? 2 : 1;

    for (int i = 0; i < menu_count; i++) {
        int y_pos = 28 + (i * 12);
        oled.setCursor(0, y_pos);
        if (i == rec_menu_cursor) {
            oled.print(">");
        } else {
            oled.print(" ");
        }
        oled.println(menu_items[i]);
    }

    oled.display();
}

void render_file_browser_to_oled() {
    oled.clearDisplay();
    oled.setTextWrap(false);
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    int max_path_len = wifi_connected ? 18 : 21;
    String path_str = current_path;

    if (path_str.length() > max_path_len) {
        path_str =
            "..." + path_str.substring(path_str.length() - (max_path_len - 3));
    }
    oled.setCursor(0, 0);
    oled.print(path_str);

    if (wifi_connected) {
        oled.drawBitmap(118, 0, wifi_icon_8x8, 8, 8, SSD1306_WHITE);
    }

    oled.drawLine(0, 11, 127, 11, SSD1306_WHITE);

    if (total_items == 0) {
        oled.setCursor(16, 28);
        oled.println("Empty folder");
        oled.display();
        return;
    }

    // 4行表示
    for (int i = 0; i < 4; i++) {
        int item_idx = scroll_offset + i;
        if (item_idx >= total_items) {
            break;
        }

        FileItem item = file_list[item_idx];
        int y_pos = 14 + (i * 12);

        if (item_idx == current_cursor) {
            oled.setCursor(0, y_pos);
            oled.print(">");
        }

        if (item.is_dir) {
            oled.drawBitmap(12, y_pos, folder_icon_8x8, 8, 8, WHITE);
        }

        draw_marquee_text(24, y_pos, 17, item.name, item_idx == current_cursor);
    }

    // CMTまたはCASファイル選択時のボーレート選択領域描画
    if (total_items > 0 && !file_list[current_cursor].is_dir) {
        const char* ext = strrchr(file_list[current_cursor].name, '.');
        bool is_cmt = (ext && strcasecmp(ext, ".CMT") == 0);
        bool is_cas = (ext && strcasecmp(ext, ".CAS") == 0);

        if (is_cmt || is_cas) {
            int cursor_on_screen = current_cursor - scroll_offset;
            int ui_line = (cursor_on_screen == 0)   ? 2
                          : (cursor_on_screen == 1) ? 3
                                                    : (cursor_on_screen - 2);

            int ui_y = 14 + (ui_line * 12) - 6;
            int ui_x = 64;
            int ui_w = 54;
            int ui_h = 21;

            oled.fillRect(ui_x - 2, ui_y - 2, ui_w + 4, ui_h + 4,
                          SSD1306_BLACK);
            oled.drawRoundRect(ui_x, ui_y, ui_w, ui_h, 3, SSD1306_WHITE);
            oled.drawRoundRect(ui_x + 1, ui_y + 1, ui_w - 2, ui_h - 2, 2,
                               SSD1306_WHITE);

            // ボーレート数字の描画（サイズ2に拡大）
            oled.setTextSize(2);
            oled.setCursor(ui_x + 4, ui_y + 3);
            if (is_cmt) {
                if (cmt_selected_baud_rate == 1200) {
                    oled.print("1200");
                } else {
                    oled.print(" 600");
                }
            } else if (is_cas) {
                if (cas_selected_baud_rate == 1200) {
                    oled.print("1200");
                } else {
                    oled.print("2400");
                }
            }
            oled.setTextSize(1);  // 描画後に元のサイズに戻す

            // 左右の操作ガイド（三角矢印風）- 黒枠のさらに外側に配置
            oled.drawPixel(ui_x - 4, ui_y + 10, SSD1306_WHITE);
            oled.drawLine(ui_x - 3, ui_y + 8, ui_x - 3, ui_y + 12,
                          SSD1306_WHITE);

            oled.drawPixel(ui_x + ui_w + 3, ui_y + 10, SSD1306_WHITE);
            oled.drawLine(ui_x + ui_w + 2, ui_y + 8, ui_x + ui_w + 2, ui_y + 12,
                          SSD1306_WHITE);
        }
    }

    oled.display();
}

void render_wifi_config_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(0, 0);
    oled.println("-- WIFI CONFIG --");

    oled.setCursor(0, 12);
    oled.print("AP:");
    if (wifi_scan_in_progress) {
        oled.println("Scanning WiFi...");
    } else if (wifi_scan_count > 0) {
        if (wifi_config_field != 0 || wifi_ui_blink_on) {
            draw_marquee_text(24, 12, 16, wifi_scan_ssids[wifi_scan_cursor],
                              true);
        } else {
            oled.setCursor(24, 12);
            oled.print("                ");
        }
    } else if (wifi_ssid[0] != '\0') {
        draw_marquee_text(24, 12, 16, wifi_ssid, true);
    } else {
        oled.println("AP not found");
    }

    const int pass_visible_chars = 12;
    int pass_window_start = 0;
    if (wifi_config_field == 1 && wifi_config_pos >= pass_visible_chars) {
        pass_window_start = wifi_config_pos - pass_visible_chars + 1;
    }

    oled.setCursor(0, 24);
    oled.print("PASS:");
    for (int i = 0; i < pass_visible_chars; i++) {
        int idx = pass_window_start + i;
        bool has_char = (idx >= 0 && idx < 64 && wifi_password[idx] != '\0');
        char ch = '_';
        if (has_char) {
            if (wifi_config_field == 1) {
                ch = wifi_password[idx];
            } else {
                ch = '*';
            }
        } else if (wifi_settings_saved && wifi_config_field != 1 && idx < 4) {
            ch = '*';
        }
        if (wifi_config_field == 1 && idx == wifi_config_pos &&
            !wifi_ui_blink_on) {
            ch = ' ';
        }
        oled.print(ch);
    }

    oled.setCursor(0, 36);
    oled.print("PIN :");
    for (int i = 0; i < 4; i++) {
        bool has_digit = (wifi_pin[i] >= '0' && wifi_pin[i] <= '9');
        char ch = '_';
        if (wifi_config_field == 2) {
            ch = has_digit ? wifi_pin[i] : '_';
        } else if (has_digit || (wifi_settings_saved && i < 4)) {
            ch = '*';
        }
        if (wifi_config_field == 2 && i == wifi_config_pos &&
            !wifi_ui_blink_on) {
            ch = ' ';
        }
        oled.print(ch);
        if (i < 3) {
            oled.print(' ');
        }
    }

    oled.setCursor(0, 56);
    if (wifi_connected) {
        oled.print("IP:");
        oled.print(WiFi.localIP());
    }

    oled.display();
}

void render_wifi_pin_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(0, 0);
    oled.println("-- WIFI PIN --");
    oled.setCursor(0, 12);
    oled.println("Enter PIN to connect");

    oled.setTextSize(2);
    oled.setCursor(16, 28);
    for (int i = 0; i < 4; i++) {
        char ch =
            (wifi_pin[i] >= '0' && wifi_pin[i] <= '9') ? wifi_pin[i] : '_';
        if (i == wifi_config_pos && !wifi_ui_blink_on) {
            ch = ' ';
        }
        oled.print(ch);
        if (i < 3) {
            oled.print(" ");
        }
    }

    oled.setTextSize(1);
    int cursor_x = 16 + (wifi_config_pos * 24);
    oled.drawLine(cursor_x, 49, cursor_x + 10, 49, SSD1306_WHITE);

    oled.display();
}

void render_wifi_connect_error_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(0, 0);
    oled.println("-- WIFI ERROR --");
    oled.setCursor(0, 16);
    oled.println("Connect failed");
    oled.setCursor(0, 28);
    oled.println("Check SSID/PASS");
    oled.setCursor(0, 40);
    oled.println("or PIN and retry");
    oled.setCursor(0, 56);
    oled.println("Press OK to return");

    oled.display();
}

void update_oled_display() {
    if (!oled_need_refresh) {
        return;
    }

    if (wifi_connect_failed || current_state == STATE_WIFI_CONNECT_ERROR) {
        render_wifi_connect_error_to_oled();
        oled.display();
        oled_need_refresh = false;
        return;
    } else if (current_state == STATE_IDLE) {
        if (sd_card_mounted) {
            render_file_browser_to_oled();
        } else {
            oled.clearDisplay();
            oled.setTextSize(1);
            oled.setTextColor(SSD1306_WHITE);
            oled.setCursor(0, 0);
            oled.println("No SD Card");
            oled.setCursor(0, 12);
            oled.println("Insert card...");
            oled.display();
        }
    } else if (current_state == STATE_REC_MENU) {
        render_rec_menu_to_oled();
    } else if (current_state == STATE_WIFI_MENU) {
        render_wifi_menu_to_oled();
    } else if (current_state == STATE_WIFI_CONFIG) {
        render_wifi_config_to_oled();
    } else if (current_state == STATE_WIFI_PIN_CONNECT) {
        render_wifi_pin_to_oled();
    } else if (current_state == STATE_WIFI_SAVE_RESULT) {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 20);
        if (wifi_save_result_success) {
            oled.println("WiFi settings saved");
        } else {
            oled.println("WiFi save failed");
        }
        oled.setCursor(0, 36);
        oled.println("Returning to menu...");
        oled.display();
    } else if (current_state == STATE_PLAY_STANDBY ||
               current_state == STATE_PLAYING) {
        render_playback_screen_to_oled();
    } else {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextWrap(false);

        if (current_state == STATE_RECORDING) {
            oled.setCursor(0, 0);
            oled.print("RECORDING");

            if (rec_triggered && (millis() - rec_last_sound_time_ms) < 300) {
                if ((millis() / 250) % 2 == 0) {
                    oled.setCursor(115, 0);
                    oled.print("*");
                }
            }

            draw_marquee_text(0, 16, 21, target_filename, true);

            if (rec_triggered) {
                uint32_t elapsed_sec = (millis() - rec_start_time_ms) / 1000;
                uint32_t m = elapsed_sec / 60;
                uint32_t s = elapsed_sec % 60;
                char time_str[10];
                snprintf(time_str, sizeof(time_str), "%02lu:%02lu", m, s);

                oled.setCursor(52, 40);
                oled.setTextSize(2);
                oled.print(time_str);
                oled.setTextSize(1);
            } else {
                oled.setCursor(52, 44);
                oled.print("Wait...");
            }

            draw_cassette_animation(0, 38, rec_triggered);

        } else {
            draw_marquee_text(0, 0, 21, target_filename, true);

            if (current_state == STATE_REC_STANDBY) {
                oled.setCursor(0, 16);
                oled.println("REC STANDBY");
                oled.setCursor(0, 26);
                oled.println("Press OK to start");
            }

            draw_cassette_animation(0, 38, false);
        }
    }

    oled.display();
    oled_need_refresh = false;
}