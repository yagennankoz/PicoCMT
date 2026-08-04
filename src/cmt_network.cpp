#include "cmt_network.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <SimpleFTPServer.h>
#include <WiFi.h>
#include <bearssl/bearssl_hash.h>
#include <string.h>

#include "cmt_globals.h"
#include "cmt_network_config.h"
#include "cmt_ui.h"
#include "pico/unique_id.h"

#ifdef ENABLE_DEBUG_LOG
#define WIFI_LOG(fmt, ...) Serial.printf("[WIFI] " fmt "\r\n", ##__VA_ARGS__)
static const char* get_wifi_status_str(int status) {
    switch (status) {
        case WL_IDLE_STATUS:
            return "IDLE_STATUS";
        case WL_NO_SSID_AVAIL:
            return "NO_SSID_AVAIL (AP Not Found)";
        case WL_SCAN_COMPLETED:
            return "SCAN_COMPLETED";
        case WL_CONNECTED:
            return "CONNECTED";
        case WL_CONNECT_FAILED:
            return "CONNECT_FAILED (Wrong Password / Handshake Timeout)";
        case WL_CONNECTION_LOST:
            return "CONNECTION_LOST";
        case WL_DISCONNECTED:
            return "DISCONNECTED";
        default:
            return "UNKNOWN";
    }
}
#else
#define WIFI_LOG(fmt, ...)
#endif

FtpServer ftp_server;

char wifi_ssid[33] = WIFI_SSID;
char wifi_password[65] = WIFI_PASSWORD;
char wifi_pin[5] = "0000";
uint8_t wifi_session_key[32] = {0};
bool wifi_session_key_valid = false;
int wifi_config_field = 0;
int wifi_config_pos = 0;
int wifi_menu_cursor = 0;
bool wifi_config_apply_requested = false;
bool wifi_connect_with_pin_requested = false;
bool wifi_disconnect_requested = false;
bool wifi_settings_saved = false;
bool wifi_user_enabled = false;
bool wifi_ui_blink_on = true;
bool wifi_save_result_success = false;
bool wifi_settings_pin_requested = false;
uint32_t wifi_save_result_until_ms = 0;
int wifi_opt_sel = 1;

bool wifi_connected = false;
bool ftp_server_running = false;
bool wifi_connect_failed = false;
bool wifi_connect_in_progress = false;
uint32_t wifi_connect_started_ms = 0;

#define WIFI_SCAN_MAX_RESULTS 16
char wifi_scan_ssids[WIFI_SCAN_MAX_RESULTS][33];
int32_t wifi_scan_rssi[WIFI_SCAN_MAX_RESULTS];
int wifi_scan_count = 0;
int wifi_scan_cursor = 0;
bool wifi_scan_requested = false;
bool wifi_scan_in_progress = false;
int wifi_pin_failed_attempts = 0;

static const char WIFI_CONFIG_MAGIC[4] = {'W', 'C', 'F', '1'};
static const char SYS_CONFIG_MAGIC[4] = {'S', 'Y', 'S', '1'};
static const uint32_t WIFI_KDF_ROUNDS = 20000;

RecFormat default_rec_fmt = FMT_T88;

typedef struct {
    uint8_t ssid_len;
    uint8_t pass_len;
    char ssid[33];
    char pass[65];
    uint32_t checksum;
} WifiConfigPayload;

typedef struct {
    char magic[4];
    WifiConfigPayload payload;
} WifiConfigEepromStorage;

typedef struct {
    char magic[4];
    uint8_t default_rec_fmt;
    WifiConfigEepromStorage wifi;
} SysConfig;

static void sha256_bytes(const uint8_t* data, size_t len, uint8_t out[32]) {
    br_sha256_context ctx;
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, data, len);
    br_sha256_out(&ctx, out);
}

