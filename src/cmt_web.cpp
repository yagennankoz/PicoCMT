#include "cmt_web.h"

#include <Arduino.h>
#include <LEAmDNS.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

#include "cmt_globals.h"
#include "cmt_network_config.h"
#include "cmt_web_page.h"
#include "cmt_web_util.h"
#include "hardware/watchdog.h"

#ifdef ENABLE_DEBUG_LOG
#define WEB_LOG(fmt, ...) Serial.printf("[WEB] " fmt "\r\n", ##__VA_ARGS__)
#else
#define WEB_LOG(fmt, ...)
#endif

#define WEB_PATH_MAX 256
#define WEB_SEND_CHUNK 1024
#define WEB_SEND_STALL_TIMEOUT_MS 5000

static WebServer server(WEB_SERVER_PORT);
static bool server_running = false;
static bool mdns_running = false;
static uint32_t mdns_last_announce_ms = 0;
static bool routes_registered = false;

// アップロード中の状態 (WebServer は1つずつしか処理しないので1組で足りる)
static File upload_file;
static bool upload_started = false;
static bool upload_temp_created = false;
static int upload_error = 0;  // 0 = 正常、それ以外は返す HTTP ステータス
static const char* upload_error_msg = "";
static char upload_temp_path[WEB_PATH_MAX];
static char upload_final_path[WEB_PATH_MAX];

static char json_buf[WEB_PATH_MAX * 6 + 64];

// ----------------------------------------------------------------------------
// 共通処理
// ----------------------------------------------------------------------------
static bool check_auth() {
    if (!web_auth_enabled) {
        return true;  // 認証 OFF
    }
    if (!server.authenticate(WEB_USERNAME, web_password)) {
        server.requestAuthentication(BASIC_AUTH, "PicoCmt");
        return false;
    }
    return true;
}

static void send_text(int code, const char* msg) {
    server.send(code, "text/plain; charset=utf-8", msg);
}

static bool storage_ready() {
    if (!sd_card_mounted) {
        send_text(503, "SD card is not ready");
        return false;
    }
    return true;
}

// 引数をパスとして取り出して正規化する
static bool get_path_arg(const char* key, char* out) {
    if (!server.hasArg(key)) {
        return false;
    }
    return web_normalize_path(server.arg(key).c_str(), out, WEB_PATH_MAX);
}

// 本体のファイルブラウザが開いているフォルダが消えたら、ルートに戻す
static void on_folder_removed(const char* path) {
    if (web_path_is_within(current_path.c_str(), path)) {
        current_path = "/";
        current_cursor = 0;
        scroll_offset = 0;
        oled_need_refresh = true;
    }
}

static bool is_hidden_entry(const char* name) {
    return name[0] == '.' || strcasecmp(name, "System Volume Information") == 0;
}

// ----------------------------------------------------------------------------
// GET /
// ----------------------------------------------------------------------------
static void handle_root() {
    if (!check_auth()) {
        return;
    }
    server.send(200, "text/html; charset=utf-8", WEB_INDEX_HTML);
}

// ----------------------------------------------------------------------------
// GET /api/list?path=<フォルダ>
//   {"path":"/x","entries":[{"name":"A.T88","dir":false,"size":123,"mtime":1700000000}, ...]}
//   mtime は更新日時 (UNIX時刻)
// ----------------------------------------------------------------------------
static void send_json_string(const char* s) {
    if (web_json_escape(s, json_buf, sizeof(json_buf)) == (size_t)-1) {
        server.sendContent("");
        return;
    }
    server.sendContent(json_buf);
}

