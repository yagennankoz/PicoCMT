#include "player_cas.h"

#include <string.h>

#include "cmt_globals.h"

// MSX CASファイルのブロック区切りを示す8バイトのシグネチャ
static const uint8_t CAS_SIGNATURE[8] = {0x1F, 0xA6, 0xDE, 0xBA,
                                         0xCC, 0x13, 0x7D, 0x74};

// PIOプログラムがパルスを出力する際にかかる命令オーバーヘッド(約7us)
// MSXの2400ボーは非常にシビアなため、この分を差し引いて波形を補正する
#define PIO_OVERHEAD_US 7

PlayerCAS::PlayerCAS() {
    cmt_file = nullptr;
    buffered_reader = nullptr;
    baud_rate = 1200;
    mark_pulse_us = 208 - PIO_OVERHEAD_US;
    space_pulse_us = 417 - PIO_OVERHEAD_US;
}

PlayerCAS::~PlayerCAS() {
    if (buffered_reader) {
        delete buffered_reader;
    }
}

void PlayerCAS::set_baud_rate(int baud) {
    baud_rate = baud;
    if (baud == 2400) {
        mark_pulse_us = 104 - PIO_OVERHEAD_US;
        space_pulse_us = 208 - PIO_OVERHEAD_US;
    } else {
        mark_pulse_us = 208 - PIO_OVERHEAD_US;
        space_pulse_us = 417 - PIO_OVERHEAD_US;
    }
}

void PlayerCAS::set_log(const char* text) {
    snprintf(playback_log, sizeof(playback_log), "%s", text);
}

bool PlayerCAS::enqueue_pulse(queue_t* q, uint16_t us) {
    if (queue_get_level(q) >= PLAY_QUEUE_LENGTH) {
        return false;
    }
    bool res = queue_try_add(q, &us);
    if (res) {
        total_enqueued_pulses++;
    }
    return res;
}

bool PlayerCAS::init(File* f, const char* filename) {
    cmt_file = f;

    if (buffered_reader) {
        delete buffered_reader;
    }
    buffered_reader = new SdBufferedRead(cmt_file, 4096);

    if (!cmt_file || cmt_file->size() == 0) {
        return false;
    }

    buffered_reader->seek(0);

    end_reached = false;
    exhausted = false;
    state = STATE_INIT;
    phase_counter = 0;
    current_file_pos = 0;
    bit_index = -1;
    pulse_count = 0;
    start_ms = millis();

    // 最初の8バイトをウィンドウに先読みする
    window_filled = buffered_reader->read(window, 8);

    init_position_tracking();

    set_log("CAS Init");
    return true;
}