static void derive_wifi_crypto_key(const char pin[5], uint8_t key_out[32]) {
    static const uint8_t kdf_label[] = "PicoCmt-WiFiCfg-KDF-v2";

    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    uint8_t base[64] = {0};
    size_t pos = 0;
    memcpy(base + pos, board_id.id, PICO_UNIQUE_BOARD_ID_SIZE_BYTES);
    pos += PICO_UNIQUE_BOARD_ID_SIZE_BYTES;
    memcpy(base + pos, kdf_label, sizeof(kdf_label) - 1);
    pos += sizeof(kdf_label) - 1;
    memcpy(base + pos, pin, 4);
    pos += 4;

    uint8_t digest[32] = {0};
    sha256_bytes(base, pos, digest);

    uint8_t mix[96] = {0};
    for (uint32_t i = 0; i < WIFI_KDF_ROUNDS; i++) {
        memcpy(mix, digest, 32);
        memcpy(mix + 32, base, pos);
        mix[32 + pos + 0] = (uint8_t)(i & 0xFFu);
        mix[32 + pos + 1] = (uint8_t)((i >> 8) & 0xFFu);
        mix[32 + pos + 2] = (uint8_t)((i >> 16) & 0xFFu);
        mix[32 + pos + 3] = (uint8_t)((i >> 24) & 0xFFu);
        sha256_bytes(mix, 32 + pos + 4, digest);
    }

    memcpy(key_out, digest, 32);
}

void refresh_wifi_session_key_from_pin() {
    char pin_for_kdf[5] = {0};
    strncpy(pin_for_kdf, wifi_pin, sizeof(pin_for_kdf) - 1);
    WIFI_LOG("Deriving session key from PIN: [%s]", pin_for_kdf);
    derive_wifi_crypto_key(pin_for_kdf, wifi_session_key);
    wifi_session_key_valid = true;
}

static uint32_t calc_wifi_payload_checksum(const WifiConfigPayload& p) {
    uint32_t sum = 2166136261u;
    sum ^= p.ssid_len;
    sum *= 16777619u;
    sum ^= p.pass_len;
    sum *= 16777619u;
    for (int i = 0; i < (int)sizeof(p.ssid); i++) {
        sum ^= (uint8_t)p.ssid[i];
        sum *= 16777619u;
    }
    for (int i = 0; i < (int)sizeof(p.pass); i++) {
        sum ^= (uint8_t)p.pass[i];
        sum *= 16777619u;
    }
    return sum;
}

static void crypt_wifi_buffer(uint8_t* buf, size_t len, const uint8_t key[32]) {
    uint32_t counter = 0;
    size_t offset = 0;
    uint8_t block_input[36];
    uint8_t stream[32];

    while (offset < len) {
        memcpy(block_input, key, 32);
        block_input[32] = (uint8_t)(counter & 0xFFu);
        block_input[33] = (uint8_t)((counter >> 8) & 0xFFu);
        block_input[34] = (uint8_t)((counter >> 16) & 0xFFu);
        block_input[35] = (uint8_t)((counter >> 24) & 0xFFu);
        sha256_bytes(block_input, sizeof(block_input), stream);

        size_t chunk =
            (len - offset > sizeof(stream)) ? sizeof(stream) : (len - offset);
        for (size_t i = 0; i < chunk; i++) {
            buf[offset + i] ^= stream[i];
        }

        offset += chunk;
        counter++;
    }
}

void save_rec_format_to_eeprom() {
    SysConfig cfg;
    EEPROM.get(0, cfg);
    memcpy(cfg.magic, SYS_CONFIG_MAGIC, 4);
    cfg.default_rec_fmt = (uint8_t)default_rec_fmt;
    EEPROM.put(0, cfg);
    EEPROM.commit();
}

void clear_wifi_settings_in_eeprom() {
    WIFI_LOG("Clearing WiFi settings in EEPROM...");
    SysConfig cfg;
    EEPROM.get(0, cfg);
    memset(&cfg.wifi, 0, sizeof(cfg.wifi));
    EEPROM.put(0, cfg);
    EEPROM.commit();
}

