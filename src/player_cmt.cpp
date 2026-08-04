#include "player_cmt.h"

#include <string.h>

#include "cmt_globals.h"

static uint16_t cmt_ticks_to_us_u16(uint32_t ticks) {
    uint64_t us = ((uint64_t)ticks * 1000000ULL + 2400ULL) / 4800ULL;
    if (us == 0) {
        us = 1;
    }
    if (us > 65535ULL) {
        us = 65535ULL;
    }
    return (uint16_t)us;
}

PlayerCMT::PlayerCMT() {
    cmt_file = nullptr;
    buffered_reader = nullptr;
    baud_rate = 1200;
}

PlayerCMT::~PlayerCMT() {
    if (buffered_reader) {
        delete buffered_reader;
    }
}

void PlayerCMT::set_log(const char* text) {
    snprintf(playback_log, sizeof(playback_log), "%s", text);
}

void PlayerCMT::set_baud_rate(int baud) {
    if (baud == 600 || baud == 1200) {
        this->baud_rate = baud;
    }
}

bool PlayerCMT::enqueue_pulse(queue_t* q, uint16_t us) {
    if (queue_get_level(q) >= PLAY_QUEUE_LENGTH) {
        return false;
    }
    bool res = queue_try_add(q, &us);
    if (res) {
        total_enqueued_pulses++;
    }
    return res;
}

bool PlayerCMT::init(File* f, const char* filename) {
    cmt_file = f;
    if (!cmt_file || cmt_file->size() == 0) {
        return false;
    }

    if (buffered_reader) {
        delete buffered_reader;
    }
    buffered_reader = new SdBufferedRead(cmt_file, 4096);

    end_reached = false;
    exhausted = false;
    state = STATE_INIT;
    phase_counter = 0;
    current_file_pos = 0;

    bit_index = -1;
    playback_data_bit_active = false;
    dummy_bytes_sent = 0;
    start_ms = millis();

    init_position_tracking();

    set_log("CMT Init");
    return true;
}

bool PlayerCMT::begin_next_bit() {
    while (true) {
        if (bit_index < 0) {
            // ダミーまたは実データの読み込み
            if (state == STATE_DUMMY_DATA) {
                if (dummy_bytes_sent < 8) {
                    current_byte = 0x00;
                    dummy_bytes_sent++;
                } else {
                    state = STATE_DATA;
                    set_log("CMT Data");
                    // 続けて下の STATE_DATA の判定へ流れる
                }
            }

            if (state == STATE_DATA) {
                if (buffered_reader->available() > 0) {
                    buffered_reader->read_byte(current_byte);
                    current_file_pos = buffered_reader->position();
                } else {
                    state = STATE_TRAILER_MARK;
                    phase_counter = 9600;  // 2秒間のMARK
                    set_log("CMT Trailer");
                    return false;  // 今回のデータブロック終了
                }
            }
            bit_index = 0;
        }

        int idx = bit_index++;

        if (idx == 0) {
            playback_data_bit_value = 0;  // スタートビット (SPACE=0)
            return true;
        }
        if (idx >= 1 && idx <= 8) {
            playback_data_bit_value =
                (current_byte >> (idx - 1)) & 0x01;  // LSB First
            return true;
        }
        if (idx == 9 || idx == 10) {
            playback_data_bit_value = 1;  // ストップビット (MARK=1)
            if (idx == 10) {
                bit_index = -1;  // 1バイト完了
            }
            return true;
        }
        bit_index = -1;
    }
}

