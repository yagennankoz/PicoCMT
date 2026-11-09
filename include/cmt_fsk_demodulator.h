#pragma once

#include <Arduino.h>

#include <functional>
#include <memory>

// ----------------------------------------------------------------------------
// 1バイトを読むUARTの処理。FskDemodulatorの本体と、ボーレートを決めるための
// 試し読み(600ボー/1200ボー)で共通に使う
// ----------------------------------------------------------------------------
struct FskUart {
    uint64_t current_time_us = 0;
    uint64_t last_edge_time_us = 0;
    uint64_t window_start_us = 0;

    int uart_state = 0;
    int bit_index = 0;
    uint8_t current_byte = 0;

    // UARTサンプリング用
    bool sample_taken = false;
    int sampled_bit_val = -1;
    bool previous_was_mark = true;

    // スタートビットのフライング防止用
    int uart_start_space_count = 0;
    uint64_t first_space_start_us = 0;

    // 1バイトはデータ8bitの後の2ビット(9・10ビット目)まで読む。
    // パリティ無しならどちらもストップビット(1)、パリティ付き(ストップビット1つ)なら
    // 9ビット目がパリティ、10ビット目がストップビット。どちらも1バイトは11ビット
    // 読み終えたバイトの数と、形の崩れの数:
    //   framing_errors:9ビット目が0(パリティ無しとして崩れている)
    //   even_errors/odd_errors:偶数/奇数パリティとして合わないか、10ビット目が0
    uint32_t byte_count = 0;
    uint32_t framing_errors = 0;
    uint32_t even_errors = 0;
    uint32_t odd_errors = 0;
    // 最後に読み終えたバイトの形が崩れていないか(9・10ビット目のどちらかが1)
    bool last_framing_ok = false;
    // 最後に読み終えたバイトの9ビット目(パリティ付きならパリティビット)
    int last_bit9 = 1;
    // 読み中のバイトの9ビット目
    int bit9_ = 1;
    // 続いているSpaceの半周期の数と、その始まりの時刻
    int space_run_ = 0;
    uint64_t space_run_start_ = 0;

    // パリティ無し・偶数・奇数パリティのうち、いちばん合っている形での崩れの数
    uint32_t best_errors() const {
        uint32_t e = framing_errors;
        if (even_errors < e) {
            e = even_errors;
        }
        if (odd_errors < e) {
            e = odd_errors;
        }
        return e;
    }

    // 9・10ビット目まで読んでバイトを数える
    void count_frame(int bit10) {
        byte_count++;
        int parity = __builtin_popcount(current_byte) & 1;  // 偶数パリティのビット
        bool stop_ok = (bit10 == 1);
        if (bit9_ != 1) {
            framing_errors++;
        }
        if (!(stop_ok && bit9_ == parity)) {
            even_errors++;
        }
        if (!(stop_ok && bit9_ != parity)) {
            odd_errors++;
        }
        last_framing_ok = (bit9_ == 1) || stop_ok;
        last_bit9 = bit9_;
    }

    // 10ビット目の値でバイトを確定し、次のスタートビットを待つ。
    // 10ビット目が0なら、次のスタートビットがもう始まっているかもしれない
    // (ストップビット1つ・パリティ無しのデータなど)。待機中と同じく、Spaceの半周期が
    // 2つ続いていればスタートビットとする。ここまでのSpaceの半周期を数えに入れる
    void end_frame(int bit10) {
        count_frame(bit10);
        uart_state = 0;
        uart_start_space_count = 0;
        if (bit9_ == 0 && bit10 == 0) {
            // 形が崩れている(低い音が続く雑音など)。Markを挟むまで次のバイトを読まない
            uart_start_space_count = 2;
            return;
        }
        if (bit10 != 0 || space_run_ == 0) {
            return;
        }
        uint64_t bit10_start = window_start_us;
        uart_start_space_count = space_run_;
        first_space_start_us = space_run_start_;
        if (first_space_start_us < bit10_start) {
            // 9ビット目から続くSpaceは、10ビット目の頭から数える
            first_space_start_us = bit10_start;
            uart_start_space_count = 1;
        }
        if (uart_start_space_count >= 2) {
            uart_state = 1;
            window_start_us = first_space_start_us;
            bit_index = -1;
            sample_taken = true;
            sampled_bit_val = 0;
        }
    }

