#pragma once

#include <Arduino.h>

/**
 * @brief RAWファイルを解析し、MSX用CASファイルへ変換します。
 * (録音データから1200baud/2400baudを自動判別し、シグネチャを自動挿入します)
 *
 * @param raw_path 入力となる中間RAWファイルのSDパス
 * @param cas_path 出力先となる CAS ファイルのSDパス
 * @return true 変換成功
 * @return false 変換失敗
 */
bool convert_raw_to_cas(const char* raw_path, const char* cas_path);