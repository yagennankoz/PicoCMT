// ============================================================================
// player_p6.cpp
// ============================================================================
#include "player_p6.h"

#include <string.h>

#include "cmt_globals.h"

PlayerP6::PlayerP6() {
    cmt_file = nullptr;
    buffered_reader = nullptr;
}

PlayerP6::~PlayerP6() {
    if (buffered_reader) {
        delete buffered_reader;
    }
}

void PlayerP6::set_log(const char* text) {
    snprintf(playback_log, sizeof(playback_log), "%s", text);
}

bool PlayerP6::enqueue_pulse(queue_t* q, uint16_t us) {
    if (queue_get_level(q) >= PLAY_QUEUE_LENGTH) {
        return false;
    }
    bool res = queue_try_add(q, &us);
    if (res) {
        total_enqueued_pulses++;
    }
    return res;
}

bool PlayerP6::init(File* f, const char* filename) {
    cmt_file = f;

    // P6は純粋なバイナリダンプなので、ファイルサイズが0でなければOKとする
    if (!cmt_file || cmt_file->size() == 0) {
        return false;
    }

    if (buffered_reader) {
        delete buffered_reader;
    }
    buffered_reader = new SdBufferedRead(cmt_file, 4096);

    buffered_reader->seek(0);

    end_reached = false;
    exhausted = false;
    state = STATE_INIT;
    phase_counter = 0;
    current_file_pos = 0;
    bit_index = -1;
    pulse_count = 0;
    start_ms = millis();

    init_position_tracking();

    set_log("P6 Init");
    return true;
}

void PlayerP6::process(queue_t* play_queue) {
    if (end_reached) {
        if (!exhausted) {
            exhausted = true;
        }
        return;
    }

    int iterations = 0;
    while ((PLAY_QUEUE_LENGTH - queue_get_level(play_queue)) > 16) {
        if (++iterations > 256) {
            break;
        }

        switch (state) {
            case STATE_INIT:
                state = STATE_BLANK;
                start_ms = millis();
                // ハードウェア無音化トリック(2usアンダーラン)
                enqueue_pulse(play_queue, 2);
                set_log("P6 Blank");
                break;

            case STATE_BLANK:
                // 実時間で2秒間の無音ギャップを作る
                if (millis() - start_ms >= 2000) {
                    state = STATE_HEADER;
                    // 600baudのMARK(2400Hz)を3秒間
                    // 2400Hz = 1秒間に4800回の半波長。3秒 = 14400回。
                    phase_counter = 14400;
                    pulse_count = 0;
                    set_log("P6 Header");
                } else {
                    return;
                }
                break;

            case STATE_HEADER:
                if (enqueue_pulse(play_queue, 208)) {
                    phase_counter--;
                    if (phase_counter == 0) {
                        state = STATE_DATA;
                        bit_index = -1;  // スタートビットから開始
                        pulse_count = 0;
                        set_log("P6 Data");
                    }
                }
                break;

            case STATE_DATA: {
                if (bit_index == -1 && pulse_count == 0) {
                    if (buffered_reader->available() > 0) {
                        buffered_reader->read_byte(current_byte);
                        current_file_pos = buffered_reader->position();
                    } else {
                        state = STATE_TRAILER;
                        // ファイル末尾は2秒間のMARK
                        phase_counter = 4800 * 2;
                        pulse_count = 0;
                        set_log("P6 Trailer");
                        break;
                    }
                }

                // bit_index: -1=Start(SPACE:0), 0..7=Data, 8..9=Stop(MARK:1)
                bool bit_val;
                if (bit_index == -1) {
                    bit_val = false;
                } else if (bit_index >= 8) {
                    bit_val = true;  // 安全のため2ストップビット
                } else {
                    bit_val = (current_byte >> bit_index) & 1;
                }

                // 【P6 / 600baud のエンコード仕様】
                // 1ビット = 約1.66ms (1/600秒)
                // MARK (1) = 2400Hz (半波長208us) が 8回
                // SPACE(0) = 1200Hz (半波長417us) が 4回
                int target_pulses = bit_val ? 8 : 4;
                uint16_t pulse_us = bit_val ? 208 : 417;

                if (pulse_count < target_pulses) {
                    if (enqueue_pulse(play_queue, pulse_us)) {
                        pulse_count++;
                    }
                } else {
                    pulse_count = 0;
                    bit_index++;
                    if (bit_index > 9) {  // 1バイト送信完了
                        bit_index = -1;
                    }
                }
                break;
            }

            case STATE_TRAILER:
                if (enqueue_pulse(play_queue, 208)) {
                    phase_counter--;
                    if (phase_counter == 0) {
                        state = STATE_END;
                        set_log("P6 End");
                    }
                }
                break;

            case STATE_END:
                end_reached = true;
                return;
        }
    }
}

bool PlayerP6::seek(int direction, queue_t* play_queue) {
    if (direction < 0) {
        if (buffered_reader) {
            buffered_reader->seek(0);
        }
        state = STATE_INIT;
        end_reached = false;
        exhausted = false;
        set_log("P6 Seek 0");
        return true;
    } else {
        end_reached = true;
        return true;
    }
}

void PlayerP6::stop() {}

bool PlayerP6::is_end_reached() { return end_reached; }
bool PlayerP6::is_exhausted() { return exhausted; }

void PlayerP6::get_status(PlaybackStatus* out) {
    out->playback_t88_mode = false;
    out->playback_t88_end_reached = end_reached;
    out->playback_source_exhausted = exhausted;
    out->file_position =
        buffered_reader ? buffered_reader->display_position() : 0;
    out->file_size = cmt_file ? cmt_file->size() : 0;
    snprintf(out->playback_log, sizeof(out->playback_log), "%s", playback_log);
}