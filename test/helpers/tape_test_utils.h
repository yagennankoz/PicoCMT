// ============================================================================
// テープ信号のテスト用ユーティリティ (各テストスイートで共通)
//   - FSK (KCS系) 信号の生成
//   - 録音PIO (cmt_rec.pio) / 録音処理のモデル → RAWファイル
//   - Core1 + 再生PIO (cmt_play.pio) のモデル → 出力波形
//   - 出力波形を FSK としてバイト列に戻す参照デコーダ
// ============================================================================
#pragma once

#include <SD.h>
#include <math.h>
#include <unity.h>

#include <random>
#include <vector>

#include "cmt_globals.h"
#include "cmt_player.h"

struct Half {
    bool high;
    double us;
};

// ----------------------------------------------------------------------------
// FSK フォーマット
//   '1' = 短い半周期 x halves_one, '0' = 長い半周期 x halves_zero
//   (one_is_long のときは '1' が長い半周期、'0' が短い半周期)
//   長い半周期は long_half_us (0 なら短い半周期の2倍)
//   バイト = スタート(0) + 8bit(LSBファースト) + ストップ(1) x stop_bits
// ----------------------------------------------------------------------------
struct FskFormat {
    double short_half_us;
    int halves_one;
    int halves_zero;
    int stop_bits;
    bool one_is_long = false;
    double long_half_us = 0;

    double long_us() const { return long_half_us > 0 ? long_half_us : short_half_us * 2; }
    double half_us_of(int b) const { return (b != 0) == one_is_long ? long_us() : short_half_us; }
    int halves_of(int b) const { return b ? halves_one : halves_zero; }
};
// PC-8801 / MSX 1200ボー (2400Hz / 1200Hz)
static const FskFormat FSK_1200 = {1e6 / 2400 / 2, 4, 2, 2};
// FM-7 (1ビット = 1周期。'0' = 2400Hz, '1' = 1200Hz)
static const FskFormat FSK_FM7 = {1e6 / 2400 / 2, 2, 2, 2, true};
// XM7 が出力する FM-7 の信号 (BASIC_I.t77 の実測: 短い 25.6 / 長い 48.8 カウント x 9us)
static const FskFormat FSK_FM7_XM7 = {230.4, 2, 2, 2, true, 439.2};
// PC-8801 600ボー
static const FskFormat FSK_600 = {1e6 / 2400 / 2, 8, 4, 2};
// PC-6001 1200 / 600ボー (ストップビット3)
static const FskFormat FSK_P6_1200 = {1e6 / 2400 / 2, 4, 2, 3};
static const FskFormat FSK_P6_600 = {1e6 / 2400 / 2, 8, 4, 3};
// MSX 2400ボー (4800Hz / 2400Hz)
static const FskFormat FSK_2400 = {1e6 / 4800 / 2, 4, 2, 2};

// テープ速度を変えたフォーマット (speed > 1 で速い = 半周期が短い)
inline FskFormat at_speed(FskFormat f, double speed) {
    f.short_half_us /= speed;
    f.long_half_us /= speed;
    return f;
}

class TapeSignal {
   public:
    std::vector<Half> halves;

    explicit TapeSignal(FskFormat f, double jitter = 0.0, uint32_t seed = 1)
        : fmt_(f), jitter_(jitter), rng_(seed) {}

    void bit(int b) {
        double us = fmt_.half_us_of(b);
        for (int i = 0; i < fmt_.halves_of(b); i++) {
            half(us, true);
        }
    }
    void byte(uint8_t v) {
        bit(0);
        for (int j = 0; j < 8; j++) {
            bit((v >> j) & 1);
        }
        for (int i = 0; i < fmt_.stop_bits; i++) {
            bit(1);
        }
    }
    void bytes(const std::vector<uint8_t>& data) {
        for (uint8_t v : data) {
            byte(v);
        }
    }
    void ones(int n) {
        for (int i = 0; i < n; i++) {
            bit(1);
        }
    }
    // 指定時間のマーク(リーダー)
    void leader_ms(double ms) {
        double bit_us = fmt_.half_us_of(1) * fmt_.halves_of(1);
        ones((int)round(ms * 1000.0 / bit_us));
    }
    void silence(double ms) { half(ms * 1000.0, false); }
    // 1つの長いパルス (ドロップアウト等)
    void pulse(double us) { half(us, false); }