void PlayerCMT::process(queue_t* play_queue) {
    if (end_reached) {
        if (!exhausted) {
            exhausted = true;
        }
        return;
    }

    uint32_t queue_free = PLAY_QUEUE_LENGTH - queue_get_level(play_queue);
    int iterations = 0;

    while (queue_free > 0) {
        if (++iterations > 256) {
            break;
        }

        switch (state) {
            case STATE_INIT:
                state = STATE_SPACE_CARRIER;
                phase_counter = 4800;  // 1200Hzを2秒間 = 4800サイクル
                set_log("CMT SPACE");
                break;

            case STATE_SPACE_CARRIER: {
                uint16_t pulse_us = cmt_ticks_to_us_u16(2);  // 417us (SPACE)
                if (enqueue_pulse(play_queue, pulse_us)) {
                    queue_free--;
                    phase_counter--;
                    if (phase_counter == 0) {
                        state = STATE_MARK_CARRIER;
                        phase_counter = 14400;  // 2400Hzを3秒間 = 14400サイクル
                        set_log("CMT MARK");
                    }
                } else {
                    queue_free = 0;
                }
                break;
            }

            case STATE_MARK_CARRIER: {
                uint16_t pulse_us = cmt_ticks_to_us_u16(1);  // 208us (MARK)
                if (enqueue_pulse(play_queue, pulse_us)) {
                    queue_free--;
                    phase_counter--;
                    if (phase_counter == 0) {
                        state = STATE_DUMMY_DATA;
                        bit_index = -1;
                        playback_data_bit_active = false;
                        dummy_bytes_sent = 0;
                        set_log("CMT Dummy");
                    }
                } else {
                    queue_free = 0;
                }
                break;
            }

            case STATE_DUMMY_DATA:
            case STATE_DATA: {
                uint32_t bit_ticks = (baud_rate == 1200) ? 4 : 8;

                if (!playback_data_bit_active) {
                    if (!begin_next_bit()) {
                        break;  // トレーラーへ移行した場合は抜ける
                    }
                    playback_data_bit_ticks_remaining = bit_ticks;
                    playback_data_bit_active = true;
                }

                uint32_t half_ticks =
                    playback_data_bit_value ? 1 : 2;  // 1=MARK, 2=SPACE
                uint32_t emit_ticks = playback_data_bit_ticks_remaining;
                if (emit_ticks > half_ticks) {
                    emit_ticks = half_ticks;
                }

                uint16_t pulse_us = cmt_ticks_to_us_u16(emit_ticks);
                if (enqueue_pulse(play_queue, pulse_us)) {
                    queue_free--;
                    playback_data_bit_ticks_remaining -= emit_ticks;
                    if (playback_data_bit_ticks_remaining == 0) {
                        playback_data_bit_active = false;
                    }
                } else {
                    queue_free = 0;
                }
                break;
            }

            case STATE_TRAILER_MARK: {
                uint16_t pulse_us = cmt_ticks_to_us_u16(1);  // 208us (MARK)
                if (enqueue_pulse(play_queue, pulse_us)) {
                    queue_free--;
                    phase_counter--;
                    if (phase_counter == 0) {
                        state = STATE_END;
                        set_log("CMT End");
                    }
                } else {
                    queue_free = 0;
                }
                break;
            }

            case STATE_END:
                end_reached = true;
                return;
        }
    }
}

bool PlayerCMT::seek(int direction, queue_t* play_queue) {
    if (direction < 0) {
        if (buffered_reader) {
            buffered_reader->seek(0);
        }
        state = STATE_INIT;
        end_reached = false;
        exhausted = false;
        set_log("CMT Seek 0");
        return true;
    } else {
        end_reached = true;
        return true;
    }
}

void PlayerCMT::stop() {}

bool PlayerCMT::is_end_reached() { return end_reached; }
bool PlayerCMT::is_exhausted() { return exhausted; }

void PlayerCMT::get_status(PlaybackStatus* out) {
    out->playback_t88_mode = false;
    out->playback_t88_end_reached = end_reached;
    out->playback_source_exhausted = exhausted;
    out->file_position =
        buffered_reader ? buffered_reader->display_position() : 0;
    out->file_size = cmt_file ? cmt_file->size() : 0;
    snprintf(out->playback_log, sizeof(out->playback_log), "%s", playback_log);
}