static void handle_list() {
    if (!check_auth() || !storage_ready()) {
        return;
    }
    char path[WEB_PATH_MAX];
    if (!get_path_arg("path", path)) {
        send_text(400, "Invalid path");
        return;
    }
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        send_text(404, "Folder not found");
        return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json; charset=utf-8", "");
    server.sendContent("{\"path\":\"");
    send_json_string(path);
    server.sendContent("\",\"entries\":[");

    bool first = true;
    while (true) {
        watchdog_update();
        File entry = dir.openNextFile();
        if (!entry) {
            break;
        }
        const char* name = web_base_name(entry.name());
        if (is_hidden_entry(name)) {
            entry.close();
            continue;
        }
        server.sendContent(first ? "{\"name\":\"" : ",{\"name\":\"");
        send_json_string(name);
        char tail[80];
        // 更新日時は2106年まで unsigned long に収まる (printf の %lld は使えない場合がある)
        time_t mtime = entry.getLastWrite();
        snprintf(tail, sizeof(tail), "\",\"dir\":%s,\"size\":%lu,\"mtime\":%lu}",
                 entry.isDirectory() ? "true" : "false", (unsigned long)entry.size(),
                 (unsigned long)(mtime > 0 ? mtime : 0));
        server.sendContent(tail);
        first = false;
        entry.close();
    }
    dir.close();
    server.sendContent("]}");
    server.sendContent("");  // チャンク転送の終わり
}

// ----------------------------------------------------------------------------
// GET /api/download?path=<ファイル>
//   streamFile() は送り終わるまで戻らないので、少しずつ送ってウォッチドッグを更新する
// ----------------------------------------------------------------------------
static void handle_download() {
    if (!check_auth() || !storage_ready()) {
        return;
    }
    char path[WEB_PATH_MAX];
    if (!get_path_arg("path", path)) {
        send_text(400, "Invalid path");
        return;
    }
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        send_text(404, "File not found");
        return;
    }

    // ファイル名: ASCII 以外は filename* (UTF-8) で渡す
    const char* name = web_base_name(path);
    char ascii_name[WEB_PATH_MAX];
    snprintf(ascii_name, sizeof(ascii_name), "%s", name);
    for (char* p = ascii_name; *p; p++) {
        if ((unsigned char)*p >= 0x80 || *p == '"') {
            *p = '_';
        }
    }
    char encoded[WEB_PATH_MAX * 3];
    String disposition = String("attachment; filename=\"") + ascii_name + "\"";
    if (web_url_encode(name, encoded, sizeof(encoded)) != (size_t)-1) {
        disposition += String("; filename*=UTF-8''") + encoded;
    }

    server.sendHeader("Content-Disposition", disposition);
    server.setContentLength(f.size());
    server.send(200, "application/octet-stream", "");

    WiFiClient& client = server.client();
    uint8_t buf[WEB_SEND_CHUNK];
    int n;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
        int sent = 0;
        uint32_t last_progress_ms = millis();
        while (sent < n) {
            watchdog_update();
            size_t w = client.write(buf + sent, n - sent);
            if (w > 0) {
                sent += (int)w;
                last_progress_ms = millis();
            } else if (!client.connected() ||
                       (millis() - last_progress_ms) >= WEB_SEND_STALL_TIMEOUT_MS) {
                WEB_LOG("Download aborted: %s", path);
                f.close();
                return;
            } else {
                delay(1);
            }
        }
    }
    f.close();
}

// ----------------------------------------------------------------------------
// POST /api/upload?dir=<フォルダ>  (multipart、1リクエストに1ファイル)
//   "<名前>.part" に書いてから名前を付け替えるので、途中で切れても元のファイルは壊れない
// ----------------------------------------------------------------------------
static void fail_upload(int code, const char* msg) {
    if (upload_error == 0) {
        upload_error = code;
        upload_error_msg = msg;
    }
}