void PlayerCAS::process(queue_t* play_queue) {
    if (end_reached) {
        if (!exhausted) {
            exhausted = true;
        }
        return;
    }

    int phase_multiplier = (baud_rate == 2400) ? 2 : 1;
    int iterations = 0;
    while ((PLAY_QUEUE_LENGTH - queue_get_level(play_queue)) > 16) {
        if (++iterations > 256) {
            break;
        }

        switch (state) {
            case STATE_INIT:
                // 先頭がシグネチャかどうかチェック
                if (window_filled == 8 &&
                    memcmp(window, CAS_SIGNATURE, 8) == 0) {
                    // シグネチャなら読み捨てて、次のデータをウィンドウに入れる
                    window_filled = buffered_reader->read(window, 8);

                    // MSXのヘッダブロック(EA, D0,
                    // D3で始まる)の場合はロングリーダー(約3秒)
                    // そうでなければデータブロックなのでショートリーダー(約1.5秒)
                    if (window_filled > 0 &&
                        (window[0] == 0xEA || window[0] == 0xD0 ||
                         window[0] == 0xD3)) {
                        phase_counter = 3600 * phase_multiplier;
                    } else {
                        phase_counter = 1800 * phase_multiplier;
                    }
                } else {
                    // 標準外だがシグネチャがなければロングリーダーで強行
                    phase_counter = 3600 * phase_multiplier;
                }

                state = STATE_BLANK;
                start_ms = millis();
                enqueue_pulse(play_queue, 2);
                set_log("CAS Blank");
                break;

            case STATE_BLANK:
                // ブロックの前に1秒間の無音（モーター動作の猶予）を挟む
                if (millis() - start_ms >= 1000) {
                    state = STATE_HEADER;
                    pulse_count = 0;
                    set_log("CAS Header");
                } else {
                    return;
                }
                break;

            case STATE_HEADER:
                // リーダー信号（MARK）の出力
                if (pulse_count < 4) {
                    if (enqueue_pulse(play_queue, mark_pulse_us)) {
                        pulse_count++;
                    }
                } else {
                    pulse_count = 0;
                    phase_counter--;
                    if (phase_counter == 0) {
                        state = STATE_DATA;
                        bit_index = -1;
                        set_log("CAS Data");
                    }
                }
                break;

            case STATE_DATA: {
                if (window_filled == 0) {
                    // ファイル終端に到達
                    state = STATE_TRAILER;
                    phase_counter =
                        1200 * phase_multiplier;  // 約1秒のトレーラー
                    pulse_count = 0;
                    set_log("CAS Trailer");
                    break;
                }

                // 1バイト(11ビット)のデータ出力
                bool bit_val;
                if (bit_index == -1) {
                    bit_val = false;  // スタートビット(0)
                } else if (bit_index >= 8) {
                    bit_val = true;  // ストップビット(1) x 2回
                } else {
                    bit_val = (window[0] >> bit_index) & 1;
                }

                int target_pulses = bit_val ? 4 : 2;
                uint16_t pulse_us = bit_val ? mark_pulse_us : space_pulse_us;

                if (pulse_count < target_pulses) {
                    if (enqueue_pulse(play_queue, pulse_us)) {
                        pulse_count++;
                    }
                } else {
                    pulse_count = 0;
                    bit_index++;
                    if (bit_index > 9) {
                        bit_index = -1;
                        current_file_pos =
                            buffered_reader->position() - window_filled;

                        // ウィンドウを1バイトシフトし、次の1バイトを補充
                        memmove(window, window + 1, 7);
                        uint8_t next_b;
                        if (buffered_reader->read_byte(next_b)) {
                            window[7] = next_b;
                        } else {
                            window_filled--;
                        }

                        // 新しいブロックのシグネチャを発見したか？
                        if (window_filled == 8 &&
                            memcmp(window, CAS_SIGNATURE, 8) == 0) {
                            // シグネチャを読み捨てる
                            window_filled = buffered_reader->read(window, 8);
                            if (window_filled > 0 &&
                                (window[0] == 0xEA || window[0] == 0xD0 ||
                                 window[0] == 0xD3)) {
                                phase_counter = 3600 * phase_multiplier;
                            } else {
                                phase_counter = 1800 * phase_multiplier;
                            }
                            state = STATE_BLANK;
                            start_ms = millis();
                            set_log("CAS Blank");
                            break;  // 現在のバイト処理を終え、STATE_BLANKに遷移
                        }
                    }
                }
                break;
            }

            case STATE_TRAILER:
                if (pulse_count < 4) {
                    if (enqueue_pulse(play_queue, mark_pulse_us)) {
                        pulse_count++;
                    }
                } else {
                    pulse_count = 0;
                    phase_counter--;
                    if (phase_counter == 0) {
                        state = STATE_END;
                        set_log("CAS End");
                    }
                }
                break;

            case STATE_END:
                end_reached = true;
                return;
        }
    }
}

bool PlayerCAS::seek(int direction, queue_t* play_queue) {
    if (direction < 0) {
        buffered_reader->seek(0);
        state = STATE_INIT;
        end_reached = false;
        exhausted = false;
        // 先頭に戻った際もウィンドウを再読み込みする
        window_filled = buffered_reader->read(window, 8);
        set_log("CAS Seek 0");
        return true;
    } else {
        end_reached = true;
        return true;
    }
}

void PlayerCAS::stop() {}

bool PlayerCAS::is_end_reached() { return end_reached; }
bool PlayerCAS::is_exhausted() { return exhausted; }

void PlayerCAS::get_status(PlaybackStatus* out) {
    out->playback_t88_mode = false;
    out->playback_t88_end_reached = end_reached;
    out->playback_source_exhausted = exhausted;
    out->file_position = current_file_pos;
    out->file_size = cmt_file ? cmt_file->size() : 0;
    snprintf(out->playback_log, sizeof(out->playback_log), "%s", playback_log);
}
