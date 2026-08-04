#pragma once

#include <Arduino.h>
#include <SD.h>

#include "cmt_sd_buffer.h"
#include "cmt_types.h"
#include "pico/util/queue.h"

class CmtPlayer {
   protected:
    uint32_t total_enqueued_pulses;
    uint32_t display_file_pos;

    struct PosRecord {
        uint32_t pulse_target;
        uint32_t file_pos;
    };
    PosRecord pos_history[64];
    int history_head;
    int history_tail;

    // トラッキングの初期化 (各Playerの init() 内で呼ぶ)
    void init_position_tracking() {
        total_enqueued_pulses = 0;
        display_file_pos = 0;
        history_head = 0;
        history_tail = 0;
    }

    // 現在のファイル位置を記録 (各Playerの process() の最後で呼ぶ)
    void record_current_position(uint32_t file_pos) {
        int next_head = (history_head + 1) % 64;

        // 連続して同じファイル位置を記録しないよう最適化
        int prev_head = (history_head - 1 + 64) % 64;
        if (history_head != history_tail &&
            pos_history[prev_head].file_pos == file_pos) {
            pos_history[prev_head].pulse_target = total_enqueued_pulses;
            return;
        }

        if (next_head != history_tail) {
            pos_history[history_head].pulse_target = total_enqueued_pulses;
            pos_history[history_head].file_pos = file_pos;
            history_head = next_head;
        }
    }

    // 実際に消費されたパルス数から、対応するファイル位置を割り出す
    // (get_statusで呼ぶ)
    uint32_t get_sync_file_position(uint32_t total_dequeued_pulses) {
        while (history_head != history_tail) {
            // Core1が消費したパルス数が、記録したパルス数に到達したら表示位置を更新
            if ((int32_t)(total_dequeued_pulses -
                          pos_history[history_tail].pulse_target) >= 0) {
                display_file_pos = pos_history[history_tail].file_pos;
                history_tail = (history_tail + 1) % 64;
            } else {
                break;
            }
        }
        return display_file_pos;
    }

   public:
    virtual ~CmtPlayer() {}

    virtual bool init(File* f, const char* filename) = 0;
    virtual void process(queue_t* play_queue) = 0;
    virtual bool seek(int direction, queue_t* play_queue) = 0;
    virtual void stop() = 0;
    virtual bool is_end_reached() = 0;
    virtual bool is_exhausted() = 0;
    virtual void get_status(PlaybackStatus* out) = 0;
};