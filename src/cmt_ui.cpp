#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <WiFi.h>

#include "cmt_globals.h"
#include "cmt_hw_config.h"
#include "cmt_network_config.h"
#include "cmt_pins.h"
#include "cmt_storage.h"
#include "cmt_storage_record.h"
#include "cmt_types.h"
#include "cmt_ui.h"
#include "cmt_usb_storage.h"
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
static const int WIFI_ICON_X = 118;  // ファイル一覧の右上 (OLED の 0 ページ目)

// ---------------------------------------------------------------------------
// Wi-Fi 接続中のアイコンの点滅 (Core1 が描く)
// WiFi.begin() は接続が終わるまで戻らず、その間 Core0 は画面を描けない。
// そこで、その間だけ手の空いている Core1 が、アイコンの 8x8 ドットだけを OLED に送って点滅させる。
// Core0 は WiFi.begin() の前後で begin / end を呼ぶ。end は Core1 が送り終わるのを待つので、
// 2つのコアが同時に Wire1 (OLED) を使うことはない
static volatile bool wifi_icon_blink_requested = false;
static volatile bool wifi_icon_blink_busy = false;

void begin_wifi_icon_blink_on_core1() {
    // アイコンを出すのはファイル一覧だけ
    if (current_state != STATE_IDLE || !sd_card_mounted) {
        return;
    }
    __dmb();
    wifi_icon_blink_requested = true;
}

void end_wifi_icon_blink_on_core1() {
    if (!wifi_icon_blink_requested) {
        return;
    }
    wifi_icon_blink_requested = false;
    __dmb();
    while (wifi_icon_blink_busy) {
        tight_loop_contents();
    }
    oled_need_refresh = true;  // Core0 の画面バッファで描き直す
}

// アイコンの部分 (列 WIFI_ICON_X〜+7、0 ページ目) だけを OLED に送る
static void send_wifi_icon_to_oled(bool on) {
    uint8_t cols[8];
    for (int c = 0; c < 8; c++) {
        uint8_t b = 0;
        if (on) {
            for (int r = 0; r < 8; r++) {
                if ((wifi_icon_8x8[r] >> (7 - c)) & 1) {
                    b |= (uint8_t)(1u << r);  // 1バイト = 縦8ドット (下位ビットが上)
                }
            }
        }
        cols[c] = b;
    }
    Wire1.beginTransmission(SCREEN_ADDRESS);
    Wire1.write((uint8_t)0x00);  // 続くのはコマンド
    Wire1.write((uint8_t)0x21);  // 列の範囲
    Wire1.write((uint8_t)WIFI_ICON_X);
    Wire1.write((uint8_t)(WIFI_ICON_X + 7));
    Wire1.write((uint8_t)0x22);  // ページの範囲
    Wire1.write((uint8_t)0);
    Wire1.write((uint8_t)0);
    Wire1.endTransmission();
    Wire1.beginTransmission(SCREEN_ADDRESS);
    Wire1.write((uint8_t)0x40);  // 続くのは表示データ
    Wire1.write(cols, sizeof(cols));
    Wire1.endTransmission();
}

