#pragma once

#include <Arduino.h>

/**
 * @brief RAWファイルを解析し、純粋なバイナリダンプであるCMTファイルへ変換します。
 * 
 * @param raw_path 入力となる中間RAWファイルのSDパス（例: "/RAW_TEMP.BIN"）
 * @param cmt_path 出力先となる CMT ファイルのSDパス（例: "/GAME.CMT"）
 * @return true 変換成功
 * @return false 変換失敗
 */
bool convert_raw_to_cmt(const char* raw_path, const char* cmt_path);