static void handle_upload_data() {
    HTTPUpload& up = server.upload();
    watchdog_update();

    switch (up.status) {
        case UPLOAD_FILE_START: {
            upload_started = true;
            upload_temp_created = false;
            upload_error = 0;
            upload_error_msg = "";

            if (web_auth_enabled && !server.authenticate(WEB_USERNAME, web_password)) {
                fail_upload(401, "Unauthorized");
                return;
            }
            if (!sd_card_mounted) {
                fail_upload(503, "SD card is not ready");
                return;
            }
            char dir[WEB_PATH_MAX];
            if (!get_path_arg("dir", dir)) {
                fail_upload(400, "Invalid folder");
                return;
            }
            const char* name = web_base_name(up.filename.c_str());
            if (!web_is_valid_name(name) ||
                !web_join_path(dir, name, upload_final_path, sizeof(upload_final_path))) {
                fail_upload(400, "Invalid file name");
                return;
            }
            int n = snprintf(upload_temp_path, sizeof(upload_temp_path), "%s.part",
                             upload_final_path);
            if (n <= 0 || (size_t)n >= sizeof(upload_temp_path)) {
                fail_upload(400, "File name is too long");
                return;
            }
            SD.remove(upload_temp_path);  // FILE_WRITE は追記なので、残っていたら消す
            upload_file = SD.open(upload_temp_path, FILE_WRITE);
            if (!upload_file) {
                fail_upload(500, "Cannot create file");
                return;
            }
            upload_temp_created = true;
            WEB_LOG("Upload start: %s", upload_final_path);
            break;
        }
        case UPLOAD_FILE_WRITE:
            if (upload_error == 0 && upload_file) {
                if (upload_file.write(up.buf, up.currentSize) != up.currentSize) {
                    fail_upload(500, "Write failed (SD card full?)");
                }
            }
            break;
        case UPLOAD_FILE_END:
            if (upload_file) {
                upload_file.close();
            }
            if (upload_error == 0) {
                if (SD.exists(upload_final_path)) {
                    SD.remove(upload_final_path);
                }
                if (!SD.rename(upload_temp_path, upload_final_path)) {
                    fail_upload(500, "Cannot rename file");
                }
            }
            if (upload_error != 0 && upload_temp_created) {
                SD.remove(upload_temp_path);
            }
            WEB_LOG("Upload end: %s (%u bytes, status %d)", upload_final_path,
                    (unsigned)up.totalSize, upload_error);
            break;
        case UPLOAD_FILE_ABORTED:
            if (upload_file) {
                upload_file.close();
            }
            if (upload_temp_created) {
                SD.remove(upload_temp_path);
            }
            fail_upload(400, "Upload aborted");
            WEB_LOG("Upload aborted");
            break;
    }
}

static void handle_upload_done() {
    bool started = upload_started;
    upload_started = false;

    if (upload_error == 401 || !started) {
        if (!check_auth()) {
            return;
        }
        send_text(400, "No file");
        return;
    }
    if (upload_error != 0) {
        send_text(upload_error, upload_error_msg);
        return;
    }
    send_text(200, "OK");
}

// ----------------------------------------------------------------------------
// POST /api/delete?path=<ファイル/空のフォルダ>
// ----------------------------------------------------------------------------
static void handle_delete() {
    if (!check_auth() || !storage_ready()) {
        return;
    }
    char path[WEB_PATH_MAX];
    if (!get_path_arg("path", path) || strcmp(path, "/") == 0) {
        send_text(400, "Invalid path");
        return;
    }
    File f = SD.open(path);
    if (!f) {
        send_text(404, "Not found");
        return;
    }
    bool is_dir = f.isDirectory();
    f.close();

    if (is_dir ? !SD.rmdir(path) : !SD.remove(path)) {
        send_text(409, is_dir ? "Cannot delete folder (not empty?)" : "Cannot delete file");
        return;
    }
    if (is_dir) {
        on_folder_removed(path);
    }
    WEB_LOG("Deleted: %s", path);
    send_text(200, "OK");
}

