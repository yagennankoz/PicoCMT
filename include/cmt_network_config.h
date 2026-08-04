#pragma once

// Wi-Fi settings for Pico W FTP server
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// FTP login account
#define FTP_USERNAME "pico"
#define FTP_PASSWORD "pico"

// Max failed PIN/decrypt attempts before deleting stored Wi-Fi password
#define WIFI_PIN_MAX_ATTEMPTS 3

// Wi-Fi UI behavior timing (ms)
#define WIFI_UI_BLINK_INTERVAL_MS 350
#define WIFI_INPUT_HOLD_START_MS 450
#define WIFI_SAVE_STATUS_DISPLAY_MS 1200

// Wi-Fi settings stored in EEPROM
#define WIFI_EEPROM_SIZE 256