   private:
    // ジッタはエッジ位置の揺らぎ (短い半周期 x jitter の範囲) として与える。
    // 揺らぎは時間軸に積み上がらない
    void half(double us, bool apply_jitter) {
        ideal_us_ += us;
        double edge = ideal_us_;
        if (apply_jitter && jitter_ > 0) {
            std::uniform_real_distribution<double> dist(-jitter_, jitter_);
            edge += dist(rng_) * fmt_.short_half_us;
        }
        halves.push_back({level_, edge - last_edge_us_});
        last_edge_us_ = edge;
        level_ = !level_;
    }

    FskFormat fmt_;
    double jitter_;
    std::mt19937 rng_;
    bool level_ = true;
    double ideal_us_ = 0;
    double last_edge_us_ = 0;
};

inline std::vector<uint8_t> random_bytes(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) {
        b = (uint8_t)(rng() & 0xFF);
    }
    return v;
}

// N88-BASIC のモニタの W名前 で保存したマシン語のヘッダ: "$$$" + ファイル名6バイト
// (X88000 で保存した T88 による。データは pc8001_machine_code と同じ形)
inline std::vector<uint8_t> n88_monitor_header(const char* name6) {
    std::vector<uint8_t> h = {0x24, 0x24, 0x24};
    h.insert(h.end(), name6, name6 + 6);
    return h;
}

// PC-8001 / 8801 のモニタの W コマンドで保存したマシン語 (名前は無い):
//   3A 開始アドレス(上・下) チェックサム、[3A 長さ データ チェックサム]...、3A 00 00
//   (r8format の cmtconv/platform/pc8001.py の説明による)
inline std::vector<uint8_t> pc8001_machine_code(uint16_t addr, size_t size, uint32_t seed) {
    std::vector<uint8_t> out = {0x3A, (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF)};
    out.push_back((uint8_t)(0x100 - ((out[1] + out[2]) & 0xFF)));
    std::vector<uint8_t> body = random_bytes(size, seed);
    for (size_t pos = 0; pos < body.size();) {
        size_t n = std::min<size_t>(255, body.size() - pos);
        uint32_t sum = (uint32_t)n;
        out.push_back(0x3A);
        out.push_back((uint8_t)n);
        for (size_t k = 0; k < n; k++) {
            out.push_back(body[pos + k]);
            sum += body[pos + k];
        }
        out.push_back((uint8_t)(0x100 - (sum & 0xFF)));
        pos += n;
    }
    out.push_back(0x3A);
    out.push_back(0x00);
    out.push_back(0x00);
    return out;
}

// ----------------------------------------------------------------------------
// cmt_rec.pio のサイクル単位モデル (1サイクル = 0.5us)
//   High: 立ち上がり検出c → mov isr, push, mov x, [jmp x--, jmp pin]...
//         立ち下がり検出 f = c+5+2k, 計測値 k+1
//   Low : f → mov isr, push, mov x, [jmp pin, jmp x--]...
//         立ち上がり検出 r = f+4+2j, 計測値 j
// ----------------------------------------------------------------------------
inline std::vector<uint32_t> rec_pio_measure(const std::vector<Half>& sig) {
    std::vector<uint32_t> out;
    double t = 0;
    int64_t c = 0;
    for (const Half& h : sig) {
        t += h.us * 2;
        int64_t base = c + (h.high ? 5 : 4);
        int64_t k = (int64_t)ceil((t - base) / 2);
        if (k < 0) {
            k = 0;
        }
        out.push_back((uint32_t)(h.high ? k + 1 : k));
        c = base + 2 * k;
    }
    return out;
}