// ----------------------------------------------------------------------------
// POST /api/rename?path=<対象>&name=<新しい名前>  (同じフォルダ内での名前の変更)
// ----------------------------------------------------------------------------
static void handle_rename() {
    if (!check_auth() || !storage_ready()) {
        return;
    }
    char path[WEB_PATH_MAX];
    char parent[WEB_PATH_MAX];
    char new_path[WEB_PATH_MAX];
    if (!get_path_arg("path", path) || !web_parent_path(path, parent, sizeof(parent))) {
        send_text(400, "Invalid path");
        return;
    }
    String name = server.arg("name");
    if (!web_is_valid_name(name.c_str()) ||
        !web_join_path(parent, name.c_str(), new_path, sizeof(new_path))) {
        send_text(400, "Invalid name");
        return;
    }
    if (!SD.exists(path)) {
        send_text(404, "Not found");
        return;
    }
    if (SD.exists(new_path)) {
        send_text(409, "The name is already used");
        return;
    }
    if (!SD.rename(path, new_path)) {
        send_text(500, "Cannot rename");
        return;
    }
    on_folder_removed(path);
    WEB_LOG("Renamed: %s -> %s", path, new_path);
    send_text(200, "OK");
}

// ----------------------------------------------------------------------------
// POST /api/mkdir?path=<親フォルダ>&name=<フォルダ名>
// ----------------------------------------------------------------------------
static void handle_mkdir() {
    if (!check_auth() || !storage_ready()) {
        return;
    }
    char parent[WEB_PATH_MAX];
    char new_path[WEB_PATH_MAX];
    if (!get_path_arg("path", parent)) {
        send_text(400, "Invalid path");
        return;
    }
    String name = server.arg("name");
    if (!web_is_valid_name(name.c_str()) ||
        !web_join_path(parent, name.c_str(), new_path, sizeof(new_path))) {
        send_text(400, "Invalid name");
        return;
    }
    if (SD.exists(new_path)) {
        send_text(409, "The name is already used");
        return;
    }
    if (!SD.mkdir(new_path)) {
        send_text(500, "Cannot create folder");
        return;
    }
    WEB_LOG("Created folder: %s", new_path);
    send_text(200, "OK");
}

static void handle_not_found() { send_text(404, "Not found"); }

// ----------------------------------------------------------------------------
// 公開関数
// ----------------------------------------------------------------------------
void web_server_start() {
    if (server_running) {
        return;
    }
    if (!routes_registered) {
        server.on("/", HTTP_GET, handle_root);
        server.on("/api/list", HTTP_GET, handle_list);
        server.on("/api/download", HTTP_GET, handle_download);
        server.on("/api/upload", HTTP_POST, handle_upload_done, handle_upload_data);
        server.on("/api/delete", HTTP_POST, handle_delete);
        server.on("/api/rename", HTTP_POST, handle_rename);
        server.on("/api/mkdir", HTTP_POST, handle_mkdir);
        server.onNotFound(handle_not_found);
        routes_registered = true;
    }
    server.begin();
    server_running = true;

    if (MDNS.begin(WEB_MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", WEB_SERVER_PORT);
        mdns_running = true;
        mdns_last_announce_ms = millis();
    }
    WEB_LOG("Web server started: http://%s.local/ (http://%s/)", WEB_MDNS_HOSTNAME,
            WiFi.localIP().toString().c_str());
}

void web_server_stop() {
    if (mdns_running) {
        MDNS.end();
        mdns_running = false;
    }
    if (server_running) {
        server.stop();
        server_running = false;
        WEB_LOG("Web server stopped");
    }
    if (upload_file) {
        upload_file.close();
    }
}

void web_server_handle(bool allow_requests) {
    if (!server_running) {
        return;
    }
    if (mdns_running) {
        MDNS.update();
        uint32_t now_ms = millis();
        if (now_ms - mdns_last_announce_ms >= WEB_MDNS_REANNOUNCE_MS) {
            mdns_last_announce_ms = now_ms;
            MDNS.announce();
        }
    }
    if (allow_requests) {
        server.handleClient();
    }
}

