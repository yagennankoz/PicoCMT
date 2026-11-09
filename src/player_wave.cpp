#include "player_wave.h"

#include <string.h>

#include "cmt_globals.h"

extern volatile uint32_t playback_base_time_ms;
extern volatile uint32_t playback_tag_start_system_ms;
extern volatile bool playback_time_valid;

// キューの値は16bitなので、これより長い半周期は分割する
#define MAX_PULSE_US 60000

// ============================================================================
// 共通部分
// ============================================================================
PlayerHalves::PlayerHalves() {
    cmt_file = nullptr;
    buffered_reader = nullptr;
    playback_log[0] = '\0';
    reset_halves();
}

PlayerHalves::~PlayerHalves() {
    if (buffered_reader) {
        delete buffered_reader;
    }
}

void PlayerHalves::reset_halves() {
    end_reached = false;
    exhausted = false;
    source_eof = false;
    has_current = false;
    current_level = false;
    current_end_us = 0;
    emitted_us = 0;
    pend_level = false;
    pend_us = 0;
    // reset_core1_playback_buffers()後のPIOは最初の値をHighで出力する
    out_high_next = true;
}

void PlayerHalves::set_log(const char* text) {
    snprintf(playback_log, sizeof(playback_log), "%s", text);
}

bool PlayerHalves::enqueue_raw(queue_t* q, uint16_t us) {
    if (!queue_try_add(q, &us)) {
        return false;
    }
    out_high_next = !out_high_next;
    total_enqueued_pulses++;
    return true;
}

// 読み込み中の半周期を確定し、キューへ渡す待ちに回す
void PlayerHalves::close_current() {
    pend_level = current_level;
    pend_us = (uint32_t)(current_end_us - emitted_us);
    emitted_us = current_end_us;
    has_current = false;
}