static bool save_wifi_settings_to_flash() {
    if (!wifi_session_key_valid) {
        return false;
    }

    WIFI_LOG("Saving settings. SSID: [%s], PassLen: %d", wifi_ssid,
             strlen(wifi_password));

    SysConfig cfg;
    EEPROM.get(0, cfg);
    memcpy(cfg.magic, SYS_CONFIG_MAGIC, 4);
    cfg.default_rec_fmt = (uint8_t)default_rec_fmt;

    memcpy(cfg.wifi.magic, WIFI_CONFIG_MAGIC, sizeof(cfg.wifi.magic));

    WifiConfigPayload payload = {};
    strncpy(payload.ssid, wifi_ssid, sizeof(payload.ssid) - 1);
    strncpy(payload.pass, wifi_password, sizeof(payload.pass) - 1);
    payload.ssid_len = (uint8_t)strnlen(payload.ssid, sizeof(payload.ssid) - 1);
    payload.pass_len = (uint8_t)strnlen(payload.pass, sizeof(payload.pass) - 1);
    payload.checksum = calc_wifi_payload_checksum(payload);

    uint8_t enc_buf[sizeof(WifiConfigPayload)];
    memcpy(enc_buf, &payload, sizeof(payload));
    crypt_wifi_buffer(enc_buf, sizeof(enc_buf), wifi_session_key);

    memcpy(&cfg.wifi.payload, enc_buf, sizeof(cfg.wifi.payload));

    EEPROM.put(0, cfg);
    EEPROM.commit();
    wifi_settings_saved = true;
    WIFI_LOG("Save successful.");
    return true;
}

static void handle_wifi_decrypt_failure() {
    wifi_pin_failed_attempts++;
    WIFI_LOG("Decrypt Failure! Attempt: %d/%d", wifi_pin_failed_attempts,
             WIFI_PIN_MAX_ATTEMPTS);
    if (wifi_pin_failed_attempts >= WIFI_PIN_MAX_ATTEMPTS) {
        WIFI_LOG(
            "Max attempts reached. Auto-clearing WiFi settings to protect "
            "security.");
        memset(wifi_password, 0, sizeof(wifi_password));
        clear_wifi_settings_in_eeprom();
        wifi_settings_saved = false;
        wifi_user_enabled = false;
        wifi_opt_sel = 1;  // メニュー表示もOFFに戻す
        wifi_pin_failed_attempts = 0;
    }
}

static bool is_wifi_settings_saved_in_eeprom() {
    SysConfig cfg;
    EEPROM.get(0, cfg);
    return memcmp(cfg.wifi.magic, WIFI_CONFIG_MAGIC, 4) == 0;
}

bool load_wifi_settings_from_flash() {
    WIFI_LOG("Loading WiFi settings from EEPROM...");
    if (!wifi_session_key_valid) {
        return false;
    }

    SysConfig cfg;
    EEPROM.get(0, cfg);

    if (memcmp(cfg.wifi.magic, WIFI_CONFIG_MAGIC, 4) != 0) {
        WIFI_LOG("No valid WiFi settings found in EEPROM.");
        return false;
    }

    wifi_settings_saved = true;

    uint8_t enc_buf[sizeof(WifiConfigPayload)] = {0};
    memcpy(enc_buf, &cfg.wifi.payload, sizeof(enc_buf));

    crypt_wifi_buffer(enc_buf, sizeof(enc_buf), wifi_session_key);

    WifiConfigPayload payload = {};
    memcpy(&payload, enc_buf, sizeof(payload));

    if (payload.ssid_len >= sizeof(payload.ssid)) {
        WIFI_LOG("Payload error: SSID length out of bounds.");
        handle_wifi_decrypt_failure();
        return false;
    }
    if (payload.pass_len >= sizeof(payload.pass)) {
        WIFI_LOG("Payload error: PASS length out of bounds.");
        handle_wifi_decrypt_failure();
        return false;
    }

    uint32_t calc_cs = calc_wifi_payload_checksum(payload);
    if (payload.checksum != calc_cs) {
        WIFI_LOG("Checksum mismatch! Expected: %08X, Calc: %08X (Wrong PIN?)",
                 payload.checksum, calc_cs);
        handle_wifi_decrypt_failure();
        return false;
    }

    wifi_pin_failed_attempts = 0;

    if (payload.ssid_len > 0) {
        strncpy(wifi_ssid, payload.ssid, sizeof(wifi_ssid) - 1);
        wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
    }
    strncpy(wifi_password, payload.pass, sizeof(wifi_password) - 1);
    wifi_password[sizeof(wifi_password) - 1] = '\0';

    WIFI_LOG("Load successful! SSID: [%s]", wifi_ssid);
    return true;
}

