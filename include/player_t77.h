// ============================================================================
// player_t77.h
// ============================================================================
#pragma once

#include "cmt_player.h"
#include "cmt_sd_buffer.h"

class PlayerT77 : public CmtPlayer {
   private:
    File* cmt_file;
    SdBufferedRead* buffered_reader;
    bool end_reached;
    bool exhausted;
    bool file_eof;

    // 読み込み中のパルス (同じレベルが続くワードをまとめたもの)
    bool has_current;
    bool current_level;
    uint64_t total_ticks;  // 読み込み済みカウントの累計
    uint64_t emitted_us;   // キューへ渡し終えた時間の累計

    // キューに入りきらなかったパルスの残り
    bool pend_level;
    uint32_t pend_us;

    // 次にキューへ入れた値が出力されるレベル (PIOは値ごとにH/Lを反転する)
    bool out_high_next;

    char playback_log[24];

    void reset_state();
    bool enqueue_raw(queue_t* q, uint16_t us);
    void close_current();
    bool drain_pending(queue_t* q);
    void set_log(const char* text);

   public:
    PlayerT77();
    virtual ~PlayerT77();

    bool init(File* f, const char* filename) override;
    void process(queue_t* play_queue) override;
    bool seek(int direction, queue_t* play_queue) override;
    void stop() override;
    bool is_end_reached() override;
    bool is_exhausted() override;
    void get_status(PlaybackStatus* out) override;
};
