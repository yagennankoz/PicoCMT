#pragma once

// Wi-Fi・EEPROM設定の初期化
void init_wifi_and_settings();

// Wi-Fi接続やFTPサーバーのバックグラウンド処理（loopで呼ぶ）
void handle_wifi_background_tasks();

// 外部から呼ばれる設定処理
void refresh_wifi_session_key_from_pin();
bool load_wifi_settings_from_flash();
void save_rec_format_to_eeprom();
void clear_wifi_settings_in_eeprom();