// Core1 の loop1() から、再生・録音していないときに呼ぶ
void core1_blink_wifi_icon() {
    static uint32_t last_toggle_ms = 0;
    static bool icon_on = true;

    if (!wifi_icon_blink_requested) {
        last_toggle_ms = 0;
        return;
    }
    uint32_t now_ms = millis();
    if (last_toggle_ms == 0) {
        // 始めたときは Core0 が描いた画面 (アイコンあり) のまま
        last_toggle_ms = now_ms;
        icon_on = true;
        return;
    }
    if ((now_ms - last_toggle_ms) < WIFI_UI_BLINK_INTERVAL_MS) {
        return;
    }

    wifi_icon_blink_busy = true;
    __dmb();
    // busy を立てた後に確かめ直す (end が先に呼ばれていたら送らない)
    if (wifi_icon_blink_requested) {
        last_toggle_ms = now_ms;
        icon_on = !icon_on;
        send_wifi_icon_to_oled(icon_on);
    }
    __dmb();
    wifi_icon_blink_busy = false;
}

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

    oled.drawLine(x + 10, y + 19, x + 34, y + 19, SSD1306_WHITE);

    // リール部分のビットマップ描画 (16x16)
    oled.drawBitmap(x + 3, y + 4, reel_bmps[current_frame], 16, 16,
                    SSD1306_WHITE);
    oled.drawBitmap(x + 26, y + 4, reel_bmps[current_frame2], 16, 16,
                    SSD1306_WHITE);
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

    if (playback_time_valid) {
        uint32_t current_play_ms = 0;
        if (is_playing) {
            current_play_ms = playback_base_time_ms +
                              (millis() - playback_tag_start_system_ms);
        } else {
            current_play_ms = playback_base_time_ms;
        }

        uint32_t total_sec = current_play_ms / 1000;
        uint32_t m = total_sec / 60;
        uint32_t s = total_sec % 60;

        char time_str[10];
        snprintf(time_str, sizeof(time_str), "%02lu:%02lu", m, s);

        oled.setCursor(52, 40);
        oled.setTextSize(2);
        oled.print(time_str);
        oled.setTextSize(1);
    }

    oled.display();
}

// Webファイルマネージャを開くURL ("http://picocmt.local/" は21文字で、画面の幅ちょうど)
#define WEB_URL_TEXT "http://" WEB_MDNS_HOSTNAME ".local/"
#define OLED_TEXT_COLUMNS 21

// IPアドレスのURL。画面に入りきらない長さのアドレスは "IP:" の形で表示する
static void print_ip_url() {
    String ip = WiFi.localIP().toString();
    if (7 + ip.length() + 1 <= OLED_TEXT_COLUMNS) {
        oled.print("http://");
        oled.print(ip);
        oled.print("/");
    } else {
        oled.print("IP:");
        oled.print(ip);
    }
}

void fix_settings_menu_cursor() {
    if (wifi_menu_cursor < 0 || wifi_menu_cursor >= SET_ROW_COUNT) {
        wifi_menu_cursor = SET_ROW_REC_FMT;
    }
    // Wi-Fi設定が無いとWiFiの行は選べないので、WiFi Configに移す
    if (wifi_menu_cursor == SET_ROW_WIFI && !wifi_settings_saved) {
        wifi_menu_cursor = SET_ROW_WIFI_CONFIG;
    }
}

// 設定メニューのrow行目の頭(カーソル)を出す。行の間隔は6行を最下行のIP表示の上に収める
static void begin_settings_row(int row) {
    const int row_h = 8;
    const int y0 = 9;
    oled.setCursor(0, y0 + row * row_h);
    oled.print(wifi_menu_cursor == row ? ">" : " ");
}

