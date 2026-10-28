#include "cmt_cas_converter.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>

#include <functional>

#include "cmt_globals.h"
#include "cmt_sd_buffer.h"
#include "cmt_ui.h"
#include "hardware/watchdog.h"

// MSX CASファイルのブロック区切りを示す8バイトのシグネチャ
static const uint8_t CAS_SIGNATURE[8] = {0x1F, 0xA6, 0xDE, 0xBA,
                                         0xCC, 0x13, 0x7D, 0x74};

/**
 * @brief MSX CAS変換に特化した軽量FSKデモジュレータ
 * 無音明けのキャリア周波数(2400Hz/4800Hz)を測定し、1200ボー/2400ボーを自動判別します。
 */
class CasFskDemodulator {
   public:
    enum BaudRate { BAUD_UNKNOWN, BAUD_1200, BAUD_2400 };
    BaudRate baud_rate = BAUD_UNKNOWN;

    // ボーレートに応じた判定パラメータ
    uint32_t current_window_us;
    uint32_t space_threshold_us;
    uint32_t dpll_min_us;
    uint32_t dpll_max_us;

    // キャリア周波数測定用
    int consecutive_mark_count = 0;
    uint32_t consecutive_pulse_sum = 0;
    bool carrier_detected = false;

    // UARTサンプリング用ステータス
    uint64_t window_start_us = 0;
    int uart_state = 0;
    int bit_index = 0;
    uint8_t current_byte = 0;

    bool sample_taken = false;
    int sampled_bit_val = -1;

    uint64_t current_time_us = 0;

    int uart_start_space_count = 0;
    uint64_t first_space_start_us = 0;

    bool has_pending_half = false;
    bool pending_is_high = false;
    uint32_t pending_us = 0;
    uint64_t last_edge_time_us = 0;

    std::function<void(uint8_t)> on_byte_decoded = nullptr;

    CasFskDemodulator() { reset(); }

    void reset() {
        baud_rate = BAUD_UNKNOWN;
        uart_state = 0;
        uart_start_space_count = 0;
        first_space_start_us = 0;
        has_pending_half = false;
        pending_us = 0;
        sample_taken = false;
        sampled_bit_val = -1;
        consecutive_mark_count = 0;
        consecutive_pulse_sum = 0;
        carrier_detected = false;
    }

    void advance_gap(uint32_t gap_us) {
        current_time_us += gap_us;
        reset();
    }

    void feed_pulse(bool is_high, uint32_t pulse_us) {
        // 無音ギャップ検出で状態リセット (ブロック区切り)
        if (pulse_us > 2000) {
            reset();
            current_time_us += pulse_us;
            return;
        }

        // 半波合成
        uint32_t full_us = 0;
        uint32_t prev_half_us = 0;
        if (!has_pending_half) {
            pending_is_high = is_high;
            pending_us = pulse_us;
            has_pending_half = true;
        } else {
            if (pending_is_high != is_high) {
                full_us = pending_us + pulse_us;
                prev_half_us = pending_us;
                pending_is_high = is_high;
                pending_us = pulse_us;
            } else {
                pending_us += pulse_us;
            }
        }

        last_edge_time_us = current_time_us;
        current_time_us += pulse_us;

        // エッジ反転時 (full_us 確定時) のみDPLLまたはキャリア判定を実施
        if (full_us > 0) {
            if (!carrier_detected) {
                // キャリアの周波数(1周期の長さ)を測定
                // MSXのMarkキャリア周期 (2400baud:約208us, 1200baud:約416us)
                if (full_us > 100 && full_us < 550) {
                    consecutive_mark_count++;
                    consecutive_pulse_sum += full_us;

                    // 16周期分(数ms)安定して受信できたらボーレートを確定
                    if (consecutive_mark_count >= 16) {
                        uint32_t avg_full_us = consecutive_pulse_sum / 16;
                        if (avg_full_us < 312) {
                            // 208us(4800Hz)寄りなので2400ボーと判定
                            baud_rate = BAUD_2400;
                            current_window_us = 416;
                            space_threshold_us = 156;
                            dpll_min_us = 312;
                            dpll_max_us = 520;
                        } else {
                            // 416us(2400Hz)寄りなので1200ボーと判定
                            baud_rate = BAUD_1200;
                            current_window_us = 833;
                            space_threshold_us = 312;
                            dpll_min_us = 625;
                            dpll_max_us = 1041;
                        }
                        carrier_detected = true;
                        uart_state = 0;
                    }
                } else {
                    consecutive_mark_count = 0;
                    consecutive_pulse_sum = 0;
                }
                return;  // キャリア未検出時はここで終了
            } else {
                // キャリア検出済みなら、DPLLでウィンドウ幅をテープ速度に追従
                // 同じ種類の半周期が2つ並んだ組だけで測る。Mark と Space が混ざった組
                // (スタートビットの境目など) は1ビットの長さと関係がない
                bool prev_space = (prev_half_us >= space_threshold_us);
                bool cur_space = (pulse_us >= space_threshold_us);
                if (uart_state == 0 && prev_space == cur_space) {  // アイドル(Mark)中のみ
                    uint32_t est_win = cur_space ? full_us : (full_us * 2);
                    if (est_win > dpll_min_us && est_win < dpll_max_us) {
                        current_window_us =
                            (current_window_us * 15 + est_win) / 16;
                    }
                }
            }
        }

        if (!carrier_detected) {
            return;
        }

        bool is_space = (pulse_us >= space_threshold_us);

        // 3. ウィンドウ判定とビット抽出
        if (uart_state == 0) {
            if (is_space) {
                if (uart_start_space_count == 0) {
                    first_space_start_us = last_edge_time_us;
                }
                uart_start_space_count++;

                // Space半波が2回連続でスタートビット確定
                if (uart_start_space_count == 2) {
                    uart_state = 1;
                    window_start_us = first_space_start_us;
                    bit_index = -1;
                    sample_taken = true;
                    sampled_bit_val = 0;
                }
            } else {
                uart_start_space_count = 0;
            }
        } else {
            uint32_t bit_period_us = current_window_us;

            // 中央サンプリング
            uint64_t sample_time = window_start_us + bit_period_us / 2;
            if (!sample_taken && last_edge_time_us <= sample_time &&
                current_time_us > sample_time) {
                sampled_bit_val = is_space ? 0 : 1;
                sample_taken = true;
            }

            while (current_time_us >= window_start_us + bit_period_us) {
                if (!sample_taken) {
                    sampled_bit_val = is_space ? 0 : 1;
                }

                int bit_val = sampled_bit_val;

                if (bit_index == -1) {
                    if (bit_val == 1) {
                        uart_state = 0;  // False start
                        break;
                    } else {
                        bit_index = 0;
                        current_byte = 0;
                    }
                } else if (bit_index >= 0 && bit_index <= 7) {
                    if (bit_val == 1) {
                        current_byte |= (1 << bit_index);
                    }
                    bit_index++;
                } else if (bit_index == 8) {
                    if (on_byte_decoded) {
                        on_byte_decoded(current_byte);
                    }
                    uart_state = 0;
                    break;
                }

                // DPLL位相同期
                int64_t target = (int64_t)(window_start_us + bit_period_us);
                int64_t err1 = (int64_t)last_edge_time_us - target;
                int64_t err2 = (int64_t)current_time_us - target;
                int64_t abs_err1 = (err1 < 0) ? -err1 : err1;
                int64_t abs_err2 = (err2 < 0) ? -err2 : err2;

                int64_t phase_error = (abs_err1 < abs_err2) ? err1 : err2;

                if (phase_error > -100 && phase_error < 100) {
                    window_start_us += bit_period_us + (phase_error / 8);
                } else {
                    window_start_us += bit_period_us;
                }

                sample_taken = false;
                sampled_bit_val = -1;

                // 次のサンプリング点が既に経過している場合の処理
                sample_time = window_start_us + bit_period_us / 2;
                if (last_edge_time_us <= sample_time &&
                    current_time_us > sample_time) {
                    sampled_bit_val = is_space ? 0 : 1;
                    sample_taken = true;
                }
            }
        }
    }
};

