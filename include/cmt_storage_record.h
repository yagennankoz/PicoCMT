#pragma once

#include <stddef.h>

#include "cmt_types.h"

bool start_raw_recording();
void stop_raw_recording_and_finalize();
void handle_raw_recording_stream();

// 追記録音できるファイルか (拡張子で判定。T88 / T77 / CAS / P6T)
bool is_appendable_filename(const char* path);

// 変換の元にできるファイルか(RAW/WAV)
bool is_convertible_source(const char* path);

// RAW/WAVのファイルを指定の形式に変換し、同じフォルダに同じ名前(拡張子は形式のもの。
// 同じ名前があれば_2などを付ける)で書く。out_pathに書いたファイルのパスを入れる
bool convert_file_to_format(const char* src_path, RecFormat fmt, char* out_path,
                            size_t out_size);