    // 信号が途切れたとき、10ビット目を待っているバイトがあれば確定する
    bool take_pending_byte(uint8_t* out) {
        if (uart_state == 1 && bit_index == 9) {
            count_frame(1);
            *out = current_byte;
            uart_state = 0;
            return true;
        }
        return false;
    }
    // バイトの外でMarkが続いている時間(ボーレートを決め直すのに使う)
    uint32_t idle_mark_us = 0;

    void reset_uart() {
        uart_state = 0;
        uart_start_space_count = 0;
        first_space_start_us = 0;
        previous_was_mark = true;
        sample_taken = false;
        sampled_bit_val = -1;
        idle_mark_us = 0;
    }

    // 半周期を1つ処理する。1バイト読み終えたらtrueを返し、*outにその値を入れる
    bool step(uint32_t pulse_us, bool carrier_detected, uint32_t bit_period_us, uint8_t* out) {
        bool done = false;
        last_edge_time_us = current_time_us;
        current_time_us += pulse_us;
        bool is_space = (pulse_us >= 312);
        if (is_space) {
            if (space_run_ == 0) {
                space_run_start_ = last_edge_time_us;
            }
            space_run_++;
        } else {
            space_run_ = 0;
        }

        if (uart_state == 0) {
            idle_mark_us = is_space ? 0 : idle_mark_us + pulse_us;
        } else {
            idle_mark_us = 0;
        }

        // ウィンドウ判定とビット抽出
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
                        // Spaceのパルスが2回連続したので、スタートビットと確定する
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
            // ビットの中央（サンプリングポイント）を通過したら、その瞬間の周波数(Space/Mark)を記録
            uint64_t sample_time = window_start_us + bit_period_us / 2;
            if (!sample_taken && last_edge_time_us <= sample_time &&
                current_time_us > sample_time) {
                sampled_bit_val = is_space ? 0 : 1;
                sample_taken = true;
            }

            // ビットの境界を越えた場合の処理
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
                    // 9ビット目(1つ目のストップビットか、パリティビット)
                    bit9_ = bit_val;
                    bit_index++;
                } else if (bit_index == 9) {
                    // 10ビット目の中央を飛び越えた長いパルス。ここで確定する
                    done = true;
                    *out = current_byte;
                    end_frame(bit_val);
                    previous_was_mark = (bit_val == 1);
                    break;  // 1バイト完了時はwhileを抜けて次のスタートビットを待つ
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
                if (last_edge_time_us <= sample_time && current_time_us > sample_time) {
                    sampled_bit_val = is_space ? 0 : 1;
                    sample_taken = true;
                }
            }

            // 10ビット目は中央で値を取ったら、ビットの終わりを待たずに確定する。
            // 終わりまで待つと、すぐ後のスタートビットの頭の半周期を取りこぼすことがある
            if (uart_state == 1 && bit_index == 9 && sample_taken) {
                done = true;
                *out = current_byte;
                end_frame(sampled_bit_val);
            }
        }

        if (uart_state == 0) {
            previous_was_mark = !is_space;
        }
        return done;
    }
};

// ----------------------------------------------------------------------------
// PC-8801/PC-6001のFSK(1200Hz/2400Hz)の復調器。ボーレート(600/1200)は
// 区間ごとに自動で判定する
// ----------------------------------------------------------------------------
class FskDemodulator : public FskUart {
   public:
    enum BaudRate { BAUD_1200, BAUD_600 };
    BaudRate baud_rate = BAUD_1200;
    bool baud_locked = false;

    // ボーレートが決まるまで半周期の長さ(us)をためておき、決まったらUART処理へ流し込む。
    // 600ボーの1バイト(約90半周期)と、その後の区切りのMark(40ms=約190半周期)が入る大きさ。
    // 変換の関数は復調器をスタックに置くので、ためる場所はヒープから取る(2KB)
    static constexpr int BAUD_DETECT_PULSES = 512;
    std::unique_ptr<uint32_t[]> pulse_buffer{new uint32_t[BAUD_DETECT_PULSES]};
    int pulse_buffer_count = 0;
    bool buffering_baud = false;

