#pragma once

// FM-7 のファイル名の長さ
#define T77_NAME_LEN 8
#define T77_MAX_HEADER_NAMES 16

// 変換時にヘッダブロックから拾ったファイル名 (整形前の生データ)
struct T77HeaderNames {
    int count;
    char name[T77_MAX_HEADER_NAMES][T77_NAME_LEN];
};

// FM-7 (.T77) 変換用関数
bool convert_raw_to_t77(const char* raw_path, const char* out_path);

// 直前の convert_raw_to_t77() で見つかったファイル名
const T77HeaderNames& t77_last_header_names();
