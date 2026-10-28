#include "player_t77.h"

#include <string.h>

#include "cmt_globals.h"
#include "cmt_t77_format.h"

extern volatile uint32_t playback_base_time_ms;
extern volatile uint32_t playback_tag_start_system_ms;
extern volatile bool playback_time_valid;

// キューの値は16bitなので、これより長いパルスは分割する
#define MAX_PULSE_US 60000

static inline uint64_t t77_ticks_to_us(uint64_t ticks) { return ticks * T77_TICK_US; }

PlayerT77::PlayerT77() {
    cmt_file = nullptr;
    buffered_reader = nullptr;
    reset_state();
}

PlayerT77::~PlayerT77() {
    if (buffered_reader) {
        delete buffered_reader;
    }
}

void PlayerT77::reset_state() {
    end_reached = false;
    exhausted = false;
    file_eof = false;

    has_current = false;
    current_level = false;
    total_ticks = 0;
    emitted_us = 0;

    pend_level = false;
    pend_us = 0;

    // reset_core1_playback_buffers() 後のPIOは最初の値をHighで出力する
    out_high_next = true;
}

void PlayerT77::set_log(const char* text) {
    snprintf(playback_log, sizeof(playback_log), "%s", text);
}

bool PlayerT77::enqueue_raw(queue_t* q, uint16_t us) {
    if (!queue_try_add(q, &us)) {
        return false;
    }
    out_high_next = !out_high_next;
    total_enqueued_pulses++;
    return true;
}

// 読み込み中のパルスを確定し、キューへ渡す待ちに回す
void PlayerT77::close_current() {
    uint64_t end_us = t77_ticks_to_us(total_ticks);
    pend_level = current_level;
    pend_us = (uint32_t)(end_us - emitted_us);
    emitted_us = end_us;
    has_current = false;
}

// 待ちパルスをキューへ入れる。キューが一杯なら残りを保持して false を返す
bool PlayerT77::drain_pending(queue_t* q) {
    while (pend_us > 0) {
        // 極性合わせのダミーと本体の2つ分の空きを確保してから入れる
        if ((PLAY_QUEUE_LENGTH - queue_get_level(q)) < 2) {
            return false;
        }

        // 出力レベルが合わない場合 (先頭がLow、長いパルスの分割など) は
        // 最短のダミー値を挟んで極性を合わせる
        if (out_high_next != pend_level) {
            if (!enqueue_raw(q, 0)) {
                return false;
            }
        }

        uint32_t chunk = (pend_us > MAX_PULSE_US) ? MAX_PULSE_US : pend_us;
        uint32_t overhead = pend_level ? PLAY_PIO_OVERHEAD_HIGH_US : PLAY_PIO_OVERHEAD_LOW_US;
        uint16_t value = (chunk > overhead) ? (uint16_t)(chunk - overhead) : 0;
        if (!enqueue_raw(q, value)) {
            return false;
        }
        pend_us -= chunk;
    }
    return true;
}

bool PlayerT77::init(File* f, const char* filename) {
    cmt_file = f;

    if (buffered_reader) {
        delete buffered_reader;
    }
    buffered_reader = new SdBufferedRead(cmt_file, 4096);

    if (!cmt_file || cmt_file->size() <= T77_HEADER_LEN) {
        return false;
    }

    uint8_t header[T77_HEADER_LEN];
    buffered_reader->seek(0);
    if (buffered_reader->read(header, T77_HEADER_LEN) != T77_HEADER_LEN) {
        return false;
    }

    if (memcmp(header, "XM7 TAPE IMAGE", 14) != 0) {
        return false;
    }

    reset_state();
    init_position_tracking();

    set_log("T77 Playing");
    return true;
}

void PlayerT77::process(queue_t* play_queue) {
    if (end_reached) {
        if (!exhausted) {
            exhausted = true;
        }
        return;
    }

    int iterations = 0;
    while (true) {
        if (pend_us > 0 && !drain_pending(play_queue)) {
            break;  // キューが一杯。残りは次回
        }

        if (file_eof) {
            if (has_current) {
                close_current();
                continue;
            }
            end_reached = true;
            break;
        }

        if ((PLAY_QUEUE_LENGTH - queue_get_level(play_queue)) <= 16 ||
            ++iterations > 8192) {
            break;
        }

        uint16_t val_le;
        if (!buffered_reader->read_word(val_le)) {
            file_eof = true;
            continue;
        }

        uint16_t val = (val_le >> 8) | (val_le << 8);
        uint32_t ticks = val & T77_COUNT_MASK;
        if (ticks == 0) {
            continue;  // 長さ0のワード (マーカー) は読み飛ばす
        }

        bool level = (val & T77_LEVEL_BIT) != 0;
        if (has_current && level != current_level) {
            close_current();
        }
        if (!has_current) {
            has_current = true;
            current_level = level;
        }
        total_ticks += ticks;
    }

    // タイマー描画用
    playback_base_time_ms = (uint32_t)(t77_ticks_to_us(total_ticks) / 1000ULL);
    playback_tag_start_system_ms = millis();
    playback_time_valid = true;
}

bool PlayerT77::seek(int direction, queue_t* play_queue) {
    if (direction < 0) {
        buffered_reader->seek(T77_HEADER_LEN);
        reset_state();

        set_log("T77 Seek 0");

        playback_base_time_ms = 0;
        playback_tag_start_system_ms = millis();
        playback_time_valid = true;

        return true;
    } else {
        end_reached = true;
        return true;
    }
}

void PlayerT77::stop() {}

bool PlayerT77::is_end_reached() { return end_reached; }
bool PlayerT77::is_exhausted() { return exhausted; }

void PlayerT77::get_status(PlaybackStatus* out) {
    out->playback_t88_mode = false;
    out->playback_t88_end_reached = end_reached;
    out->playback_source_exhausted = exhausted;
    out->file_position =
        buffered_reader ? buffered_reader->display_position() : 0;
    out->file_size = cmt_file ? cmt_file->size() : 0;
    snprintf(out->playback_log, sizeof(out->playback_log), "%s", playback_log);
}
