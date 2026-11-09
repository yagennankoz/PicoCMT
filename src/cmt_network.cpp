#include "cmt_network.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <bearssl/bearssl_hash.h>
#include <stddef.h>
#include <string.h>

#include "cmt_clock.h"
#include "cmt_globals.h"
#include "cmt_network_config.h"
#include "cmt_ui.h"
#include "cmt_web.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
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

char wifi_ssid[33] = WIFI_SSID;
char wifi_password[65] = WIFI_PASSWORD;
char web_password[WEB_PASSWORD_MAX_LEN + 1] = WEB_DEFAULT_PASSWORD;
bool web_auth_enabled = WEB_AUTH_DEFAULT_ENABLED;
int wifi_config_field = 0;
int wifi_config_pos = 0;
int wifi_menu_cursor = 0;
bool wifi_config_apply_requested = false;
bool wifi_connect_requested = false;
bool wifi_disconnect_requested = false;
bool wifi_settings_saved = false;
bool wifi_user_enabled = false;
bool wifi_ui_blink_on = true;
bool wifi_save_result_success = false;
const char* settings_save_result_label = "WiFi settings";
int settings_save_return_cursor = 0;
uint32_t wifi_save_result_until_ms = 0;
int wifi_opt_sel = 1;

bool wifi_connected = false;
bool wifi_connect_failed = false;
WifiFailReason wifi_fail_reason = WIFI_FAIL_NONE;
int wifi_fail_link_status = 0;
// 今回の接続で見えた失敗の状態(パスワード違い、APが見つからない、その他の順に優先して残す)
static int wifi_worst_link = 0;
bool wifi_connect_in_progress = false;
uint32_t wifi_connect_started_ms = 0;

#define WIFI_SCAN_MAX_RESULTS 16
char wifi_scan_ssids[WIFI_SCAN_MAX_RESULTS][33];
int32_t wifi_scan_rssi[WIFI_SCAN_MAX_RESULTS];
int wifi_scan_count = 0;
int wifi_scan_cursor = 0;
bool wifi_scan_requested = false;
bool wifi_scan_in_progress = false;

static uint32_t wifi_scan_started_ms = 0;
static int wifi_join_attempts = 0;
static uint32_t wifi_last_join_attempt_ms = 0;
static bool wifi_reconnecting = false;  // 切断後のつなぎ直し中
static bool wifi_reconnect_pending = false;  // つなぎ直す (再生・録音中は終わってから)
static bool ntp_start_pending = false;       // 接続できたら NTP で時刻を合わせる

// 再生・録音中か。この間は Wi-Fi を切り、接続・スキャン・NTP・Webのリクエストを待たせる
// Wi-Fiを切っておく状態。再生・録音の間と、USBドライブの間(消費電力のため。
// 電源を入れてPCとつながっていたときは、USBドライブにするかを決めるまで)。
// 終わったら設定どおりつなぎ直す
static bool is_tape_busy() {
    return current_state == STATE_PLAYING || current_state == STATE_PLAY_STANDBY ||
           current_state == STATE_RECORDING || current_state == STATE_REC_STANDBY ||
           current_state == STATE_USB_PROMPT || current_state == STATE_USB_STORAGE ||
           usb_startup_pending;
}

RecFormat default_rec_fmt = FMT_T88;
FileFilter file_filter = FILTER_ALL;

// ============================================================================
// 設定の保存 (EEPROM)
//   SYS2: 現在の形式。SYS1 の Wi-Fi 設定は PIN で暗号化されていて読めないので
//         引き継がず、録音フォーマットだけを引き継ぐ
// ============================================================================
static const char SYS_CONFIG_MAGIC[4] = {'S', 'Y', 'S', '2'};
static const char SYS_CONFIG_MAGIC_V1[4] = {'S', 'Y', 'S', '1'};
static const char WIFI_CONFIG_MAGIC_V1[4] = {'W', 'C', 'F', '1'};
static const char WIFI_CONFIG_MAGIC[4] = {'W', 'C', 'F', '2'};
static const char WEB_CONFIG_MAGIC[4] = {'W', 'E', 'B', '1'};

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
    uint8_t auth_enabled;  // 0: 認証なし
    uint8_t pass_len;
    char pass[WEB_PASSWORD_MAX_LEN + 1];
    uint32_t checksum;
} WebConfigPayload;