// 録音処理 (core1_rec_loop / handle_raw_recording_stream) と同じ規則でRAWを作る
// 戻り値: RAWに記録された最初の半周期の位置
inline size_t write_raw(const char* path, const std::vector<Half>& sig) {
    std::vector<uint32_t> m = rec_pio_measure(sig);
    std::vector<uint8_t> bytes;
    auto put = [&](uint16_t w) {
        bytes.push_back((uint8_t)(w & 0xFF));
        bytes.push_back((uint8_t)(w >> 8));
    };

    int valid = 0;
    bool triggered = false;
    size_t first = 0;
    for (size_t i = 0; i < sig.size(); i++) {
        uint32_t ticks = m[i];
        if (ticks < 3) {
            continue;
        }
        if (!triggered) {
            if (ticks >= REC_VALID_PULSE_MIN_TICKS && ticks <= REC_VALID_PULSE_MAX_TICKS) {
                if (++valid >= 50) {
                    triggered = true;
                    first = i + 1;
                }
            } else {
                valid = 0;
            }
            continue;
        }
        if (ticks >= REC_SILENCE_GAP_TICKS) {
            uint32_t ms = ticks / 1000;
            put(0xFFFF);
            put((uint16_t)(ms > 65534 ? 65534 : ms));
        } else {
            uint16_t w = (ticks >= 0x7FFF) ? 0x7FFE : (uint16_t)ticks;
            put(w | (sig[i].high ? 0x8000 : 0));
        }
    }
    SD.put(path, bytes);
    return first;
}

// ----------------------------------------------------------------------------
// 波形ユーティリティ
// ----------------------------------------------------------------------------
inline std::vector<Half> merge_levels(const std::vector<Half>& in) {
    std::vector<Half> out;
    for (const Half& h : in) {
        if (!out.empty() && out.back().high == h.high) {
            out.back().us += h.us;
        } else {
            out.push_back(h);
        }
    }
    return out;
}

// 無音 (短いパルスの連続) の後で、信号が High と Low のどちらの半周期から始まるか
// (無音の後ごとに1つ。PIO の値の数が奇数だとブロックごとに位相が入れ替わる)
inline std::vector<bool> levels_after_silence(const std::vector<Half>& wave, double tiny_us = 30,
                                              int min_tiny = 100) {
    std::vector<bool> out;
    int tiny = 0;
    for (const Half& h : wave) {
        if (h.us < tiny_us) {
            tiny++;
            continue;
        }
        if (tiny >= min_tiny) {
            out.push_back(h.high);
        }
        tiny = 0;
    }
    return out;
}

// 無音の代わりに出している短いパルスの連続 (値2 → 約9us) を、1つの無音にまとめる
inline std::vector<Half> normalize_silence(const std::vector<Half>& in, double tiny_us = 30) {
    std::vector<Half> out;
    bool in_tiny = false;
    for (const Half& h : in) {
        if (h.us < tiny_us) {
            if (in_tiny) {
                out.back().us += h.us;
            } else {
                out.push_back({false, h.us});
                in_tiny = true;
            }
        } else {
            out.push_back(h);
            in_tiny = false;
        }
    }
    return merge_levels(out);
}

// ----------------------------------------------------------------------------
// Core1 + 再生PIO のモデルでプレーヤーを動かし、出力波形を得る
//   - start_playback_file() と同じく、最初にキューをプレチャージする
//   - キューが空なら前回値を2つずつ繰り返す (cmt_core1.cpp と同じ)
//   - PIO は最初の値をHighで出力し、値ごとに反転、時間は y+7us(High) / y+6us(Low)
//   - millis() は出力した時間に合わせて進める
// ----------------------------------------------------------------------------
inline std::vector<Half> run_playback(CmtPlayer& player, double step_ms = 5.0,
                                      double max_seconds = 600.0) {
    queue_t q;
    queue_init(&q, sizeof(uint16_t), PLAY_QUEUE_LENGTH);
    mock_millis_value = 0;

    for (int i = 0; i < 128 && queue_get_level(&q) < BUFFER_SIZE * 2 &&
                    !player.is_end_reached();
         i++) {
        player.process(&q);
    }

    std::vector<Half> wave;
    bool level = true;
    uint16_t last = 0xFFFF;
    double t_us = 0;
    auto emit = [&](uint16_t v) {
        double us = v + (level ? 7.0 : 6.0);
        wave.push_back({level, us});
        level = !level;
        t_us += us;
    };

    while (!(player.is_exhausted() && queue_is_empty(&q))) {
        double until = t_us + step_ms * 1000.0;
        while (t_us < until) {
            uint16_t v;
            if (queue_try_remove(&q, &v)) {
                last = v;
                emit(v);
            } else {
                emit(last);
                emit(last);
            }
        }
        mock_millis_value = (uint32_t)(t_us / 1000.0);
        player.process(&q);
        TEST_ASSERT_TRUE_MESSAGE(t_us < max_seconds * 1e6, "playback did not finish");
    }
    return merge_levels(wave);
}

