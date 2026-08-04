// ============================================================================
// player_cas.h
// ============================================================================
#pragma once

#include "cmt_player.h"
#include "cmt_sd_buffer.h"

class PlayerCAS : public CmtPlayer {
   private:
    File* cmt_file;
    SdBufferedRead* buffered_reader;
    bool end_reached;
    bool exhausted;

    // CAS再生用の状態遷移
    enum State {
        STATE_INIT,
        STATE_BLANK,    // 無音ギャップ
        STATE_HEADER,   // ピーー音（MARKキャリア）
        STATE_DATA,     // バイナリデータ出力
        STATE_TRAILER,  // 終わりのピーー音
        STATE_END
    };
    State state;

    uint32_t phase_counter;
    uint32_t current_file_pos;

    int bit_index;    // -1:Start, 0..7:Data, 8..9:Stop
    int pulse_count;  // 1ビットを構成するパルスのカウンタ

    // ブロックシグネチャ検出用のスライディングウィンドウ
    uint8_t window[8];
    int window_filled;

    char playback_log[24];
    uint32_t start_ms;

    int baud_rate;
    uint16_t mark_pulse_us;
    uint16_t space_pulse_us;

    bool enqueue_pulse(queue_t* q, uint16_t us);
    void set_log(const char* text);

   public:
    PlayerCAS();
    virtual ~PlayerCAS();

    bool init(File* f, const char* filename) override;
    void process(queue_t* play_queue) override;
    bool seek(int direction, queue_t* play_queue) override;
    void stop() override;
    bool is_end_reached() override;
    bool is_exhausted() override;
    void get_status(PlaybackStatus* out) override;
    void set_baud_rate(int baud);
};