    // バイトの外でMarkがこれだけ続いたら区間の終わりとみなし、次の区間でボーレートを決め直す。
    // 1つのブロックの途中でボーレートを変えるテープ(例: 600ボーの1バイトを
    // 約50msのMarkで挟む)に対応するため。T88のDATAタグの区切り(T88_DATA_GAP_US)と同じ
    static constexpr uint32_t BAUD_RESYNC_MARK_US = 40000;
    // 試し読みで、片方の形の崩れがもう片方よりこれだけ多ければ、区間の終わりを待たずに決める
    static constexpr uint32_t BAUD_DECIDE_ERROR_MARGIN = 2;
    // 1200ボーにしか出ない並びの前に、半端な連続(1200ボーのビットの区切りに合わない連続。雑音で
    // 崩れた所)がこの数以上あれば、すぐには決めない
    static constexpr int BAUD_NOISY_RUNS = 2;
    // その場合に600ボーにする条件:600ボーでこのバイト数を崩れずに読め、
    // 1200ボーではBAUD_DECIDE_ERROR_MARGIN以上崩れた(雑音で頭が崩れた600ボーの信号)
    static constexpr uint32_t BAUD_600_CLEAN_BYTES = 4;

    // Markキャリアの連続カウント用（ノイズ誤検知防止）
    int consecutive_mark_count = 0;
    bool carrier_detected = false;

    // 基本ウィンドウ長(理論値: 1200baud=4ticks=833.3us)
    uint32_t current_window_us = 833;

    // 半波合成とDPLL用
    bool has_pending_half = false;
    bool pending_is_high = false;
    uint32_t pending_us = 0;

    // 1バイト復調完了時のコールバック関数
    std::function<void(uint8_t)> on_byte_decoded = nullptr;

    void reset() {
        reset_uart();
        has_pending_half = false;
        pending_us = 0;

        // ギャップごとにロックを解除し、次のブロックで再評価する
        baud_locked = false;
        consecutive_mark_count = 0;
        carrier_detected = false;

        pulse_buffer_count = 0;
        buffering_baud = false;
    }

    void advance_gap(uint32_t gap_us) {
        finish();
        current_time_us += gap_us;
        reset();
    }

    // 信号の区切り(無音・ファイル終端)で呼ぶ。ボーレート判定中の半周期があれば
    // 手元の情報で判定してUART処理へ流し込む
    void finish() {
        if (buffering_baud && pulse_buffer_count > 0) {
            lock_baud_and_flush((BaudRate)judge_baud_rate(true));
        }
        uint8_t value;
        if (take_pending_byte(&value) && on_byte_decoded) {
            on_byte_decoded(value);
        }
    }

