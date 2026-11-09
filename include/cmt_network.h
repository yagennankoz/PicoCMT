#pragma once

// Wi-Fi・EEPROM設定の初期化
void init_wifi_and_settings();

// Wi-Fi接続やWebサーバーのバックグラウンド処理（loopで呼ぶ）
void handle_wifi_background_tasks();

// 外部から呼ばれる設定処理
bool load_wifi_settings_from_flash();
// 電源を入れたときに Wi-Fi に自動で接続するかを保存する (WiFi: ON/OFF を確定したときに呼ぶ)
void save_wifi_auto_connect(bool enabled);
// Webのパスワードを保存する (空なら初期値に戻す)
bool save_web_password(const char* password);
// Webの認証の ON/OFF を保存する
bool save_web_auth_enabled(bool enabled);
void save_rec_format_to_eeprom();
void save_file_filter_to_eeprom();
void clear_wifi_settings_in_eeprom();