typedef struct {
    char magic[4];
    WebConfigPayload payload;
} WebConfigEepromStorage;

typedef struct {
    char magic[4];
    uint8_t default_rec_fmt;  // 旧形式 (SYS1) と同じ位置
    WifiConfigEepromStorage wifi;
    WebConfigEepromStorage web;
    // 電源を入れたときに Wi-Fi に自動で接続する (前回 ON だった)。
    // 1 のときだけ ON とみなす (消去直後のフラッシュの 0xFF を ON と読まないため)
    uint8_t wifi_auto_connect;
    // ファイル一覧のフィルタ(FileFilter)。前の版で保存した設定では0か0xFFなので、範囲外はALLにする
    uint8_t file_filter;
} SysConfig;

static_assert(sizeof(SysConfig) <= WIFI_EEPROM_SIZE, "SysConfig does not fit in EEPROM");

// 暗号化の用途 (用途ごとに別の鍵ストリームにする)
enum ConfigDomain : uint8_t { CONFIG_DOMAIN_WIFI = 1, CONFIG_DOMAIN_WEB = 2 };

// 暗号化の鍵。基板固有のIDから作る。
// RP2040 はフラッシュの読み出しを禁止できないため、本体を入手した人には復号できる。
// 中身がそのまま読めない程度の保護。
static uint8_t config_key[32];

static void sha256_bytes(const uint8_t* data, size_t len, uint8_t out[32]) {
    br_sha256_context ctx;
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, data, len);
    br_sha256_out(&ctx, out);
}

static void derive_config_key() {
    static const uint8_t label[] = "PicoCmt-Config-Key-v3";

    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    uint8_t base[PICO_UNIQUE_BOARD_ID_SIZE_BYTES + sizeof(label)];
    memcpy(base, board_id.id, PICO_UNIQUE_BOARD_ID_SIZE_BYTES);
    memcpy(base + PICO_UNIQUE_BOARD_ID_SIZE_BYTES, label, sizeof(label) - 1);
    sha256_bytes(base, PICO_UNIQUE_BOARD_ID_SIZE_BYTES + sizeof(label) - 1, config_key);
}

