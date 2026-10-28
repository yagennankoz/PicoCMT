#pragma once

#include "cmt_player.h"
#include "cmt_sd_buffer.h"

class PlayerCMT : public CmtPlayer {
   private:
    File* cmt_file;
    SdBufferedRead* buffered_reader;
    bool end_reached;
    bool exhausted;

    enum State {
        STATE_INIT,
        STATE_SPACE_CARRIER,
        STATE_MARK_CARRIER,
        STATE_DUMMY_DATA,
        STATE_DATA,
        STATE_MID_LEADER,  // ヘッダ (D3 x10 / "$$$" + ファイル名) とデータの間のピー音
        STATE_TRAILER_MARK,
        STATE_END
    };
    State state;

    uint32_t phase_counter;
    uint32_t current_file_pos;

    int bit_index;
    uint8_t current_byte;

    bool playback_data_bit_active;
    uint8_t playback_data_bit_value;
    uint32_t playback_data_bit_ticks_remaining;
    int dummy_bytes_sent;

    char playback_log[24];
    uint32_t start_ms;

    int baud_rate;

    // ヘッダ (D3 x10 / "$$$" + ファイル名) の検出
    bool p6_mode;
    int hdr_d3_run;          // 続けて出した D3 の数
    int hdr_dollar_run;      // 続けて出した 24 ('$') の数
    bool hdr_prev_zero;      // 前のバイトが 00 (またはファイルの先頭)
    int hdr_name_left;       // ヘッダのファイル名の残りバイト数 (-1 = ヘッダの外)
    bool mid_leader_pending; // 次のバイトの前にピー音を入れる
    void track_header(uint8_t b);
    void reset_header_tracking();

    bool enqueue_pulse(queue_t* q, uint16_t us);
    void set_log(const char* text);

    bool begin_next_bit();

   public:
    PlayerCMT();
    virtual ~PlayerCMT();

    bool init(File* f, const char* filename) override;
    void process(queue_t* play_queue) override;
    bool seek(int direction, queue_t* play_queue) override;
    void stop() override;
    bool is_end_reached() override;
    bool is_exhausted() override;
    void get_status(PlaybackStatus* out) override;
    void set_baud_rate(int baud);
};