#pragma once

bool start_raw_recording();
void stop_raw_recording_and_finalize();
void handle_raw_recording_stream();

// 追記録音できるファイルか (拡張子で判定。T88 / T77 / CAS / P6T)
bool is_appendable_filename(const char* path);
