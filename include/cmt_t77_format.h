#pragma once

#include <stdint.h>

// ============================================================================
// T77 (FM-7 / XM7 テープイメージ) フォーマット定義
//   先頭16バイト : "XM7 TAPE IMAGE 0"
//   以降 2バイト(ビッグエンディアン)単位のワード列
//     bit15    : 信号レベル (1=High, 0=Low)
//     bit14-0  : そのレベルが続くカウント数
//   カウント0のワード(0x0000/0x8000)は長さを持たないだけで、終端の意味はない
// ============================================================================
#define T77_HEADER_STR "XM7 TAPE IMAGE 0"
#define T77_HEADER_LEN 16
// XM7 はヘッダの直後に 0x0000 を1つ書き、t772wav はそれを読み飛ばす (先頭を 0x12 から読む)。
// PicoCmt も同じ形で書き出す。データは T77_DATA_OFFSET から始まる
#define T77_DATA_OFFSET 18
#define T77_LEVEL_BIT 0x8000
#define T77_COUNT_MASK 0x7FFF

// 1カウントの時間 (us)。XM7 と、その周辺ツール (t772wav / wav2t77) に合わせる
#define T77_TICK_US 9ULL
