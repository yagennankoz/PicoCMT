#include "cmt_clock.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <stdlib.h>

#include "cmt_network_config.h"
#include "hardware/watchdog.h"

#ifdef ENABLE_DEBUG_LOG
#define CLOCK_LOG(fmt, ...) Serial.printf("[CLOCK] " fmt "\r\n", ##__VA_ARGS__)
#else
#define CLOCK_LOG(fmt, ...)
#endif

// 時刻が合っていない間にファイルへ付ける日時 (2000-01-01 00:00:00 UTC)
static const time_t CLOCK_PLACEHOLDER_TIME = 946684800;
// これより前の時刻は「まだ合っていない」とみなす (2024-01-01 00:00:00 UTC)。
// 合わせる前の時計は 1970 年から始まり、FAT は 1980 年より前を表せない
static const time_t CLOCK_VALID_AFTER = 1704067200;

bool clock_is_valid() { return time(nullptr) >= CLOCK_VALID_AFTER; }

static time_t file_time_callback() {
    return clock_is_valid() ? time(nullptr) : CLOCK_PLACEHOLDER_TIME;
}

void clock_init() {
    // FAT のタイムスタンプはタイムゾーンを持たないので、その地域の時刻で書く
    setenv("TZ", CLOCK_TIMEZONE, 1);
    tzset();
    SD.setTimeCallback(file_time_callback);
}

static bool resolve_ntp_server(const char* host, IPAddress& addr) {
    if (host == nullptr || host[0] == '\0') {
        return false;
    }
    watchdog_update();
    bool ok = (::hostByName(host, addr, NTP_DNS_TIMEOUT_MS) == 1);
    watchdog_update();
    if (!ok) {
        CLOCK_LOG("DNS lookup failed: %s", host);
    }
    return ok;
}

void clock_start_ntp() {
    // NTP.begin(サーバー名) は DNS の待ち時間を指定できないので、先に自分で引く
    IPAddress primary, secondary;
    bool has_primary = resolve_ntp_server(NTP_SERVER_PRIMARY, primary);
    bool has_secondary = resolve_ntp_server(NTP_SERVER_SECONDARY, secondary);

    if (has_primary && has_secondary) {
        NTP.begin(primary, secondary);
    } else if (has_primary) {
        NTP.begin(primary);
    } else if (has_secondary) {
        NTP.begin(secondary);
    } else {
        CLOCK_LOG("NTP not started (no server found)");
        return;
    }
    CLOCK_LOG("NTP started (%s / %s, TZ=%s)", NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY,
              CLOCK_TIMEZONE);
}
