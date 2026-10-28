#include "player_cmt.h"

#include <string.h>
#include <strings.h>

#include "cmt_globals.h"

// tick数(4800Hz)から再生キューに入れる値(us)を求める (再生PIOの命令オーバーヘッド分を引く)
static uint16_t cmt_ticks_to_us_u16(uint32_t ticks) {
    uint64_t us = ((uint64_t)ticks * 1000000ULL + 2400ULL) / 4800ULL;
    us = (us > PLAY_PIO_OVERHEAD_US) ? us - PLAY_PIO_OVERHEAD_US : 0;
    if (us > 65535ULL) {
        us = 65535ULL;
    }
    return (uint16_t)us;
}

// BASIC のヘッダ (D3 x10 + ファイル名6文字) とプログラムの間に入れるピー音 (2400Hz) の長さ。
// 半周期の数 (4800 = 1秒)。どちらの機種の BASIC も、ヘッダの後に短いピー音を入れてから
// プログラムを書く (読み込み側はその間にファイル名を表示する)。
// ピー音は '1' が続くのと同じなので、長くても読み込みの妨げにならない
//   P6 (PC-6001): 長さの資料が見つからないので、間に合うよう1秒
//   CMT (PC-8001/8801): X88000 が記録した実機の BASIC の T88 で約0.21秒
static constexpr uint32_t P6_MID_LEADER_HALVES = 4800;
static constexpr uint32_t CMT_MID_LEADER_HALVES = 1000;
static constexpr int HEADER_D3_COUNT = 10;
static constexpr int HEADER_DOLLAR_COUNT = 3;
static constexpr int HEADER_NAME_LEN = 6;

// ストップビットの数。PC-8801 は2つ、PC-6001 は3つ (1200ボーで100バイト/秒。
// PC6001VX の DEFAULT_STOPBIT も3)
static constexpr int CMT_STOP_BITS = 2;
static constexpr int P6_STOP_BITS = 3;

PlayerCMT::PlayerCMT() {
    cmt_file = nullptr;
    buffered_reader = nullptr;
    baud_rate = 1200;
    p6_mode = false;
}

// 出力したバイトからヘッダを見つけ、ヘッダの直後にピー音を入れる印を付ける
//   D3 x10 + ファイル名6バイト: BASIC (PC-8001 / 8801 / 6001)
//   24 x3 ("$$$") + ファイル名6バイト: N88-BASIC のモニタの W名前 で保存したマシン語
//     (PC-8801 のみ。X88000 の記録ではヘッダの後に約70msのピー音)。
//     データの中の "$$$" に反応しないよう、ファイルの先頭か 00 の直後にあるときだけ
void PlayerCMT::track_header(uint8_t b) {
    bool prev_zero = hdr_prev_zero;
    hdr_prev_zero = (b == 0x00);
    if (hdr_name_left > 0) {
        if (--hdr_name_left == 0) {
            hdr_name_left = -1;
            mid_leader_pending = true;
        }
        return;
    }
    hdr_d3_run = (b == 0xD3) ? hdr_d3_run + 1 : 0;
    if (!p6_mode && b == 0x24 && (hdr_dollar_run > 0 || prev_zero)) {
        hdr_dollar_run++;
    } else {
        hdr_dollar_run = 0;
    }
    if (hdr_d3_run == HEADER_D3_COUNT || hdr_dollar_run == HEADER_DOLLAR_COUNT) {
        hdr_d3_run = 0;
        hdr_dollar_run = 0;
        hdr_name_left = HEADER_NAME_LEN;
    }
}

void PlayerCMT::reset_header_tracking() {
    hdr_d3_run = 0;
    hdr_dollar_run = 0;
    hdr_prev_zero = true;  // ファイルの先頭
    hdr_name_left = -1;
    mid_leader_pending = false;
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

    const char* ext = filename ? strrchr(filename, '.') : nullptr;
    p6_mode = (ext != nullptr && strcasecmp(ext, ".P6") == 0);
    reset_header_tracking();

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
                if (mid_leader_pending) {
                    // ヘッダを出し終えたので、プログラムの前にピー音を入れる
                    mid_leader_pending = false;
                    state = STATE_MID_LEADER;
                    phase_counter = p6_mode ? P6_MID_LEADER_HALVES : CMT_MID_LEADER_HALVES;
                    set_log("Mid Leader");
                    return false;
                }
                if (buffered_reader->available() > 0) {
                    buffered_reader->read_byte(current_byte);
                    current_file_pos = buffered_reader->position();
                    track_header(current_byte);
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
        int last_stop_idx = 8 + (p6_mode ? P6_STOP_BITS : CMT_STOP_BITS);
        if (idx >= 9 && idx <= last_stop_idx) {
            playback_data_bit_value = 1;  // ストップビット (MARK=1)
            if (idx == last_stop_idx) {
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
                if (p6_mode) {
                    // PC-6001 の CSAVE と同じく、長いピー音から始める
                    // (PC-8801 用の 1200Hz とダミーの 0x00 は出さない)
                    state = STATE_MARK_CARRIER;
                    phase_counter = 14400;  // 2400Hzを3秒間
                    set_log("P6 Leader");
                    break;
                }
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
                        state = p6_mode ? STATE_DATA : STATE_DUMMY_DATA;
                        bit_index = -1;
                        playback_data_bit_active = false;
                        dummy_bytes_sent = 0;
                        set_log(p6_mode ? "CMT Data" : "CMT Dummy");
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

            case STATE_MID_LEADER: {
                uint16_t pulse_us = cmt_ticks_to_us_u16(1);  // 208us (MARK)
                if (enqueue_pulse(play_queue, pulse_us)) {
                    queue_free--;
                    phase_counter--;
                    if (phase_counter == 0) {
                        state = STATE_DATA;
                        bit_index = -1;
                        playback_data_bit_active = false;
                        set_log("CMT Data");
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
        reset_header_tracking();
        set_log("CMT Seek 0");

        playback_base_time_ms = 0;
        playback_tag_start_system_ms = millis();
        playback_time_valid = true;

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