#pragma once

#include <Arduino.h>

#include <functional>

struct PulseLog {
    bool is_high;
    uint32_t pulse_us;
};

class FskDemodulator {
   public:
    enum BaudRate { BAUD_1200, BAUD_600 };
    BaudRate baud_rate = BAUD_1200;
    bool baud_locked = false;

    // 遅延評価（バッファリング）用
    PulseLog pulse_buffer[64];
    int pulse_buffer_count = 0;
    bool buffering_baud = false;

    // Markキャリアの連続カウント用（ノイズ誤検知防止）
    int consecutive_mark_count = 0;
    bool carrier_detected = false;

    // 基本ウィンドウ長 (理論値: 1200baud=4ticks=833.3us)
    uint32_t current_window_us = 833;
    uint64_t window_start_us = 0;

    int uart_state = 0;
    int bit_index = 0;
    uint8_t current_byte = 0;

    // UARTサンプリング用
    bool sample_taken = false;
    int sampled_bit_val = -1;

    uint64_t current_time_us = 0;
    bool previous_was_mark = true;

    // スタートビットのフライング防止用
    int uart_start_space_count = 0;
    uint64_t first_space_start_us = 0;

    // 半波合成とDPLL用
    bool has_pending_half = false;
    bool pending_is_high = false;
    uint32_t pending_us = 0;
    uint64_t last_edge_time_us = 0;

    // 1バイト復調完了時のコールバック関数
    std::function<void(uint8_t)> on_byte_decoded = nullptr;

    void reset() {
        uart_state = 0;
        uart_start_space_count = 0;
        first_space_start_us = 0;
        previous_was_mark = true;
        has_pending_half = false;
        pending_us = 0;

        sample_taken = false;
        sampled_bit_val = -1;

        // ギャップごとにロックを解除し、次のブロックで再評価する
        baud_locked = false;
        consecutive_mark_count = 0;
        carrier_detected = false;

        pulse_buffer_count = 0;
        buffering_baud = false;
    }

    void advance_gap(uint32_t gap_us) {
        current_time_us += gap_us;
        reset();
    }

    void feed_pulse(bool is_high, uint32_t pulse_us) {
        // 1. 半波合成と速度推定(Tape Speed Tracking)
        if (!has_pending_half) {
            pending_is_high = is_high;
            pending_us = pulse_us;
            has_pending_half = true;
        } else {
            if (pending_is_high != is_high) {
                uint32_t full_us = pending_us + pulse_us;
                bool is_space_full = (full_us >= 625);

                // アイドル中のみ速度学習を行う (データ中のジッタ排除)
                if (uart_state == 0) {
                    uint32_t est_win = is_space_full ? full_us : (full_us * 2);
                    // 異常値排除 (833us の ±25%)
                    if (est_win > 625 && est_win < 1041) {
                        current_window_us =
                            (current_window_us * 15 + est_win) / 16;
                    }
                }
                pending_is_high = is_high;
                pending_us = pulse_us;
            } else {
                pending_us += pulse_us;
            }
        }

        bool is_space = (pulse_us >= 312);

        if (pulse_us > 2000) {
            reset();
            return;
        }

        // ノイズ対策: 有効なMarkキャリアが一定期間(16半波 =
        // 約3.3ms)連続して初めてデータ開始とみなす
        if (!is_space) {
            if (consecutive_mark_count < 100) {
                consecutive_mark_count++;
            }
            if (consecutive_mark_count >= 16) {
                carrier_detected = true;
            }
        } else {
            consecutive_mark_count = 0;
        }

        // 2. ボーレート自動判定（バッファリングによる遅延評価）
        if (!baud_locked && carrier_detected) {
            if (is_space && pulse_buffer_count == 0) {
                buffering_baud = true;
            }
            if (buffering_baud) {
                if (pulse_buffer_count < 64) {
                    pulse_buffer[pulse_buffer_count].is_high = is_high;
                    pulse_buffer[pulse_buffer_count].pulse_us = pulse_us;
                    pulse_buffer_count++;
                }

                int space_run = 0;
                int mark_run = 0;
                int state = 0;
                for (int i = 0; i < pulse_buffer_count; i++) {
                    bool p_is_space = (pulse_buffer[i].pulse_us >= 312);
                    if (state == 0) {
                        if (p_is_space) {
                            space_run++;
                        } else {
                            state = 1;
                            mark_run++;
                        }
                    } else if (state == 1) {
                        if (!p_is_space) {
                            mark_run++;
                        } else {
                            state = 2;
                            break;
                        }
                    }
                }

                // 最初のSpace区間とMark区間が両方揃った、またはバッファが一杯になったら判定
                if (state == 2 || pulse_buffer_count >= 64) {
                    if (space_run % 4 == 2 || mark_run % 8 == 4) {
                        // 1200ボーにしか出現し得ない半波長
                        baud_rate = BAUD_1200;
                    } else if (space_run > 18) {
                        // 1200ボーの最大連続Space長（Start+8bit分=18）を超えるため600ボー確定
                        baud_rate = BAUD_600;
                    } else if (space_run == 4 && mark_run >= 12) {
                        // 600ボーの典型的な 0xD3 ヘッダのパターン
                        baud_rate = BAUD_600;
                    } else {
                        // 曖昧な場合は標準の1200ボーを優先
                        baud_rate = BAUD_1200;
                    }

                    baud_locked = true;
                    buffering_baud = false;

                    // バッファされたパルスをUART処理へ流し込む
                    for (int i = 0; i < pulse_buffer_count; i++) {
                        process_uart(pulse_buffer[i].is_high,
                                     pulse_buffer[i].pulse_us);
                    }
                    pulse_buffer_count = 0;
                }
                return;  // バッファリング中はここで処理を打ち切る
            }
        }

        // 判定済み、またはデータ外のパルスはそのままUARTへ
        process_uart(is_high, pulse_us);
    }