static void start_ftp_if_ready() {
    if (ftp_server_running) {
        return;
    }
    if (!wifi_user_enabled) {
        return;
    }
    if (!wifi_connected) {
        return;
    }
    if (!sd_card_mounted) {
        return;
    }

    WIFI_LOG("Starting FTP Server (ID: %s / PW: %s)", FTP_USERNAME,
             FTP_PASSWORD);
    ftp_server.begin(FTP_USERNAME, FTP_PASSWORD);
    ftp_server.setLocalIp(WiFi.localIP());
    ftp_server_running = true;
}

static void connect_wifi() {
    WIFI_LOG("Connecting to AP: [%s]", wifi_ssid);

    WiFi.disconnect();
    watchdog_update();
    delay(500);

    // STA(子機)モードで起動
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("PicoCmt");

    WiFi.begin(wifi_ssid, wifi_password);

    wifi_connect_in_progress = true;
    wifi_connect_started_ms = millis();
    wifi_connected = false;
}

static void refresh_wifi_connection_state() {
#ifdef ENABLE_DEBUG_LOG
    static int last_status = -1;
    int current_status = WiFi.status();
    if (current_status != last_status) {
        WIFI_LOG("WiFi Status changed: %s",
                 get_wifi_status_str(current_status));
        last_status = current_status;
    }
#endif

    bool connected_now = (WiFi.status() == WL_CONNECTED);

    if (wifi_connect_in_progress) {
        if (connected_now) {
            WIFI_LOG("WiFi Connection SUCCESS! IP Address: %s",
                     WiFi.localIP().toString().c_str());
            wifi_connect_in_progress = false;
            wifi_connected = true;
            ftp_server_running = false;
            oled_need_refresh = true;
            start_ftp_if_ready();
        } else if ((millis() - wifi_connect_started_ms) >= 30000) {
            WIFI_LOG("WiFi Connection TIMEOUT (30s). Aborting...");
            wifi_disconnect_requested = true;
            wifi_connect_in_progress = false;
            wifi_connected = false;
            wifi_connect_failed = true;

            // 接続エラー時はWiFiメニューのトグルもOFFに戻す
            wifi_user_enabled = false;
            wifi_opt_sel = 1;

            current_state = STATE_WIFI_CONNECT_ERROR;
            reset_and_ignore_pressed_buttons();
            oled_need_refresh = true;
        }
    }

    if (connected_now != wifi_connected) {
        wifi_connected = connected_now;
        if (!wifi_connected) {
            WIFI_LOG("WiFi Disconnected unexpectedly.");
            ftp_server_running = false;

            // 接続が意図せず切れた場合も、UIの整合性を保つためOFFに戻すが、
            // 「接続作業中」は一時的な切断ステータスが出やすいためONを維持する
            if (wifi_user_enabled && !wifi_connect_in_progress) {
                wifi_user_enabled = false;
                wifi_opt_sel = 1;
                wifi_disconnect_requested = true;
            }
        }
        oled_need_refresh = true;
    }
}

