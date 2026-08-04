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

    uint32_t accumulated_clocks;
    bool waiting_for_silence;
    uint32_t silence_start_ms;
    uint32_t silence_target_ms;

    char playback_log[24];

    bool enqueue_pulse(queue_t* q, uint16_t us);
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