bool convert_raw_to_cas(const char* raw_path, const char* cas_path) {
    if (raw_path == nullptr || cas_path == nullptr) {
        return false;
    }

    File raw_file = SD.open(raw_path, FILE_READ);
    if (!raw_file) {
        return false;
    }

    uint32_t total_size = raw_file.size();
    if (total_size == 0) {
        raw_file.close();
        return false;
    }

    if (SD.exists(cas_path)) {
        SD.remove(cas_path);
    }
    File cas_file = SD.open(cas_path, FILE_WRITE);
    if (!cas_file) {
        raw_file.close();
        return false;
    }

    SdBufferedRead* reader = new SdBufferedRead(&raw_file, 4096);
    SdBufferedWrite* writer = new SdBufferedWrite(&cas_file, 4096);

    draw_conversion_progress(0);

    // キャリア周波数から自動測定してボーレートを決めるため初期化時の指定は不要
    CasFskDemodulator demod;

    uint64_t last_byte_time = 0;
    uint32_t written = 0;  // 書き込んだバイト数 (シグネチャの位置合わせ用)
    bool write_ok = true;  // SDへの書き込みに失敗したら false

    auto write = [&](const uint8_t* data, size_t len) {
        if (!writer->write_bytes(data, len)) {
            write_ok = false;
        }
        written += len;
    };

    demod.on_byte_decoded = [&](uint8_t byte_val) {
        // ドロップアウト対策: 無音ギャップの判定を 1秒(1000000us) に延長
        if (last_byte_time == 0 ||
            demod.current_time_us - last_byte_time > 1000000) {
            // シグネチャは8の倍数の位置に置く (そうでないとヘッダとして認識されない)
            static const uint8_t PADDING[8] = {0};
            if (written % 8 != 0) {
                write(PADDING, 8 - (written % 8));
            }
            write(CAS_SIGNATURE, 8);
        }
        write(&byte_val, 1);
        last_byte_time = demod.current_time_us;
    };

    uint16_t raw_word = 0;
    uint32_t loop_count = 0;
    int last_percent = -1;

    while (write_ok && reader->read_word(raw_word)) {
        loop_count++;
        if ((loop_count & 0x01FF) == 0) {
            watchdog_update();
            int percent = ((loop_count * 2) * 100) / total_size;
            if (percent > 100) {
                percent = 100;
            }
            if (percent != last_percent) {
                draw_conversion_progress(percent);
                last_percent = percent;
            }
        }

        if (raw_word == 0x0000) {
            continue;
        }
        if (raw_word == 0xFFFF) {
            uint16_t ms_word = 100;
            if (!reader->read_word(ms_word)) {
                ms_word = 100;
            }
            loop_count++;
            demod.advance_gap(ms_word * 1000);
            continue;
        }

        bool is_high = (raw_word & 0x8000) != 0;
        uint32_t pulse_us = raw_pulse_us(raw_word);
        demod.feed_pulse(is_high, pulse_us);
    }

    if (!writer->flush()) {
        write_ok = false;
    }
    delete writer;
    delete reader;
    raw_file.close();
    cas_file.close();

    draw_conversion_progress(100);
    return write_ok;
}