static void perform_wifi_scan_if_requested() {
    if (!wifi_scan_requested || wifi_scan_in_progress) {
        return;
    }

    wifi_scan_requested = false;
    wifi_scan_in_progress = true;
    wifi_scan_count = 0;
    wifi_scan_cursor = 0;
    WIFI_LOG("Starting WiFi Scan...");

    int found = WiFi.scanNetworks();
    if (found > 0) {
        WIFI_LOG("Scan complete. Found %d networks.", found);
        int limit =
            (found < WIFI_SCAN_MAX_RESULTS) ? found : WIFI_SCAN_MAX_RESULTS;
        int unique_count = 0;

        for (int i = 0; i < limit && unique_count < WIFI_SCAN_MAX_RESULTS;
             i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) {
                continue;
            }

            bool already_exists = false;
            for (int j = 0; j < unique_count; j++) {
                if (strcmp(wifi_scan_ssids[j], ssid.c_str()) == 0) {
                    already_exists = true;
                    if (WiFi.RSSI(i) > wifi_scan_rssi[j]) {
                        wifi_scan_rssi[j] = WiFi.RSSI(i);
                    }
                    break;
                }
            }

            if (already_exists) {
                continue;
            }

            ssid.toCharArray(wifi_scan_ssids[unique_count],
                             sizeof(wifi_scan_ssids[unique_count]));
            wifi_scan_rssi[unique_count] = WiFi.RSSI(i);
            unique_count++;
        }

        for (int i = 0; i < unique_count - 1; i++) {
            for (int j = i + 1; j < unique_count; j++) {
                if (wifi_scan_rssi[j] > wifi_scan_rssi[i]) {
                    int32_t tmp_rssi = wifi_scan_rssi[i];
                    wifi_scan_rssi[i] = wifi_scan_rssi[j];
                    wifi_scan_rssi[j] = tmp_rssi;

                    char tmp_ssid[33];
                    snprintf(tmp_ssid, sizeof(tmp_ssid), "%s",
                             wifi_scan_ssids[i]);
                    snprintf(wifi_scan_ssids[i], sizeof(wifi_scan_ssids[i]),
                             "%s", wifi_scan_ssids[j]);
                    snprintf(wifi_scan_ssids[j], sizeof(wifi_scan_ssids[j]),
                             "%s", tmp_ssid);
                }
            }
        }

        wifi_scan_count = unique_count;
        if (wifi_scan_cursor >= wifi_scan_count) {
            wifi_scan_cursor =
                (wifi_scan_count > 0) ? (wifi_scan_count - 1) : 0;
        }
    } else {
        WIFI_LOG("Scan complete. No networks found.");
        wifi_scan_count = 0;
        wifi_scan_cursor = 0;
    }

    WiFi.scanDelete();
    wifi_scan_in_progress = false;
    oled_need_refresh = true;
}

static void apply_wifi_settings_if_requested() {
    if (!wifi_config_apply_requested) {
        return;
    }
    wifi_config_apply_requested = false;
    wifi_user_enabled = false;
    wifi_opt_sel = 1;  // メニューをOFF状態に戻す
    wifi_disconnect_requested = true;
    refresh_wifi_session_key_from_pin();
    wifi_save_result_success = save_wifi_settings_to_flash();
    wifi_save_result_until_ms = millis() + WIFI_SAVE_STATUS_DISPLAY_MS;
    current_state = STATE_WIFI_SAVE_RESULT;
    reset_and_ignore_pressed_buttons();
    oled_need_refresh = true;
}

