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
// これ以下のバイト数で、見込む長さの2倍を超えて間延びしたDATAは雑音とみなす
#define T88_NOISE_MAX_BYTES 4

// DATAタグの型(実データ種別)。bit8がボーレート(1:1200ボー、0:600ボー)、bit2-7は
// 8251のモード命令と同じ並び(bit7-6:ストップビット長01=1・10=1.5・11=2、
// bit5:偶数パリティ、bit4:パリティを使う、bit3-2:データビット長00=5〜11=8)。bit0-1は0
#define T88_TYPE_1200 0x0100
#define T88_MODE_8N2 0xCC  // 8bit・パリティ無し・ストップビット2(標準)
#define T88_MODE_8E1 0x7C  // 8bit・偶数パリティ・ストップビット1
#define T88_MODE_8O1 0x5C  // 8bit・奇数パリティ・ストップビット1

// DATAタグの型から読んだ1バイトの形
struct T88Frame {
    int data_bits;       // 5〜8
    int parity;          // 0:無し、1:奇数、2:偶数
    int stop_half_bits;  // ストップビットの長さ(半ビット単位、2=1ビット、3=1.5ビット、4=2ビット)
};

// 型の下位バイトが0のもの(古いツール)やストップビット長が00のものは標準(8bit・パリティ無し・
// ストップビット2)として扱う
static inline T88Frame t88_frame_from_type(uint16_t type) {
    uint8_t mode = (uint8_t)(type & 0xFC);
    T88Frame f = {8, 0, 4};
    if (mode == 0 || (mode & 0xC0) == 0) {
        return f;
    }
    f.data_bits = 5 + ((mode >> 2) & 0x03);
    if (mode & 0x10) {
        f.parity = (mode & 0x20) ? 2 : 1;
    }
    f.stop_half_bits = 1 + ((mode >> 6) & 0x03);
    return f;
}

/**
 * @brief 2つの T88 をつないで out_path に書く (追記録音で使う)
 *
 * base のタグの後に、無音 (T88_APPEND_BLANK_TICKS) と、add のタグ (VERSION を除き、
 * 開始時刻を後ろへずらしたもの) を続け、最後に END タグを書く。
 * @return false どちらかが T88 でない、または読み書きに失敗した
 */
bool t88_append(const char* base_path, const char* add_path, const char* out_path);