    void feed_pulse(bool is_high, uint32_t pulse_us) {
        // 長いパルス(ドロップアウト等)は区切りとして扱う。時間は経過させる
        if (pulse_us > 2000) {
            finish();
            current_time_us += pulse_us;
            reset();
            return;
        }

        // 1. 半波合成と速度推定(Tape Speed Tracking)
        if (!has_pending_half) {
            pending_is_high = is_high;
            pending_us = pulse_us;
            has_pending_half = true;
        } else {
            if (pending_is_high != is_high) {
                uint32_t full_us = pending_us + pulse_us;
                // 同じ種類の半周期が2つ並んだ組だけで測る。Mark と Space が混ざった組
                // (スタートビットの境目など) は1ビットの長さと関係がない
                bool prev_space = (pending_us >= 312);
                bool cur_space = (pulse_us >= 312);

                // アイドル中のみ速度学習を行う (データ中のジッタ排除)。
                // 1200ボーの並びを見つけて区間の終わりを待っている間は、UARTに流し込んでいないので、
                // 1200ボーの試し読みがバイトの外にいるかで見る
                int learn_state = wait_after_1200_run_ ? trial_[0].uart_state : uart_state;
                if (learn_state == 0 && prev_space == cur_space) {
                    uint32_t est_win = cur_space ? full_us : (full_us * 2);
                    // 異常値排除 (833us の ±25%)
                    if (est_win > 625 && est_win < 1041) {
                        current_window_us = (current_window_us * 15 + est_win) / 16;
                    }
                }
                pending_is_high = is_high;
                pending_us = pulse_us;
            } else {
                pending_us += pulse_us;
            }
        }

        bool is_space = (pulse_us >= 312);

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

        // 2.長いMarkで区間が終わったら、次の区間でボーレートを決め直す
        if (baud_locked && uart_state == 0 && idle_mark_us >= BAUD_RESYNC_MARK_US) {
            baud_locked = false;
        }

        // 3.ボーレート自動判定（バッファリングによる遅延評価）
        if (!baud_locked && carrier_detected) {
            if (is_space && pulse_buffer_count == 0) {
                start_trials();
            }
            if (buffering_baud) {
                pulse_buffer[pulse_buffer_count++] = pulse_us;

                // 最初のSpace(スタートビット)が1半波で終わった場合はノイズとみなして判定をやめる
                if (pulse_buffer_count == 2 && !is_space) {
                    abort_baud_detection();
                    return;
                }
                feed_trials(pulse_us, is_space);
                // 1200ボーにしか出ない並びは確かな証拠なので、すぐ1200ボーに決める。
                // ただしその前に半端な連続(雑音で崩れた所)がいくつもあるときは、600ボーの信号の頭が
                // 崩れただけのこともあるので、区間の終わりまでためて試し読みと合わせて決める
                int noisy_runs = 0;
                if (!wait_after_1200_run_ && has_1200_only_run(&noisy_runs)) {
                    if (noisy_runs < BAUD_NOISY_RUNS) {
                        lock_baud_and_flush(BAUD_1200);
                        return;
                    }
                    wait_after_1200_run_ = true;
                }
                int rate = judge_baud_rate(pulse_buffer_count >= BAUD_DETECT_PULSES);
                if (rate >= 0) {
                    lock_baud_and_flush((BaudRate)rate);
                }
                return;  // ためた半周期は流し込み済み、またはバッファリング中
            }
        }

        // 判定済み、またはデータ外のパルスはそのままUARTへ
        process_uart(pulse_us);
    }

    void process_uart(uint32_t pulse_us) {
        uint32_t bit_period_us =
            (baud_rate == BAUD_1200) ? current_window_us : (current_window_us * 2);
        uint8_t value;
        if (step(pulse_us, carrier_detected, bit_period_us, &value) && on_byte_decoded) {
            on_byte_decoded(value);
        }
    }

   private:
    // ボーレートを決めるための試し読み([0]=1200ボー、[1]=600ボー)
    FskUart trial_[2];
    // ためた半周期の末尾でMarkが続いている時間
    uint32_t trial_tail_mark_us_ = 0;
    // 1200ボーにしか出ない並びを見つけ、区間の終わりまでためている
    bool wait_after_1200_run_ = false;

    void start_trials() {
        buffering_baud = true;
        for (FskUart& t : trial_) {
            t = static_cast<const FskUart&>(*this);
            t.byte_count = 0;
            t.framing_errors = 0;
            t.even_errors = 0;
            t.odd_errors = 0;
        }
        trial_tail_mark_us_ = 0;
        wait_after_1200_run_ = false;
    }

    void feed_trials(uint32_t pulse_us, bool is_space) {
        uint8_t value;
        trial_[0].step(pulse_us, carrier_detected, current_window_us, &value);
        trial_[1].step(pulse_us, carrier_detected, current_window_us * 2, &value);
        trial_tail_mark_us_ = is_space ? 0 : trial_tail_mark_us_ + pulse_us;
    }

