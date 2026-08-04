#pragma once

#include "cmt_player.h"
#include "cmt_sd_buffer.h"

class PlayerT88 : public CmtPlayer {
   private:
    File* cmt_file;
    SdBufferedRead* buffered_reader;
    bool playback_t88_end_reached;
    bool playback_source_exhausted;
    uint16_t playback_tag_id;
    uint32_t playback_tag_ticks_remaining;
    bool playback_data_tag_active;
    uint32_t playback_data_bytes_remaining;
    uint32_t playback_data_extra_skip;
    uint16_t playback_data_actual_type;
    uint8_t playback_data_current_byte;
    int playback_data_frame_bit_index;
    bool playback_data_bit_active;
    uint8_t playback_data_bit_value;
    uint32_t playback_data_bit_ticks_remaining;
    uint32_t playback_current_start_ticks;
    uint32_t playback_queue_last_progress_ms;
    uint32_t playback_true_file_pos;
    char playback_last_log[24];
    bool playback_lock_start_ticks;

    bool waiting_for_silence;
    uint32_t silence_start_ms;
    uint32_t silence_target_ms;

    void set_playback_log(const char* text);
    bool read_u16_le(uint16_t* out);
    bool read_u32_le(uint32_t* out);
    bool skip_bytes(uint32_t count);
    bool is_t88_file();

    bool emit_pulses_from_current_tag(queue_t* play_queue);
    bool begin_next_data_tag_bit();
    bool emit_pulses_from_data_tag(queue_t* play_queue);
    void fill_queue(queue_t* play_queue);
    bool scan_for_blank_tag(uint32_t current_pos, int direction,
                            uint32_t* target_pos);

   public:
    PlayerT88();
    virtual ~PlayerT88();

    bool init(File* f, const char* filename) override;
    void process(queue_t* play_queue) override;
    bool seek(int direction, queue_t* play_queue) override;
    void stop() override;
    bool is_end_reached() override;
    bool is_exhausted() override;
    void get_status(PlaybackStatus* out) override;
};