static void crypt_config_buffer(uint8_t* buf, size_t len, uint8_t domain) {
    uint32_t counter = 0;
    size_t offset = 0;
    uint8_t block_input[37];
    uint8_t stream[32];

    while (offset < len) {
        memcpy(block_input, config_key, 32);
        block_input[32] = domain;
        block_input[33] = (uint8_t)(counter & 0xFFu);
        block_input[34] = (uint8_t)((counter >> 8) & 0xFFu);
        block_input[35] = (uint8_t)((counter >> 16) & 0xFFu);
        block_input[36] = (uint8_t)((counter >> 24) & 0xFFu);
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

static uint32_t fnv1a(const uint8_t* data, size_t len) {
    uint32_t sum = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        sum ^= data[i];
        sum *= 16777619u;
    }
    return sum;
}

// checksum より前の部分のチェックサム
template <typename T>
static uint32_t payload_checksum(const T& p) {
    return fnv1a((const uint8_t*)&p, offsetof(T, checksum));
}

template <typename T>
static void seal_payload(T& p, uint8_t domain) {
    p.checksum = payload_checksum(p);
    crypt_config_buffer((uint8_t*)&p, sizeof(T), domain);
}

template <typename T>
static bool unseal_payload(T& p, uint8_t domain) {
    crypt_config_buffer((uint8_t*)&p, sizeof(T), domain);
    return p.checksum == payload_checksum(p);
}

// 現在の形式で読み込む (形式が違えば初期値)
static void read_config(SysConfig& cfg) {
    EEPROM.get(0, cfg);
    if (memcmp(cfg.magic, SYS_CONFIG_MAGIC, 4) != 0) {
        memset(&cfg, 0, sizeof(cfg));
        memcpy(cfg.magic, SYS_CONFIG_MAGIC, 4);
        cfg.default_rec_fmt = (uint8_t)default_rec_fmt;
    }
}

static bool write_config(const SysConfig& cfg) {
    EEPROM.put(0, cfg);
    return EEPROM.commit();
}

void save_rec_format_to_eeprom() {
    SysConfig cfg;
    read_config(cfg);
    cfg.default_rec_fmt = (uint8_t)default_rec_fmt;
    write_config(cfg);
}

void save_file_filter_to_eeprom() {
    SysConfig cfg;
    read_config(cfg);
    cfg.file_filter = (uint8_t)file_filter;
    write_config(cfg);
}

void clear_wifi_settings_in_eeprom() {
    WIFI_LOG("Clearing WiFi settings in EEPROM...");
    SysConfig cfg;
    read_config(cfg);
    memset(&cfg.wifi, 0, sizeof(cfg.wifi));
    cfg.wifi_auto_connect = 0;
    write_config(cfg);
}

void save_wifi_auto_connect(bool enabled) {
    SysConfig cfg;
    read_config(cfg);
    uint8_t value = enabled ? 1 : 0;
    if (cfg.wifi_auto_connect != value) {
        cfg.wifi_auto_connect = value;
        write_config(cfg);
    }
}

static bool save_wifi_settings_to_flash() {
    WIFI_LOG("Saving settings. SSID: [%s], PassLen: %d", wifi_ssid,
             strlen(wifi_password));

    SysConfig cfg;
    read_config(cfg);
    memcpy(cfg.wifi.magic, WIFI_CONFIG_MAGIC, sizeof(cfg.wifi.magic));

    WifiConfigPayload payload = {};
    strncpy(payload.ssid, wifi_ssid, sizeof(payload.ssid) - 1);
    strncpy(payload.pass, wifi_password, sizeof(payload.pass) - 1);
    payload.ssid_len = (uint8_t)strnlen(payload.ssid, sizeof(payload.ssid) - 1);
    payload.pass_len = (uint8_t)strnlen(payload.pass, sizeof(payload.pass) - 1);
    seal_payload(payload, CONFIG_DOMAIN_WIFI);
    cfg.wifi.payload = payload;

    if (!write_config(cfg)) {
        WIFI_LOG("Save failed.");
        return false;
    }
    wifi_settings_saved = true;
    WIFI_LOG("Save successful.");
    return true;
}

bool load_wifi_settings_from_flash() {
    WIFI_LOG("Loading WiFi settings from EEPROM...");

    SysConfig cfg;
    read_config(cfg);
    if (memcmp(cfg.wifi.magic, WIFI_CONFIG_MAGIC, 4) != 0) {
        WIFI_LOG("No valid WiFi settings found in EEPROM.");
        return false;
    }

    WifiConfigPayload payload = cfg.wifi.payload;
    if (!unseal_payload(payload, CONFIG_DOMAIN_WIFI) ||
        payload.ssid_len >= sizeof(payload.ssid) || payload.pass_len >= sizeof(payload.pass)) {
        WIFI_LOG("WiFi settings are broken.");
        return false;
    }

    memset(wifi_ssid, 0, sizeof(wifi_ssid));
    memset(wifi_password, 0, sizeof(wifi_password));
    memcpy(wifi_ssid, payload.ssid, payload.ssid_len);
    memcpy(wifi_password, payload.pass, payload.pass_len);

    WIFI_LOG("Load successful! SSID: [%s]", wifi_ssid);
    return true;
}

static void load_web_settings_from_flash() {
    SysConfig cfg;
    read_config(cfg);
    if (memcmp(cfg.web.magic, WEB_CONFIG_MAGIC, 4) != 0) {
        return;  // 未設定なら初期値のまま
    }
    WebConfigPayload payload = cfg.web.payload;
    if (!unseal_payload(payload, CONFIG_DOMAIN_WEB) || payload.pass_len == 0 ||
        payload.pass_len > WEB_PASSWORD_MAX_LEN) {
        WIFI_LOG("Web settings are broken. Using the defaults.");
        return;
    }
    web_auth_enabled = (payload.auth_enabled != 0);
    memset(web_password, 0, sizeof(web_password));
    memcpy(web_password, payload.pass, payload.pass_len);
}

static bool save_web_settings(const char* password, bool auth_enabled) {
    SysConfig cfg;
    read_config(cfg);
    memcpy(cfg.web.magic, WEB_CONFIG_MAGIC, sizeof(cfg.web.magic));

    WebConfigPayload payload = {};
    payload.auth_enabled = auth_enabled ? 1 : 0;
    strncpy(payload.pass, password, WEB_PASSWORD_MAX_LEN);
    payload.pass_len = (uint8_t)strnlen(payload.pass, WEB_PASSWORD_MAX_LEN);
    seal_payload(payload, CONFIG_DOMAIN_WEB);
    cfg.web.payload = payload;

    if (!write_config(cfg)) {
        return false;
    }
    web_auth_enabled = auth_enabled;
    if (password != web_password) {
        memset(web_password, 0, sizeof(web_password));
        strncpy(web_password, password, WEB_PASSWORD_MAX_LEN);
    }
    WIFI_LOG("Web settings saved. (auth: %s)", auth_enabled ? "ON" : "OFF");
    return true;
}

bool save_web_password(const char* password) {
    // 空なら初期値に戻す
    const char* value = (password != nullptr && password[0] != '\0') ? password
                                                                      : WEB_DEFAULT_PASSWORD;
    return save_web_settings(value, web_auth_enabled);
}

bool save_web_auth_enabled(bool enabled) { return save_web_settings(web_password, enabled); }

static void start_web_if_ready() {
    if (!wifi_user_enabled || !wifi_connected) {
        return;
    }
    WIFI_LOG("Starting Web Server (ID: %s / PW: %s)", WEB_USERNAME, web_password);
    web_server_start();
}

// 接続の処理で画面が数秒止まるので、その前に「接続中」の表示にしておく
static void show_connecting_now() {
    wifi_ui_blink_on = true;
    oled_need_refresh = true;
    update_oled_display();
}

// 接続(join)を1回試みる
static void start_wifi_join_attempt() {
    wifi_join_attempts++;
    WIFI_LOG("Join attempt %d/%d", wifi_join_attempts, WIFI_JOIN_MAX_ATTEMPTS);
    show_connecting_now();

    WiFi.setTimeout(WIFI_JOIN_ATTEMPT_TIMEOUT_MS);
    // WiFi.begin() は接続(join)が終わるまで戻らないので、その間だけウォッチドッグを延ばす
    watchdog_enable(WIFI_JOIN_WATCHDOG_MS, 0);
    begin_wifi_icon_blink_on_core1();  // 戻るまでの間は Core1 がアイコンを点滅させる
    WiFi.begin(wifi_ssid, wifi_password);
    end_wifi_icon_blink_on_core1();
    watchdog_enable(SYSTEM_WATCHDOG_TIMEOUT_MS, 0);

    wifi_last_join_attempt_ms = millis();
}

static void connect_wifi() {
    WIFI_LOG("Connecting to AP: [%s]", wifi_ssid);

    wifi_connect_in_progress = true;
    wifi_connected = false;
    show_connecting_now();

    begin_wifi_icon_blink_on_core1();
    WiFi.disconnect();
    watchdog_update();
    delay(500);
    end_wifi_icon_blink_on_core1();

    // STA(子機)モードで起動
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("PicoCmt");

    wifi_connect_in_progress = true;
    wifi_connect_started_ms = millis();
    wifi_connected = false;
    wifi_join_attempts = 0;
    wifi_worst_link = 0;

    start_wifi_join_attempt();
}

// 試している間に見えたWi-Fiチップの失敗の状態を覚えておく
static void note_link_failure(int link) {
    auto rank = [](int l) {
        return (l == CYW43_LINK_BADAUTH) ? 3 : (l == CYW43_LINK_NONET) ? 2 : (l < 0) ? 1 : 0;
    };
    if (rank(link) > rank(wifi_worst_link)) {
        wifi_worst_link = link;
    }
}

// 接続をあきらめる
static void fail_wifi_connection(WifiFailReason reason) {
    wifi_fail_reason = reason;
    wifi_fail_link_status = wifi_worst_link;
    WIFI_LOG("WiFi connect failed: reason=%d link=%d", (int)reason, wifi_worst_link);
    wifi_disconnect_requested = true;
    wifi_connect_in_progress = false;
    wifi_connected = false;

    // 接続エラー時はWiFiメニューのトグルもOFFに戻す
    wifi_user_enabled = false;
    wifi_opt_sel = 1;
    oled_need_refresh = true;

    if (wifi_reconnecting) {
        // 切断後のつなぎ直しに失敗した場合は、エラー画面は出さずにOFFに戻す
        WIFI_LOG("Reconnect failed. WiFi turned OFF.");
        wifi_reconnecting = false;
        return;
    }

    wifi_connect_failed = true;
    current_state = STATE_WIFI_CONNECT_ERROR;
    reset_and_ignore_pressed_buttons();
}

static void refresh_wifi_connection_state(bool tape_busy) {
#ifdef ENABLE_DEBUG_LOG
    static int last_status = -1;
    int current_status = WiFi.status();
    if (current_status != last_status) {
        // link: Wi-Fi チップの状態 (0=DOWN 1=JOIN 2=NOIP 3=UP -1=FAIL -2=NONET -3=BADAUTH)
        WIFI_LOG("WiFi Status changed: %s (link=%d, t=%lums)",
                 get_wifi_status_str(current_status),
                 cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA),
                 (unsigned long)millis());
        last_status = current_status;
    }
#endif

    bool connected_now = (WiFi.status() == WL_CONNECTED);

    if (wifi_connect_in_progress) {
        uint32_t now_ms = millis();
        bool timed_out = (now_ms - wifi_connect_started_ms) >= WIFI_CONNECT_TIMEOUT_MS;
        int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        note_link_failure(link);

        if (connected_now) {
            WIFI_LOG("WiFi Connection SUCCESS! IP Address: %s",
                     WiFi.localIP().toString().c_str());
            wifi_connect_in_progress = false;
            wifi_reconnecting = false;
            wifi_connected = true;
            web_server_stop();
            oled_need_refresh = true;
            start_web_if_ready();
            ntp_start_pending = true;
        } else if (link == CYW43_LINK_BADAUTH) {
            // パスワード違いは、やり直しても成功しないのですぐにあきらめる
            WIFI_LOG("WiFi authentication failed (wrong password?).");
            fail_wifi_connection(WIFI_FAIL_BAD_PASSWORD);
        } else if (link == CYW43_LINK_JOIN) {
            // APには接続済みで、DHCPでIPアドレスを受け取るのを待っている
            if (timed_out) {
                WIFI_LOG("WiFi DHCP TIMEOUT. Aborting...");
                fail_wifi_connection(WIFI_FAIL_NO_IP);
            }
        } else if ((now_ms - wifi_last_join_attempt_ms) >= WIFI_JOIN_RETRY_INTERVAL_MS) {
            // 接続(join)に失敗している。回数と時間が残っていればやり直す
            if (wifi_join_attempts < WIFI_JOIN_MAX_ATTEMPTS && !timed_out) {
                start_wifi_join_attempt();
            } else {
                WIFI_LOG("WiFi Connection failed after %d attempts. Aborting...",
                         wifi_join_attempts);
                // 途中で見えた状態から理由を決める
                fail_wifi_connection((wifi_worst_link == CYW43_LINK_BADAUTH) ? WIFI_FAIL_BAD_PASSWORD
                                     : (wifi_worst_link == CYW43_LINK_NONET) ? WIFI_FAIL_AP_NOT_FOUND
                                                                             : WIFI_FAIL_JOIN);
            }
        }
    }

    // 再生・録音のために切った後は読み直さない (切った直後は WL_CONNECTED が残ることがある)。
    // つなぎ直しは下で行う
    if (!tape_busy && !wifi_reconnect_pending && connected_now != wifi_connected) {
        wifi_connected = connected_now;
        if (!wifi_connected) {
            WIFI_LOG("WiFi Disconnected unexpectedly.");
            web_server_stop();

            // 接続が意図せず切れた場合は、つなぎ直しを試みる (失敗したらOFFに戻す)
            if (wifi_user_enabled && !wifi_connect_in_progress) {
                wifi_reconnect_pending = true;
            }
        }
        oled_need_refresh = true;
    }

    if (wifi_reconnect_pending && !tape_busy) {
        wifi_reconnect_pending = false;
        if (wifi_user_enabled && !wifi_connected && !wifi_connect_in_progress) {
            WIFI_LOG("Reconnecting...");
            wifi_reconnecting = true;
            connect_wifi();
        }
    }
}

