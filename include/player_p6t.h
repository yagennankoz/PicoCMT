#pragma once

#include "cmt_p6t_format.h"
#include "cmt_player.h"
#include "cmt_sd_buffer.h"

// P6T (PC-6001) の再生
//   ブロックごとに 無音 (stime) → ピー音 (ptime) → データ をブロックのボーレートで出す。
//   1バイト = スタート(0) + 8bit (LSBファースト) + ストップ(1) x3
//   ボーレートはブロックに入っているので、ファイル一覧での選択は使わない
class PlayerP6T : public CmtPlayer {
   public:
    PlayerP6T();
    virtual ~PlayerP6T();

    bool init(File* f, const char* filename) override;
    void process(queue_t* play_queue) override;
    bool seek(int direction, queue_t* play_queue) override;
    void stop() override;
    bool is_end_reached() override;
    bool is_exhausted() override;
    void get_status(PlaybackStatus* out) override;

    // テスト・表示用
    const P6TInfo& info() const { return p6t; }

   private:
    enum State { STATE_BLOCK_START, STATE_SILENCE, STATE_LEADER, STATE_DATA, STATE_TRAILER, STATE_END };

    File* cmt_file;
    SdBufferedRead* reader;
    P6TInfo p6t;

    State state;
    int block_index;
    uint32_t block_remaining;  // ブロックのデータの残りバイト数
    int block_baud;
    uint32_t block_started_ms;  // 頭出し用: ブロックの再生を始めた時刻

    uint32_t halves_remaining;  // ピー音の残り半周期数
    bool silence_started;
    uint32_t silence_start_ms;

    // 1バイトの出力
    int bit_index;  // -1: 次のバイトを読む、0: スタート、1-8: データ、9-11: ストップ
    uint8_t current_byte;
    bool bit_active;
    int bit_value;
    uint32_t bit_ticks_remaining;  // 4800Hz 単位

    // ヘッダ (D3 x10 + ファイル名) の後にプログラムが続くブロックではピー音を入れる
    int d3_run;
    int name_left;
    bool mid_leader_pending;

    bool end_reached;
    bool exhausted;
    uint32_t current_pos;
    char playback_log[24];

    uint64_t total_ticks;  // ブロックの頭から出した tick 数 (丸め誤差を積み上げないため)
    uint16_t ticks_to_queue_us(uint32_t ticks, bool high);

    void start_block(int index);
    bool enqueue(queue_t* q, uint16_t us);
    bool emit_data(queue_t* q);
    uint32_t block_start_time_ms(int index) const;
    void set_log(const char* text);
};