void render_wifi_menu_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    fix_settings_menu_cursor();

    oled.setCursor(0, 0);
    oled.println("--- SETTINGS ---");

    begin_settings_row(SET_ROW_REC_FMT);
    oled.print(" Rec Fmt: ");
    if (default_rec_fmt >= FMT_MAX) {
        default_rec_fmt = FMT_T88;
    }
    oled.println(rec_format_name(default_rec_fmt));

    begin_settings_row(SET_ROW_FILTER);
    oled.print(" Filter: ");
    oled.println(file_filter_name(file_filter));

    begin_settings_row(SET_ROW_WIFI);
    oled.print(" WiFi: ");
    if (wifi_settings_saved) {
        // 1:OFF、2:ON(設定を消すのはWiFi Config画面で行う)
        static const char* const opt_names[] = {"", "OFF", "ON"};
        // 今の状態と違う値を選んでいる (まだ OK を押していない) ときは、
        // 値を点滅させて"OK?"を付ける
        int current_opt = wifi_user_enabled ? 2 : 1;
        bool pending = (wifi_opt_sel != current_opt);
        int opt = (wifi_opt_sel >= 1 && wifi_opt_sel <= 2) ? wifi_opt_sel : current_opt;
        if (!pending || wifi_ui_blink_on) {
            oled.print(opt_names[opt]);
        } else {
            oled.print(opt == 1 ? "   " : "  ");
        }
        if (pending) {
            oled.print(" OK?");
        }
        oled.println();
    } else {
        oled.println("DISABLED");
    }

    begin_settings_row(SET_ROW_WIFI_CONFIG);
    oled.println(" WiFi Config");

    begin_settings_row(SET_ROW_WEB_PASS);
    oled.print(" Web Pass: ");
    oled.println(web_auth_enabled ? "ON" : "OFF");

    begin_settings_row(SET_ROW_USB);
    oled.print(" USB Drive");
    if (!usb_host_connected()) {
        oled.print(": no PC");
    }

    oled.setCursor(0, 57);
    if (wifi_connect_in_progress) {
        if (wifi_ui_blink_on) {
            oled.print("IP: Connecting..");
        }
    } else if (!wifi_connected) {
        oled.print("IP: Disconnected");
    } else {
        // 接続中は、ブラウザで開くURL・IPアドレス・ログイン情報 (認証 ON のとき) を順に表示する
        switch ((millis() / 2500) % (web_auth_enabled ? 3 : 2)) {
            case 0:
                oled.print(WEB_URL_TEXT);
                break;
            case 1:
                print_ip_url();
                break;
            default:
                oled.print("ID:");
                oled.print(WEB_USERNAME);
                oled.print(" PW:");
                oled.print(web_password);
                break;
        }
    }

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

    bool is_appendable = is_appendable_filename(target_filename);

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

    // 接続中 (つなぎ直しを含む) はアイコンを点滅させる
    bool show_wifi_icon = wifi_connected || wifi_connect_in_progress;
    int max_path_len = show_wifi_icon ? 18 : 21;
    // フィルタ中は、フィルタの名前(例:[T88])をパスの右に出す
    char filter_label[8] = {0};
    if (file_filter != FILTER_ALL) {
        snprintf(filter_label, sizeof(filter_label), "[%s]", file_filter_name(file_filter));
        max_path_len -= (int)strlen(filter_label);
    }
    String path_str = current_path;

    if (path_str.length() > max_path_len) {
        path_str =
            "..." + path_str.substring(path_str.length() - (max_path_len - 3));
    }
    oled.setCursor(0, 0);
    oled.print(path_str);
    if (filter_label[0] != 0) {
        int right = show_wifi_icon ? WIFI_ICON_X - 2 : 128;
        oled.setCursor(right - 6 * (int)strlen(filter_label), 0);
        oled.print(filter_label);
    }

    if (wifi_connected || (wifi_connect_in_progress && wifi_ui_blink_on)) {
        oled.drawBitmap(WIFI_ICON_X, 0, wifi_icon_8x8, 8, 8, SSD1306_WHITE);
    }

    oled.drawLine(0, 11, 127, 11, SSD1306_WHITE);

    if (total_items == 0) {
        oled.setCursor(16, 28);
        oled.println("Empty folder");
        if (file_filter != FILTER_ALL) {
            oled.setCursor(16, 40);
            oled.print("(Filter: ");
            oled.print(file_filter_name(file_filter));
            oled.print(")");
        }
        oled.display();
        return;
    }

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

    if (total_items > 0 && !file_list[current_cursor].is_dir) {
        const char* ext = strrchr(file_list[current_cursor].name, '.');
        bool is_cmt = (ext && strcasecmp(ext, ".CMT") == 0);
        bool is_cas = (ext && strcasecmp(ext, ".CAS") == 0);
        bool is_p6 = (ext && strcasecmp(ext, ".P6") == 0);

        if (is_cmt || is_cas || is_p6) {
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
            if (is_cmt || is_p6) {  // P6 は CMT とボーレート設定を共用
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
            oled.setTextSize(1);

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

    if (wifi_clear_confirm) {
        oled.setCursor(0, 16);
        oled.println("Clear saved WiFi");
        oled.setCursor(0, 26);
        oled.println("settings (SSID/PASS)?");
        oled.setCursor(0, 44);
        oled.print("OK:Clear STOP:Cancel");
        oled.display();
        return;
    }

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
        }
        if (wifi_config_field == 1 && idx == wifi_config_pos &&
            !wifi_ui_blink_on) {
            ch = ' ';
        }
        oled.print(ch);
    }

    oled.setCursor(0, 40);
    oled.print(wifi_config_field == 0 ? "OK:Select STOP:Back" : "OK:Save STOP:Cancel");
    if (wifi_config_field == 0 && wifi_settings_saved) {
        oled.setCursor(0, 48);
        oled.print("LEFT:Clear saved");
    }

    oled.setCursor(0, 56);
    if (wifi_connected) {
        oled.print(WEB_URL_TEXT);
    }

    oled.display();
}