static void perform_wifi_scan_if_requested(bool tape_busy) {
    if (wifi_scan_requested && !wifi_scan_in_progress && !tape_busy) {
        wifi_scan_requested = false;
        wifi_scan_in_progress = true;
        wifi_scan_count = 0;
        wifi_scan_cursor = 0;
        WIFI_LOG("Starting WiFi Scan...");

        // 完了まで待つとウォッチドッグが切れるので、非同期で開始して完了を待つ
        WiFi.scanNetworks(true);
        wifi_scan_started_ms = millis();
        oled_need_refresh = true;
        return;
    }
    if (!wifi_scan_in_progress) {
        return;
    }

    int found = WiFi.scanComplete();
    if (found < 0) {
        if ((millis() - wifi_scan_started_ms) < WIFI_SCAN_TIMEOUT_MS) {
            return;  // スキャン中
        }
        WIFI_LOG("Scan TIMEOUT.");
        found = 0;
    }

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
    wifi_save_result_success = save_wifi_settings_to_flash();
    settings_save_result_label = "WiFi settings";
    settings_save_return_cursor = SET_ROW_WIFI;
    wifi_save_result_until_ms = millis() + WIFI_SAVE_STATUS_DISPLAY_MS;
    current_state = STATE_WIFI_SAVE_RESULT;
    reset_and_ignore_pressed_buttons();
    oled_need_refresh = true;
}

