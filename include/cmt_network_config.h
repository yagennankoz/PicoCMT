#pragma once

// Wi-Fi settings for Pico W web file manager
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Web file manager (http://<WEB_MDNS_HOSTNAME>.local/)
#define WEB_SERVER_PORT 80
#define WEB_MDNS_HOSTNAME "picocmt"
// mDNS で名前 (picocmt.local) を知らせ直す間隔。LEAmDNS は始めたときに 1秒おきに8回
// 知らせた後は知らせないので、PC 側の覚えている期限 (120秒) より短い間隔で知らせ続ける
#define WEB_MDNS_REANNOUNCE_MS 60000
// Web login account (Basic認証)。設定メニューの Web Pass で ON/OFF とパスワードを変更できる
#define WEB_AUTH_DEFAULT_ENABLED true
#define WEB_USERNAME "pico"
#define WEB_DEFAULT_PASSWORD "pico"
#define WEB_PASSWORD_MAX_LEN 32

// Wi-Fi UI behavior timing (ms)
#define WIFI_UI_BLINK_INTERVAL_MS 350
#define WIFI_INPUT_HOLD_START_MS 450
#define WIFI_SAVE_STATUS_DISPLAY_MS 1200

// Wi-Fi connection retry
// WiFi.begin() は接続(join)が終わるまで戻らないため、1回の試行の待ち時間を
// ウォッチドッグの上限 (RP2040 は約8.3秒) より短くし、その間だけウォッチドッグを延ばす
#define WIFI_JOIN_ATTEMPT_TIMEOUT_MS 7000
#define WIFI_JOIN_WATCHDOG_MS 8300
#define WIFI_JOIN_MAX_ATTEMPTS 4
#define WIFI_JOIN_RETRY_INTERVAL_MS 1000
// 接続開始からIPアドレス取得までの全体の制限時間
#define WIFI_CONNECT_TIMEOUT_MS 40000
// Wi-Fiスキャンの制限時間
#define WIFI_SCAN_TIMEOUT_MS 10000

// 時刻 (NTP)。地域ごとのファームウェアで切り替える (platformio.ini の FW_REGION_JP)
//   CLOCK_TIMEZONE は POSIX 形式 ("JST-9" は UTC+9)
#if defined(FW_REGION_JP)
#define CLOCK_TIMEZONE "JST-9"
#define NTP_SERVER_PRIMARY "ntp.nict.jp"
#define NTP_SERVER_SECONDARY "pool.ntp.org"
#else
#define CLOCK_TIMEZONE "UTC0"
#define NTP_SERVER_PRIMARY "pool.ntp.org"
#define NTP_SERVER_SECONDARY ""
#endif
// NTPサーバー名の問い合わせ (DNS) の待ち時間 (2つ引いてもウォッチドッグに収まる長さ)
#define NTP_DNS_TIMEOUT_MS 1500

// Wi-Fi settings stored in EEPROM
#define WIFI_EEPROM_SIZE 256