static void apply_wifi_connect_with_pin_if_requested() {
    if (!wifi_connect_with_pin_requested) {
        return;
    }

    if (current_state != STATE_WIFI_MENU || !wifi_user_enabled) {
        wifi_connect_with_pin_requested = false;
        return;
    }

    wifi_connect_with_pin_requested = false;

    if (!wifi_settings_saved) {
        wifi_user_enabled = false;
        wifi_opt_sel = 1;
        oled_need_refresh = true;
        return;
    }

    wifi_connected = false;
    ftp_server_running = false;
    wifi_connect_failed = false;

    memset(wifi_ssid, 0, sizeof(wifi_ssid));
    memset(wifi_password, 0, sizeof(wifi_password));

    refresh_wifi_session_key_from_pin();
    bool load_wifi_settings_ok = load_wifi_settings_from_flash();
    if (!load_wifi_settings_ok) {
        wifi_user_enabled = false;
        wifi_opt_sel = 1;
        wifi_connect_failed = true;
        current_state = STATE_WIFI_CONNECT_ERROR;
        reset_and_ignore_pressed_buttons();
        oled_need_refresh = true;
        return;
    }

    wifi_connect_failed = false;
    connect_wifi();
    oled_need_refresh = true;
}

static void apply_wifi_disconnect_if_requested() {
    if (!wifi_disconnect_requested) {
        return;
    }
    WIFI_LOG("Disconnecting WiFi.");
    wifi_disconnect_requested = false;
    WiFi.disconnect();
    wifi_connected = false;
    wifi_connect_in_progress = false;
    ftp_server_running = false;
    oled_need_refresh = true;
}

void init_wifi_and_settings() {
    EEPROM.begin(WIFI_EEPROM_SIZE);

    SysConfig cfg;
    EEPROM.get(0, cfg);

    if (memcmp(cfg.magic, SYS_CONFIG_MAGIC, 4) == 0) {
        default_rec_fmt = (RecFormat)cfg.default_rec_fmt;
        if (default_rec_fmt >= FMT_MAX) {
            default_rec_fmt = FMT_T88;
        }
    } else {
        WifiConfigEepromStorage old_wifi;
        EEPROM.get(0, old_wifi);
        if (memcmp(old_wifi.magic, WIFI_CONFIG_MAGIC, 4) == 0) {
            memcpy(cfg.magic, SYS_CONFIG_MAGIC, 4);
            cfg.default_rec_fmt = (uint8_t)FMT_T88;
            cfg.wifi = old_wifi;
            EEPROM.put(0, cfg);
            EEPROM.commit();
        }
        default_rec_fmt = FMT_T88;
    }

    wifi_settings_saved = is_wifi_settings_saved_in_eeprom();
}

void handle_wifi_background_tasks() {
    apply_wifi_settings_if_requested();
    apply_wifi_connect_with_pin_if_requested();
    apply_wifi_disconnect_if_requested();

    if (wifi_connect_failed && current_state != STATE_WIFI_CONNECT_ERROR) {
        current_state = STATE_WIFI_CONNECT_ERROR;
        oled_need_refresh = true;
    }

    static uint32_t last_blink_ms = 0;
    uint32_t now_ms = millis();
    if ((current_state == STATE_WIFI_CONFIG ||
         current_state == STATE_WIFI_PIN_CONNECT ||
         current_state == STATE_WIFI_MENU) &&
        (now_ms - last_blink_ms) >= WIFI_UI_BLINK_INTERVAL_MS) {
        last_blink_ms = now_ms;
        wifi_ui_blink_on = !wifi_ui_blink_on;
        oled_need_refresh = true;
    }

    refresh_wifi_connection_state();
    perform_wifi_scan_if_requested();

    if (ftp_server_running && current_state != STATE_PLAYING &&
        current_state != STATE_RECORDING) {
        ftp_server.handleFTP();
    }

    if (current_state == STATE_PLAYING || current_state == STATE_PLAY_STANDBY) {
        if (wifi_connected || wifi_connect_in_progress) {
            WiFi.disconnect();
            wifi_connected = false;
            wifi_connect_in_progress = false;

            // 再生・録音開始で切断された場合もメニューをOFFにする
            if (wifi_user_enabled) {
                wifi_user_enabled = false;
                wifi_opt_sel = 1;
            }
        }
        ftp_server_running = false;
    }
}