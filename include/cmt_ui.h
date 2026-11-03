#pragma once

void init_ui_buttons();
void scan_ui_buttons();
void reset_and_ignore_pressed_buttons();

// Wi-Fi 接続中 (WiFi.begin() で Core0 が止まる間) のアイコンの点滅
void begin_wifi_icon_blink_on_core1();
void end_wifi_icon_blink_on_core1();
void core1_blink_wifi_icon();

void update_oled_display();
void draw_conversion_progress(int percent);
void draw_marquee_text(int x, int y, int max_chars, const char* text,
                       bool scroll_enabled);

void render_wifi_menu_to_oled();
void render_rec_menu_to_oled();
void render_file_browser_to_oled();
void render_wifi_config_to_oled();
void render_web_pass_config_to_oled();
void render_wifi_connect_error_to_oled();