// 待ちの半周期をキューへ入れる。キューが一杯なら残りを保持してfalseを返す
bool PlayerHalves::drain_pending(queue_t* q) {
    while (pend_us > 0) {
        // 極性合わせのダミーと本体の2つ分の空きを確保してから入れる
        if ((PLAY_QUEUE_LENGTH - queue_get_level(q)) < 2) {
            return false;
        }
        // 出力レベルが合わない場合(先頭がLow、長い半周期の分割など)は
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

void PlayerHalves::process(queue_t* play_queue) {
    if (end_reached) {
        exhausted = true;
        return;
    }

    int iterations = 0;
    while (true) {
        if (pend_us > 0 && !drain_pending(play_queue)) {
            break;  // キューが一杯。残りは次回
        }
        if (source_eof) {
            if (has_current) {
                close_current();
                continue;
            }
            end_reached = true;
            break;
        }
        if ((PLAY_QUEUE_LENGTH - queue_get_level(play_queue)) <= 16 || ++iterations > 4096) {
            break;
        }

        bool level;
        uint64_t end_us;
        if (!read_half(&level, &end_us)) {
            source_eof = true;
            continue;
        }
        if (end_us <= emitted_us) {
            continue;  // 長さ0
        }
        if (has_current && level != current_level) {
            close_current();
        }
        has_current = true;
        current_level = level;
        if (end_us > current_end_us) {
            current_end_us = end_us;
        }
    }

    // タイマー描画用
    playback_base_time_ms = (uint32_t)(emitted_us / 1000ULL);
    playback_tag_start_system_ms = millis();
    playback_time_valid = true;
}

bool PlayerHalves::seek(int direction, queue_t* play_queue) {
    (void)play_queue;
    if (direction < 0) {
        rewind();
        reset_halves();
        set_log("Seek 0");
        playback_base_time_ms = 0;
        playback_tag_start_system_ms = millis();
        playback_time_valid = true;
        return true;
    }
    end_reached = true;
    return true;
}

void PlayerHalves::stop() {}

bool PlayerHalves::is_end_reached() { return end_reached; }
bool PlayerHalves::is_exhausted() { return exhausted; }

void PlayerHalves::get_status(PlaybackStatus* out) {
    out->playback_t88_mode = false;
    out->playback_t88_end_reached = end_reached;
    out->playback_source_exhausted = exhausted;
    out->file_position = buffered_reader ? buffered_reader->display_position() : 0;
    out->file_size = cmt_file ? cmt_file->size() : 0;
    snprintf(out->playback_log, sizeof(out->playback_log), "%s", playback_log);
}

// ============================================================================
// RAW
// ============================================================================
bool PlayerRaw::init(File* f, const char* filename) {
    (void)filename;
    cmt_file = f;
    if (!cmt_file || cmt_file->size() < 2) {
        return false;
    }
    if (buffered_reader) {
        delete buffered_reader;
    }
    buffered_reader = new SdBufferedRead(cmt_file, 4096);
    rewind();
    reset_halves();
    init_position_tracking();
    set_log("RAW Playing");
    return true;
}

void PlayerRaw::rewind() {
    buffered_reader->seek(0);
    time_us = 0;
    last_level = false;
}

bool PlayerRaw::read_half(bool* level, uint64_t* end_us) {
    while (true) {
        uint16_t w;
        if (!buffered_reader->read_word(w)) {
            return false;
        }
        if (w == 0x0000) {
            continue;
        }
        if (w == 0xFFFF) {
            // 無音。それまでのレベルの反対のまま、波が止まっていた
            uint16_t ms = 0;
            if (!buffered_reader->read_word(ms)) {
                return false;
            }
            if (ms == 0) {
                continue;
            }
            last_level = !last_level;
            time_us += (uint64_t)ms * 1000;
        } else {
            // 録音PIOの計測値は実際より短いので補正する(変換と同じ)
            last_level = (w & 0x8000) != 0;
            time_us += raw_pulse_us(w);
        }
        *level = last_level;
        *end_us = time_us;
        return true;
    }
}

// ============================================================================
// WAV
// ============================================================================
bool PlayerWav::init(File* f, const char* filename) {
    (void)filename;
    cmt_file = f;
    if (!cmt_file) {
        return false;
    }
    if (buffered_reader) {
        delete buffered_reader;
    }
    buffered_reader = new SdBufferedRead(cmt_file, 4096);
    SdBufferedRead* r = buffered_reader;
    auto read_at = [r](uint32_t pos, uint8_t* buf, uint32_t len) -> int {
        if (!r->seek(pos)) {
            return -1;
        }
        return r->read(buf, len);
    };
    if (!wav_parse_header(read_at, (uint32_t)cmt_file->size(), &fmt)) {
        return false;
    }
    rewind();
    reset_halves();
    init_position_tracking();
    set_log("WAV Playing");
    return true;
}

void PlayerWav::rewind() {
    buffered_reader->seek(fmt.data_offset);
    detector.begin(fmt.sample_rate);
    data_read = 0;
    tail_done = false;
}

bool PlayerWav::read_half(bool* level, uint64_t* end_us) {
    uint8_t frame[32];
    uint32_t n = fmt.block_align;
    if (n > sizeof(frame)) {
        n = sizeof(frame);
    }
    // 波が無い所(無音)を読み続けて止まらないよう、0.1秒ごとに一度戻る。
    // そこまでを今のレベルの続きとして渡す(同じレベルはまとめられる)。
    // 波の切り替わりは0を横切ってから少し後に分かるので、10ms手前までにする
    uint32_t limit = fmt.sample_rate / 10;
    uint32_t count = 0;
    while (data_read + fmt.block_align <= fmt.data_size) {
        if (++count > limit) {
            *level = (detector.state() > 0);
            *end_us = (detector.samples() - fmt.sample_rate / 100) * 1000000ULL / fmt.sample_rate;
            return true;
        }
        if (buffered_reader->read(frame, n) != (int)n) {
            break;
        }
        if (fmt.block_align > n) {
            buffered_reader->seek(buffered_reader->position() + (fmt.block_align - n));
        }
        data_read += fmt.block_align;

        uint64_t edge_q8;
        bool high;
        if (detector.feed(wav_sample_value(frame, fmt.bits), &edge_q8, &high)) {
            *level = high;
            *end_us = edge_q8 * 1000000ULL / ((uint64_t)fmt.sample_rate << 8);
            return true;
        }
    }
    // ファイルの終わり。最後のレベルのまま終わりまで出す
    if (!tail_done && detector.state() != 0) {
        tail_done = true;
        *level = (detector.state() > 0);
        *end_us = detector.samples() * 1000000ULL / fmt.sample_rate;
        return true;
    }
    return false;
}