static void apply_wifi_connect_if_requested() {
    if (!wifi_connect_requested) {
        return;
    }

    if (current_state != STATE_WIFI_MENU || !wifi_user_enabled) {
        wifi_connect_requested = false;
        return;
    }

    wifi_connect_requested = false;

    if (!wifi_settings_saved) {
        wifi_user_enabled = false;
        wifi_opt_sel = 1;
        oled_need_refresh = true;
        return;
    }

    wifi_connected = false;
    web_server_stop();
    wifi_connect_failed = false;

    // 設定画面で編集途中の内容が残っていても、保存されている設定でつなぐ
    bool load_wifi_settings_ok = load_wifi_settings_from_flash();
    if (!load_wifi_settings_ok) {
        wifi_user_enabled = false;
        wifi_opt_sel = 1;
        wifi_connect_failed = true;
        wifi_fail_reason = WIFI_FAIL_SETTINGS;
        wifi_fail_link_status = 0;
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
    wifi_reconnecting = false;
    wifi_reconnect_pending = false;
    web_server_stop();
    oled_need_refresh = true;
}

// 再生・録音の間だけ Wi-Fi を切る。メニューの ON/OFF は変えず、
// 終わったら refresh_wifi_connection_state() がつなぎ直す (失敗したら黙って OFF に戻す)
static void suspend_wifi_for_tape() {
    WIFI_LOG("Tape busy: WiFi suspended.");
    web_server_stop();
    WiFi.disconnect();
    wifi_connected = false;
    wifi_connect_in_progress = false;
    wifi_reconnecting = false;
    wifi_reconnect_pending = wifi_user_enabled;
    oled_need_refresh = true;
}

void init_wifi_and_settings() {
    EEPROM.begin(WIFI_EEPROM_SIZE);
    derive_config_key();

    SysConfig cfg;
    EEPROM.get(0, cfg);

    if (memcmp(cfg.magic, SYS_CONFIG_MAGIC, 4) != 0) {
        bool is_v1 = (memcmp(cfg.magic, SYS_CONFIG_MAGIC_V1, 4) == 0);
        bool is_old = is_v1 || (memcmp(cfg.magic, WIFI_CONFIG_MAGIC_V1, 4) == 0);
        uint8_t fmt = is_v1 ? cfg.default_rec_fmt : (uint8_t)FMT_T88;

        memset(&cfg, 0, sizeof(cfg));
        memcpy(cfg.magic, SYS_CONFIG_MAGIC, 4);
        cfg.default_rec_fmt = fmt;
        if (is_old) {
            // 旧形式の Wi-Fi 設定は PIN で暗号化されていて読めないので、設定し直してもらう
            WIFI_LOG("Old settings found. WiFi settings must be set up again.");
            write_config(cfg);
        }
    }

    default_rec_fmt = (cfg.default_rec_fmt < FMT_MAX) ? (RecFormat)cfg.default_rec_fmt : FMT_T88;
    file_filter = (cfg.file_filter < FILTER_MAX) ? (FileFilter)cfg.file_filter : FILTER_ALL;
    wifi_settings_saved = load_wifi_settings_from_flash();
    load_web_settings_from_flash();

    // 前回 Wi-Fi が ON だったら自動で接続する。接続は loop() の中で始め
    // (起動を遅らせない)、つながらなければ切断時のつなぎ直しと同じく黙って OFF に戻す
    if (wifi_settings_saved && cfg.wifi_auto_connect == 1) {
        WIFI_LOG("Auto connect is enabled.");
        wifi_user_enabled = true;
        wifi_opt_sel = 2;
        wifi_reconnect_pending = true;
    }
}

void handle_wifi_background_tasks() {
    bool tape_busy = is_tape_busy();

    // 再生・録音の間は Wi-Fi を切り、終わったらつなぎ直す。
    // Wi-Fi の送信で電源が揺れ、USB ハブなど容量の小さい電源では
    // 再生の信号が乱れて読み込みエラーになった (実機の PC-8801 で確認)
    if (tape_busy && (wifi_connected || wifi_connect_in_progress)) {
        suspend_wifi_for_tape();
    }

    apply_wifi_settings_if_requested();
    apply_wifi_connect_if_requested();
    apply_wifi_disconnect_if_requested();

    if (wifi_connect_failed && current_state != STATE_WIFI_CONNECT_ERROR && !tape_busy) {
        current_state = STATE_WIFI_CONNECT_ERROR;
        oled_need_refresh = true;
    }

    static uint32_t last_blink_ms = 0;
    uint32_t now_ms = millis();
    if ((current_state == STATE_WIFI_CONFIG ||
         current_state == STATE_WEB_PASS_CONFIG ||
         current_state == STATE_WIFI_MENU ||
         (current_state == STATE_IDLE && wifi_connect_in_progress)) &&
        (now_ms - last_blink_ms) >= WIFI_UI_BLINK_INTERVAL_MS) {
        last_blink_ms = now_ms;
        wifi_ui_blink_on = !wifi_ui_blink_on;
        oled_need_refresh = true;
    }

    refresh_wifi_connection_state(tape_busy);
    perform_wifi_scan_if_requested(tape_busy);

    // NTP の開始は DNS で数秒止まることがあるので、再生・録音中は待たせる
    if (ntp_start_pending && wifi_connected && !tape_busy) {
        ntp_start_pending = false;
        clock_start_ntp();
    }

    // 再生・録音中は Wi-Fi を切ってあるので、Web サーバーも止まっている
    web_server_handle(!tape_busy);
}