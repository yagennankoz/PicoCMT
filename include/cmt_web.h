#pragma once

// ============================================================================
// Webファイルマネージャ (WebServer + mDNS)
//   Wi-Fi 接続後に web_server_start() で開始し、http://<WEB_MDNS_HOSTNAME>.local/ で開ける
// ============================================================================

// Webサーバーと mDNS を開始する (Wi-Fi 接続後に呼ぶ)
void web_server_start();

// Webサーバーと mDNS を止める (Wi-Fi 切断時に呼ぶ)
void web_server_stop();

// loop から呼ぶ。allow_requests が false の間 (再生・録音中) はリクエストを処理しない
void web_server_handle(bool allow_requests);
