#pragma once

#include <stddef.h>

// ============================================================================
// Webサーバー用の文字列処理 (Arduinoに依存しないので PC 上でもテストできる)
// ============================================================================

// Webから受け取ったパスを正規化する ("/a//b/./" → "/a/b")。
// "/" で始まらない、".." を含む、制御文字を含む、長すぎる場合は false
bool web_normalize_path(const char* in, char* out, size_t out_size);

// ファイル名・フォルダ名として使えるか (区切り文字や FAT で使えない文字を含まない)
bool web_is_valid_name(const char* name);

// フォルダとファイル名をつなぐ ("/" + "A.T88" → "/A.T88")
bool web_join_path(const char* dir, const char* name, char* out, size_t out_size);

// 親フォルダのパス ("/a/b" → "/a"、"/a" → "/")。ルート自身は false
bool web_parent_path(const char* path, char* out, size_t out_size);

// path が base 自身か、その配下か
bool web_path_is_within(const char* path, const char* base);

// パスからファイル名の部分を取り出す (区切りは '/' と '\')
const char* web_base_name(const char* path);

// JSON 文字列用にエスケープする。戻り値は書き込んだ長さ、out が足りなければ (size_t)-1
size_t web_json_escape(const char* in, char* out, size_t out_size);

// URL 用にパーセントエンコードする (RFC 5987 の filename* 用)。
// 戻り値は書き込んだ長さ、out が足りなければ (size_t)-1
size_t web_url_encode(const char* in, char* out, size_t out_size);
