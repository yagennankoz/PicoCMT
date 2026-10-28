#pragma once

#include <SD.h>
#include <stdint.h>

#include <vector>

#include "cmt_sd_buffer.h"

// ============================================================================
// P6T (PC-6001 / PC6001V・PC6001VX のテープイメージ, Ver.2)
//   [ベタイメージ (P6 と同じデータ)] [フッタ] [ベタイメージのサイズ 4バイト]
//   数値はすべてリトルエンディアン (PC6001VX の p6t2.cpp で確認)
//
//   フッタ: "P6" / ver(1) / DATAブロック数(1) / オートスタート(1) / BASICモード(1) /
//           ページ数(1) / オートスタートのコマンド長(2) + コマンド / 拡張情報の長さ(2) + 拡張情報 /
//           DATAブロック x 数
//   DATAブロック: "TI" / ID(1) / 名前(16) / ボーレート(2, 600/1200) /
//                 無音の時間(2, ms) / ピー音の時間(2, ms) / オフセット(4) / サイズ(4)
//   1つの DATAブロックは「無音 → ピー音 → データ」の順に再生する。
//   BASIC のように「ヘッダ + プログラム」の場合は2つのブロックになり、同じ ID で関連付ける
// ============================================================================
#define P6T_FOOTER_ID "P6"
#define P6T_BLOCK_ID "TI"
#define P6T_VERSION 2
#define P6T_MAX_BLOCKS 255
#define P6T_NAME_LEN 16

// フッタが読めない (素の P6 の) ときに使う値。PC6001VX と同じ
#define P6T_PLAIN_STIME_MS 3400
#define P6T_PLAIN_PTIME_MS 6800
#define P6T_DEFAULT_BAUD 1200

struct P6TBlock {
    uint8_t id = 0;
    char name[P6T_NAME_LEN] = {0};
    uint16_t baud = P6T_DEFAULT_BAUD;
    uint16_t stime_ms = 0;  // 無音の時間
    uint16_t ptime_ms = 0;  // ピー音の時間
    uint32_t offset = 0;    // ベタイメージ先頭からの位置
    uint32_t size = 0;
};

struct P6TInfo {
    bool has_footer = false;  // false: 素の P6 としてブロック1つにした
    uint32_t data_size = 0;   // ベタイメージのサイズ
    std::vector<P6TBlock> blocks;
};

// ファイルの P6T フッタを読む。フッタが無い・壊れているときは、PC6001VX と同じく
// ファイル全体を1つのブロック (無音 3.4秒、ピー音 6.8秒、1200ボー) とする
bool p6t_read_info(File& f, P6TInfo& info);

// フッタとベタイメージのサイズを書く (ベタイメージを書き終えた後に呼ぶ)
bool p6t_write_footer(SdBufferedWrite* w, const std::vector<P6TBlock>& blocks,
                      uint32_t data_size);