void render_web_pass_config_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(0, 0);
    oled.println("-- WEB PASSWORD --");
    oled.setCursor(0, 12);
    oled.print("ID  :");
    oled.print(WEB_USERNAME);

    const int pass_visible_chars = 12;
    int pass_window_start = 0;
    if (wifi_config_pos >= pass_visible_chars) {
        pass_window_start = wifi_config_pos - pass_visible_chars + 1;
    }

    oled.setCursor(0, 24);
    oled.print("PASS:");
    for (int i = 0; i < pass_visible_chars; i++) {
        int idx = pass_window_start + i;
        char ch = '_';
        if (idx < WEB_PASSWORD_MAX_LEN && web_password_edit[idx] != '\0') {
            ch = web_password_edit[idx];
        }
        if (idx == wifi_config_pos && !wifi_ui_blink_on) {
            ch = ' ';
        }
        oled.print(ch);
    }

    oled.setCursor(0, 40);
    oled.print("OK:Save STOP:Cancel");
    oled.setCursor(0, 52);
    oled.print("All blank: " WEB_DEFAULT_PASSWORD);

    oled.display();
}

void render_wifi_connect_error_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    // 失敗の理由ごとに、見出しと確かめること(1行21文字)
    const char* title = "Connect failed";
    const char* hint1 = "Check SSID/PASS";
    const char* hint2 = "and retry";
    switch (wifi_fail_reason) {
        case WIFI_FAIL_BAD_PASSWORD:
            title = "Wrong password";
            hint1 = "AP rejected the PASS";
            hint2 = "Fix it in WiFi Config";
            break;
        case WIFI_FAIL_AP_NOT_FOUND:
            title = "AP not found";
            hint1 = "Check SSID / range";
            hint2 = "(2.4GHz only)";
            break;
        case WIFI_FAIL_JOIN:
            title = "Could not join AP";
            hint1 = "Weak signal, or PASS";
            hint2 = "may be wrong";
            break;
        case WIFI_FAIL_NO_IP:
            title = "No IP address";
            hint1 = "Joined, but DHCP";
            hint2 = "timed out (router?)";
            break;
        case WIFI_FAIL_SETTINGS:
            title = "Settings unreadable";
            hint1 = "Set WiFi again in";
            hint2 = "WiFi Config";
            break;
        default:
            break;
    }

    oled.setCursor(0, 0);
    oled.println("-- WIFI ERROR --");
    oled.setCursor(0, 12);
    oled.println(title);
    oled.setCursor(0, 22);
    oled.println(hint1);
    oled.setCursor(0, 32);
    oled.println(hint2);
    // 接続先と、Wi-Fiチップの状態(調べるとき用)
    oled.setCursor(0, 44);
    oled.print("SSID:");
    // 状態の番号を右端に出すので、SSIDは13文字まで
    char ssid_short[14];
    snprintf(ssid_short, sizeof(ssid_short), "%s", wifi_ssid);
    oled.print(ssid_short);
    if (wifi_fail_link_status != 0) {
        oled.setCursor(108, 44);
        oled.print(wifi_fail_link_status);
    }
    oled.setCursor(0, 56);
    oled.println("Press OK to return");

    oled.display();
}

