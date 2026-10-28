#pragma once

// RAW → P6T (PC-6001)
//   無音、または一定以上のピー音 (P6T_SPLIT_LEADER_MS) でブロックを区切り、
//   ブロックごとに ボーレート・無音の時間・ピー音の時間・ヘッダのファイル名 を記録する
bool convert_raw_to_p6t(const char* raw_path, const char* p6t_path);

// P6T を結合する (追記録音用): base のブロックの後に add のブロックを続けて out に書く
bool p6t_append(const char* base_path, const char* add_path, const char* out_path);

// これより長いピー音 (データの間の '1' の連続) があればブロックを区切る (ms)
#define P6T_SPLIT_LEADER_MS 100