    void process_uart(bool is_high, uint32_t pulse_us) {
        last_edge_time_us = current_time_us;
        current_time_us += pulse_us;
        bool is_space = (pulse_us >= 312);

        // 3. ウィンドウ判定とビット抽出
        if (uart_state == 0) {
            // スタートビットのフライングを防止する
            if (is_space) {
                // 有効なキャリアが検出された後のみスタートビットとして許可する
                if (carrier_detected) {
                    if (uart_start_space_count == 0) {
                        // 1回目のSpaceパルスの開始時刻を記憶
                        first_space_start_us = last_edge_time_us;
                    }
                    uart_start_space_count++;

                    if (uart_start_space_count == 2) {
                        // Spaceのパルスが2回連続したことで、本物のスタートビットと確定！
                        uart_state = 1;
                        // 記憶しておいた「1回目の開始時刻」をスタート地点にする
                        window_start_us = first_space_start_us;
                        bit_index = -1;

                        // スタートビット(SPACE=0)はすでに確定しているので事前サンプリング済とする
                        sample_taken = true;
                        sampled_bit_val = 0;
                    }
                }
            } else {
                // 途中でMarkに戻った場合は、ノイズ(過渡パルス)とみなしてリセット
                uart_start_space_count = 0;
            }
        } else {
            uint32_t bit_period_us = (baud_rate == BAUD_1200)
                                         ? current_window_us
                                         : (current_window_us * 2);

            // 1.
            // ビットの中央（サンプリングポイント）を通過したら、その瞬間の周波数(Space/Mark)を記録
            uint64_t sample_time = window_start_us + bit_period_us / 2;
            if (!sample_taken && last_edge_time_us <= sample_time &&
                current_time_us > sample_time) {
                sampled_bit_val = is_space ? 0 : 1;
                sample_taken = true;
            }

            // 2. ビットの境界を越えた場合の処理
            while (current_time_us >= window_start_us + bit_period_us) {
                // 万が一サンプリング点を飛び越えるような巨大ノイズがあった場合のフェイルセーフ
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
                    previous_was_mark = (bit_val == 1);
                    break;  // 1バイト完了時は while
                            // を抜けて次のスタートビットを待つ（Stopビットは無視）
                }

                // DPLLによる境界の位相同期
                int64_t target = (int64_t)(window_start_us + bit_period_us);
                int64_t err1 = (int64_t)last_edge_time_us - target;
                int64_t err2 = (int64_t)current_time_us - target;
                int64_t abs_err1 = (err1 < 0) ? -err1 : err1;
                int64_t abs_err2 = (err2 < 0) ? -err2 : err2;

                // より境界に近い方の誤差をフェーズエラーとして採用
                int64_t phase_error = (abs_err1 < abs_err2) ? err1 : err2;

                if (phase_error > -100 && phase_error < 100) {
                    window_start_us += bit_period_us + (phase_error / 8);
                } else {
                    window_start_us += bit_period_us;
                }

                // 次のビットの準備
                sample_taken = false;
                sampled_bit_val = -1;

                // もし現在のパルスが次のビットのサンプリング点をも跨いでいた場合は、即座にサンプリング
                sample_time = window_start_us + bit_period_us / 2;
                if (last_edge_time_us <= sample_time &&
                    current_time_us > sample_time) {
                    sampled_bit_val = is_space ? 0 : 1;
                    sample_taken = true;
                }
            }
        }

        if (uart_state == 0) {
            previous_was_mark = !is_space;
        }
    }
};