const char* rec_format_name(int fmt) {
    static const char* const names[] = {"T88", "CMT", "CAS", "P6", "T77", "P6T", "RAW"};
    static_assert(sizeof(names) / sizeof(names[0]) == FMT_MAX, "names");
    return (fmt >= 0 && fmt < FMT_MAX) ? names[fmt] : "?";
}

// パスのファイル名の部分
static const char* base_name_of(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void render_convert_menu_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextWrap(false);

    oled.setCursor(0, 0);
    oled.println("--- CONVERT ---");
    draw_marquee_text(0, 12, 21, base_name_of(convert_src_path), true);

    if (convert_result == 0) {
        oled.setCursor(0, 28);
        oled.print("To: < ");
        oled.print(rec_format_name(convert_fmt));
        oled.print(" >");
        oled.setCursor(0, 56);
        oled.print("OK:Convert STOP:Back");
    } else {
        oled.setCursor(0, 28);
        if (convert_result > 0) {
            oled.print("Done:");
            draw_marquee_text(30, 28, 16, base_name_of(convert_out_path), true);
        } else {
            oled.print("Convert failed");
        }
        oled.setCursor(0, 56);
        oled.print("OK:Back");
    }
    oled.display();
}

void render_usb_prompt_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("--- USB ---");
    oled.setCursor(0, 14);
    oled.println("PC connected.");
    oled.setCursor(0, 26);
    oled.println("Use the SD card as");
    oled.setCursor(0, 36);
    oled.println("a USB drive?");
    oled.setCursor(0, 56);
    oled.print("OK:Yes STOP:No");
    oled.display();
}

void render_usb_storage_to_oled() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("--- USB DRIVE ---");
    if (usb_exit_confirm) {
        oled.setCursor(0, 14);
        oled.println("Exit USB drive?");
        oled.setCursor(0, 26);
        oled.println("Eject it on the PC");
        oled.setCursor(0, 36);
        oled.println("first.");
        oled.setCursor(0, 56);
        oled.print("OK:Exit STOP:Back");
        oled.display();
        return;
    }
    oled.setCursor(0, 14);
    oled.println("The SD card is shown");
    oled.setCursor(0, 24);
    oled.println("to the PC.");
    oled.setCursor(0, 36);
    oled.print("R:");
    oled.print(usb_storage_read_kb());
    oled.print("KB W:");
    oled.print(usb_storage_write_kb());
    oled.print("KB");
    oled.setCursor(0, 56);
    oled.print("Eject on PC / STOP");
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
    } else if (current_state == STATE_CONVERT_MENU) {
        render_convert_menu_to_oled();
    } else if (current_state == STATE_USB_PROMPT) {
        render_usb_prompt_to_oled();
    } else if (current_state == STATE_USB_STORAGE) {
        render_usb_storage_to_oled();
    } else if (current_state == STATE_WIFI_MENU) {
        render_wifi_menu_to_oled();
    } else if (current_state == STATE_WIFI_CONFIG) {
        render_wifi_config_to_oled();
    } else if (current_state == STATE_WEB_PASS_CONFIG) {
        render_web_pass_config_to_oled();
    } else if (current_state == STATE_WIFI_SAVE_RESULT) {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 16);
        oled.print(settings_save_result_label);
        oled.setCursor(0, 26);
        oled.print(wifi_save_result_success ? "Saved" : "Save failed");
        oled.setCursor(0, 44);
        oled.print("Returning to menu...");
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