#pragma once

#include <Arduino.h>

/**
 * @brief RAWファイルを解析し、DATAタグ生成および600baud/1200baud対応を行ったT88ファイルへ変換します。
 * 
 * @param raw_path 入力となる中間RAWファイルのSDパス（例: "/RAW_TEMP.BIN"）
 * @param t88_path 出力先となる T88 ファイルのSDパス（例: "/GAME.T88"）
 * @return true 変換成功
 * @return false 変換失敗
 */
bool convert_raw_to_t88(const char* raw_path, const char* t88_path);
