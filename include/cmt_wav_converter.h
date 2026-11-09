#pragma once

/**
 * @brief WAVファイルの波形を、録音と同じ形のRAWファイルにする
 *
 * 半周期を録音PIOの計測値と同じ形(16bitワード: bit15=レベル、bit14-0=us、
 * 実際よりREC_PIO_OVERHEAD_US短い値)で書き、10ms以上の半周期は無音(0xFFFF+ミリ秒)にする。
 * できたRAWはconvert_raw_to_*()でそのまま各形式に変換できる
 * @return false WAVが読めない(リニアPCMでない)、または読み書きに失敗した
 */
bool convert_wav_to_raw(const char* wav_path, const char* raw_path);