// ----------------------------------------------------------------------------
// 参照デコーダ: 波形を FSK としてバイト列に戻す (無音で区切ったブロックごと)
// ----------------------------------------------------------------------------
struct FskDecodeResult {
    std::vector<std::vector<uint8_t>> blocks;
    int framing_errors = 0;
};

inline FskDecodeResult decode_fsk(const std::vector<Half>& wave, const FskFormat& f) {
    std::vector<Half> w = normalize_silence(wave);
    FskDecodeResult r;
    std::vector<int> bits;
    double s = f.short_half_us;
    double threshold = (s + f.long_us()) / 2;
    int short_bit = f.one_is_long ? 0 : 1;

    auto flush_bits = [&]() {
        std::vector<uint8_t> out;
        size_t k = 0;
        while (k + 9 < bits.size()) {
            if (bits[k] == 1) {
                k++;
                continue;
            }
            uint8_t v = 0;
            for (int b = 0; b < 8; b++) {
                v |= (uint8_t)(bits[k + 1 + b] << b);
            }
            if (bits[k + 9] == 1) {
                out.push_back(v);
            } else {
                r.framing_errors++;
            }
            k += 10;
        }
        if (!out.empty()) {
            r.blocks.push_back(out);
        }
        bits.clear();
    };

    size_t i = 0;
    while (i < w.size()) {
        if (w[i].us >= 4 * s) {
            flush_bits();  // 無音
            i++;
            continue;
        }
        bool is_short = w[i].us < threshold;
        size_t j = i;
        while (j < w.size() && w[j].us < 4 * s && ((w[j].us < threshold) == is_short)) {
            j++;
        }
        int n = (int)(j - i);
        int bit = is_short ? short_bit : 1 - short_bit;
        int per = f.halves_of(bit);
        int nb = (n + per / 2) / per;
        for (int b = 0; b < nb; b++) {
            bits.push_back(bit);
        }
        i = j;
    }
    flush_bits();
    return r;
}

// 短い/長い半周期の平均時間 (無音は除く)
struct HalfStats {
    double mean_short = 0;
    double mean_long = 0;
};

inline HalfStats measure_halves(const std::vector<Half>& wave, const FskFormat& f) {
    double s = f.short_half_us;
    double sum_s = 0, sum_l = 0;
    int n_s = 0, n_l = 0;
    for (const Half& h : normalize_silence(wave)) {
        if (h.us < 0.5 * s || h.us >= 4 * s) {
            continue;
        }
        if (h.us < 1.5 * s) {
            sum_s += h.us;
            n_s++;
        } else {
            sum_l += h.us;
            n_l++;
        }
    }
    HalfStats st;
    st.mean_short = n_s ? sum_s / n_s : 0;
    st.mean_long = n_l ? sum_l / n_l : 0;
    return st;
}

// 無音区間 (指定以上の長さ) の時間を先頭から順に返す
inline std::vector<double> silence_durations_ms(const std::vector<Half>& wave,
                                                double min_ms = 50) {
    std::vector<double> out;
    for (const Half& h : normalize_silence(wave)) {
        if (h.us >= min_ms * 1000.0) {
            out.push_back(h.us / 1000.0);
        }
    }
    return out;
}

inline void assert_bytes_equal(const std::vector<uint8_t>& expected,
                               const std::vector<uint8_t>& actual, const char* what) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: size", what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected.size(), actual.size(), msg);
    for (size_t i = 0; i < expected.size(); i++) {
        if (expected[i] != actual[i]) {
            snprintf(msg, sizeof(msg), "%s: mismatch at byte %u", what, (unsigned)i);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

// actual の末尾が expected と一致するか (先頭に余分なバイトがあってもよい)
inline void assert_bytes_end_with(const std::vector<uint8_t>& expected,
                                  const std::vector<uint8_t>& actual, const char* what) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: too short (%u < %u)", what, (unsigned)actual.size(),
             (unsigned)expected.size());
    TEST_ASSERT_TRUE_MESSAGE(actual.size() >= expected.size(), msg);
    std::vector<uint8_t> tail(actual.end() - expected.size(), actual.end());
    assert_bytes_equal(expected, tail, what);
}
