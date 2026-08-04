// ============================================================================
// player_p6.h
// ============================================================================
#pragma once

#include "cmt_player.h"
#include "cmt_sd_buffer.h"

class PlayerP6 : public CmtPlayer {
   private:
    File* cmt_file;
    SdBufferedRead* buffered_reader;
    bool end_reached;
    bool exhausted;

    // P6再生用の状態遷移
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

    int bit_index;  // -1:Start, 0..7:Data, 8..9:Stop
    uint8_t current_byte;
    int pulse_count;  // 1ビットを構成するパルスのカウンタ

    char playback_log[24];
    uint32_t start_ms;

    bool enqueue_pulse(queue_t* q, uint16_t us);
    void set_log(const char* text);

   public:
    PlayerP6();
    virtual ~PlayerP6();

    bool init(File* f, const char* filename) override;
    void process(queue_t* play_queue) override;
    bool seek(int direction, queue_t* play_queue) override;
    void stop() override;
    bool is_end_reached() override;
    bool is_exhausted() override;
    void get_status(PlaybackStatus* out) override;
};