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

// 追記で2つの T88 をつなぐときに、境目に入れる無音の長さ (1/4800秒単位、2秒)
#define T88_APPEND_BLANK_TICKS 9600

// データの後にこれより長くピー音か無音が続いたら、DATA タグを区切る (us)。
// N88-BASIC のモニタの W名前 では、ヘッダ ("$$$" + 名前) とデータの間が約70ms
// (X88000 の記録)。600ボーの2バイト分 (約37ms) より長くする
#define T88_DATA_GAP_US 40000

/**
 * @brief 2つの T88 をつないで out_path に書く (追記録音で使う)
 *
 * base のタグの後に、無音 (T88_APPEND_BLANK_TICKS) と、add のタグ (VERSION を除き、
 * 開始時刻を後ろへずらしたもの) を続け、最後に END タグを書く。
 * @return false どちらかが T88 でない、または読み書きに失敗した
 */
bool t88_append(const char* base_path, const char* add_path, const char* out_path);
