#pragma once

// Common hardware / behavior configuration
#define SCREEN_ADDRESS   0x3C

// Common stream and queue sizing
#define BUFFER_SIZE      512
#define SD_READ_SIZE     512
#define PLAY_QUEUE_LENGTH 2048

// 再生PIO(cmt_play.pio)の命令オーバーヘッド。キューの値yに対して
// y+7us(High) / y+6us(Low) 出力するので、プレーヤーはこの分を引いて値を入れる
#define PLAY_PIO_OVERHEAD_HIGH_US 7
#define PLAY_PIO_OVERHEAD_LOW_US 6
// 出力レベルを追跡しないプレーヤー用 (Highに合わせる。Lowは1us短くなる)
#define PLAY_PIO_OVERHEAD_US 7

#define DEBOUNCE_MS      20
#define LONG_PRESS_MS    1000
#define REPEAT_MS        60

// ウォッチドッグのタイムアウト (loop() が止まったら再起動する)
#define SYSTEM_WATCHDOG_TIMEOUT_MS 4000