    // ボーレートを決める。まだ決められなければ-1。forceのときは必ず決める
    int judge_baud_rate(bool force) const {
        const FskUart& t1200 = trial_[0];
        const FskUart& t600 = trial_[1];
        // パリティ付きのデータもあるので、パリティ無し・偶数・奇数のうち合う形で数える
        uint32_t e1200 = t1200.best_errors();
        uint32_t e600 = t600.best_errors();
        // 雑音の後に1200ボーにしか出ない並びがあった区間は、区間の終わりまでためてから、
        // 600ボーで1つも崩れずに読めて1200ボーでは崩れが多いときだけ600ボーにする
        if (wait_after_1200_run_) {
            bool ended = force || trial_tail_mark_us_ >= BAUD_RESYNC_MARK_US;
            if (!ended) {
                return -1;
            }
            if (e600 == 0 && t600.byte_count >= BAUD_600_CLEAN_BYTES &&
                e1200 >= BAUD_DECIDE_ERROR_MARGIN) {
                return BAUD_600;
            }
            return BAUD_1200;
        }
        // 片方だけ形が大きく崩れていれば、もう片方
        if (e1200 >= e600 + BAUD_DECIDE_ERROR_MARGIN && t600.byte_count >= 1) {
            return BAUD_600;
        }
        if (e600 >= e1200 + BAUD_DECIDE_ERROR_MARGIN && t1200.byte_count >= 1) {
            return BAUD_1200;
        }
        // 区間が長いMarkで終わったら、手元の情報で決める
        bool ended_by_mark = trial_tail_mark_us_ >= BAUD_RESYNC_MARK_US &&
                             t1200.uart_state == 0 && t600.uart_state == 0;
        if (ended_by_mark) {
            if (e1200 != e600) {
                return (e600 < e1200) ? BAUD_600 : BAUD_1200;
            }
            // どちらで読んでも形が崩れない(どちらで再生しても同じ波形になる)。
            // Markに挟まれた短い区間(600ボーで1〜2バイト)は600ボーとする
            // (600ボーの1バイトは、1200ボーの1〜2バイトと区別できない)
            if (t600.byte_count >= 1 && t600.byte_count <= 2) {
                return BAUD_600;
            }
            return decide_baud_rate();
        }
        // 無音や途切れで終わった区間(モーターが止まるときの雑音など)は、従来の判定にする
        if (force) {
            return decide_baud_rate();
        }
        return -1;
    }

    // 1ビットの半周期数は 1200ボー: Space 2 / Mark 4、600ボー: Space 4 / Mark 8。
    // 600ボーの連続数は Space が4の倍数、Mark が8の倍数にしかならないので、
    // それ以外 (Space 4n+2 / Mark 8n+4) は1200ボーにしか出ない。
    // リーダーやアイドルの長いMarkは判断材料にしない
    static bool is_1200_only_run(bool space, int n) {
        if (space) {
            return n <= 18 && (n % 4) == 2;
        }
        return n <= 40 && (n % 8) == 4;
    }

    // ためた半周期の中で、区切りが確定した連続 (次の切り替わりまで見えたもの) を調べる。
    // noisy_runsには、見つけた並びより前の半端な連続 (1200ボーのビットの区切りに合わない連続:
    // Spaceが奇数、Markが4の倍数でない) の数を返す
    bool has_1200_only_run(int* noisy_runs = nullptr) const {
        int run = 0;
        bool run_space = false;
        int noisy = 0;
        for (int i = 0; i < pulse_buffer_count; i++) {
            bool s = (pulse_buffer[i] >= 312);
            if (run > 0 && s != run_space) {
                if (is_1200_only_run(run_space, run)) {
                    if (noisy_runs) {
                        *noisy_runs = noisy;
                    }
                    return true;
                }
                if (run_space ? (run % 2) != 0 : (run % 4) != 0) {
                    noisy++;
                }
                run = 0;
            }
            run_space = s;
            run++;
        }
        return false;
    }

    // 区切りが確定した連続の数
    int completed_runs() const {
        int runs = 0;
        for (int i = 1; i < pulse_buffer_count; i++) {
            if ((pulse_buffer[i] >= 312) != (pulse_buffer[i - 1] >= 312)) {
                runs++;
            }
        }
        return runs;
    }

    // 試し読みで決まらなかった場合:十分な数の連続を見ていれば600ボー。
    // 判断材料が少ない場合は標準の1200ボーを優先する
    BaudRate decide_baud_rate() const {
        if (has_1200_only_run()) {
            return BAUD_1200;
        }
        return (completed_runs() >= 4) ? BAUD_600 : BAUD_1200;
    }

    void lock_baud_and_flush(BaudRate rate) {
        baud_rate = rate;
        baud_locked = true;
        flush_pulse_buffer();
    }

    void abort_baud_detection() { flush_pulse_buffer(); }

    // バッファされたパルスをUART処理へ流し込む
    void flush_pulse_buffer() {
        buffering_baud = false;
        wait_after_1200_run_ = false;
        int count = pulse_buffer_count;
        pulse_buffer_count = 0;
        for (int i = 0; i < count; i++) {
            process_uart(pulse_buffer[i]);
        }
    }
};
