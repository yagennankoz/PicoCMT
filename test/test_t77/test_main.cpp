// ============================================================================
// T77 変換・再生のテスト (PC上で実行: pio test -e native)
//
//   FM-7形式の信号 → 録音PIOモデル → RAW → convert_raw_to_t77() → T77
//   → PlayerT77 → 再生PIOモデル → 波形
// の各段で、波形が元の信号と一致することを確認する。
// 後半は実物のデータ (XM7 で SAVE した T77、音声の WAV、それを公開ツール wav2t77 で
// 変換した T77) と突き合わせる。データはリポジトリに含めず、
// python tools/fetch_tools.py xm7 で tools/external/xm7/data/ に取得する (無ければスキップ)
// ============================================================================
#include <unity.h>

#include <stdio.h>
#include <stdlib.h>

#include <string>

#include "cmt_t77_converter.h"
#include "cmt_t77_format.h"
#include "player_t77.h"
#include "crosscheck_utils.h"
#include "tape_test_utils.h"

// テスト対象が参照するファームウェア側のグローバル
volatile uint32_t playback_base_time_ms = 0;
volatile uint32_t playback_tag_start_system_ms = 0;
volatile bool playback_time_valid = false;
void draw_conversion_progress(int) {}

static const char* RAW_PATH = "/RAW_TEMP.BIN";
static const char* T77_PATH = "/OUT.T77";
static const double TICK_US = (double)T77_TICK_US;

// ----------------------------------------------------------------------------
// FM-7 形式の信号 (1ビット = 1周期。'0' = 短い周期、'1' = 長い周期)
//   ブロック = リーダー(0xFF x16) + 01 3C [種別] [長さ] [データ] [チェックサム]
// ----------------------------------------------------------------------------
static void fm7_block(TapeSignal& s, uint8_t type, const std::vector<uint8_t>& data) {
    for (int i = 0; i < 16; i++) {
        s.byte(0xFF);
    }
    uint8_t len = (uint8_t)(data.size() & 0xFF);
    uint8_t sum = type + len;
    for (uint8_t d : data) {
        sum += d;
    }
    s.byte(0x01);
    s.byte(0x3C);
    s.byte(type);
    s.byte(len);
    s.bytes(data);
    s.byte(sum);
}

static void fm7_header(TapeSignal& s, const char* name) {
    std::vector<uint8_t> d(20, 0);
    for (int i = 0; i < T77_NAME_LEN; i++) {
        d[i] = (i < (int)strlen(name)) ? (uint8_t)name[i] : ' ';
    }
    fm7_block(s, 0x00, d);
}

// 極性合わせのダミー (10us未満) を前後のパルスに含めてまとめる
static std::vector<Half> fold_dummies(const std::vector<Half>& in) {
    std::vector<Half> out;
    for (const Half& h : in) {
        if (out.size() >= 2 && out.back().us < 10 && out[out.size() - 2].high == h.high) {
            double dummy = out.back().us;
            out.pop_back();
            out.back().us += dummy + h.us;
        } else {
            out.push_back(h);
        }
    }
    // 先頭のダミーは比較対象外
    if (!out.empty() && out.front().us < 10) {
        out.erase(out.begin());
    }
    return out;
}

static std::vector<uint16_t> read_t77_words(const char* path) {
    std::vector<uint8_t> d = SD.get(path);
    std::vector<uint16_t> w;
    for (size_t i = T77_HEADER_LEN; i + 1 < d.size(); i += 2) {
        w.push_back((uint16_t)((d[i] << 8) | d[i + 1]));
    }
    return w;
}

static std::vector<Half> t77_to_halves(const std::vector<uint16_t>& words) {
    std::vector<Half> out;
    for (uint16_t w : words) {
        uint32_t c = w & T77_COUNT_MASK;
        if (c != 0) {
            out.push_back({(w & T77_LEVEL_BIT) != 0, c * TICK_US});
        }
    }
    return merge_levels(out);
}

static void write_t77(const std::vector<uint16_t>& words) {
    std::vector<uint8_t> d(T77_HEADER_STR, T77_HEADER_STR + T77_HEADER_LEN);
    for (uint16_t w : words) {
        d.push_back((uint8_t)(w >> 8));
        d.push_back((uint8_t)(w & 0xFF));
    }
    SD.put(T77_PATH, d);
}

struct CompareResult {
    double max_pulse_err = 0;   // 2ms未満のパルスの最大誤差
    double max_drift = 0;       // 無音で区切った区間内の累積ずれの最大
    double min_gap_err = 0;     // 無音の誤差 (最小)
    double max_gap_err = 0;     // 無音の誤差 (最大)
};

static CompareResult compare_waves(const std::vector<Half>& actual,
                                   const std::vector<Half>& expected) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected.size(), actual.size(), "pulse count");
    CompareResult r;
    double drift = 0;
    // 最後のパルスは打ち切りの差が出るので除く
    for (size_t i = 0; i + 1 < expected.size(); i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "level mismatch at pulse %u", (unsigned)i);
        TEST_ASSERT_EQUAL_MESSAGE(expected[i].high, actual[i].high, msg);

        double err = actual[i].us - expected[i].us;
        if (expected[i].us >= 2000) {
            r.min_gap_err = fmin(r.min_gap_err, err);
            r.max_gap_err = fmax(r.max_gap_err, err);
            drift = 0;
            continue;
        }
        r.max_pulse_err = fmax(r.max_pulse_err, fabs(err));
        drift += err;
        r.max_drift = fmax(r.max_drift, fabs(drift));
    }
    return r;
}

// ----------------------------------------------------------------------------
// PlayerT77 を動かし、再生PIO (cmt_play.pio) の出力を再現する
//   最初の値はHigh、値ごとにH/L反転、出力時間は y+7us(High) / y+6us(Low)
// ----------------------------------------------------------------------------
static std::vector<Half> play_t77(uint32_t max_consume_per_call, uint32_t seed) {
    File f = SD.open(T77_PATH, FILE_READ);
    PlayerT77 player;
    TEST_ASSERT_TRUE(player.init(&f, T77_PATH));

    queue_t q;
    queue_init(&q, sizeof(uint16_t), PLAY_QUEUE_LENGTH);
    std::mt19937 rng(seed);
    std::vector<uint16_t> values;

    int guard = 0;
    while (!(player.is_exhausted() && queue_is_empty(&q))) {
        player.process(&q);
        uint32_t n = rng() % (max_consume_per_call + 1);
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v;
            if (!queue_try_remove(&q, &v)) {
                break;
            }
            values.push_back(v);
        }
        TEST_ASSERT_TRUE_MESSAGE(++guard < 10000000, "player did not finish");
    }

    std::vector<Half> wave;
    bool level = true;
    for (uint16_t v : values) {
        wave.push_back({level, v + (level ? 7.0 : 6.0)});
        level = !level;
    }
    return merge_levels(wave);
}

// FM-7形式のテスト信号 (ヘッダ2つ、データ、無音を含む)
static TapeSignal make_test_signal(double jitter, const FskFormat& f = FSK_FM7) {
    TapeSignal s(f, jitter, 7);
    s.ones(60);  // 録音のトリガ用
    fm7_header(s, "GAME");
    s.silence(400);
    fm7_block(s, 0x01, random_bytes(255, 1));
    fm7_block(s, 0x01, random_bytes(100, 2));
    s.silence(150);
    fm7_header(s, "SCORE01");
    s.silence(300);
    fm7_block(s, 0x01, random_bytes(200, 3));
    fm7_block(s, 0xFF, {});
    s.silence(500);
    s.ones(4);
    return s;
}

// ============================================================================
// 変換 (convert_raw_to_t77)
// ============================================================================
void test_convert_writes_valid_t77() {
    write_raw(RAW_PATH, make_test_signal(0.0).halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));

    std::vector<uint8_t> d = SD.get(T77_PATH);
    TEST_ASSERT_TRUE(d.size() > T77_HEADER_LEN);
    TEST_ASSERT_EQUAL_MEMORY(T77_HEADER_STR, d.data(), T77_HEADER_LEN);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, (d.size() - T77_HEADER_LEN) % 2, "odd data length");

    // XM7 と同じく、ヘッダの直後に 0x0000 が1つだけある
    std::vector<uint16_t> words = read_t77_words(T77_PATH);
    TEST_ASSERT_EQUAL_HEX16(0x0000, words[0]);
    for (size_t i = 1; i < words.size(); i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, words[i] & T77_COUNT_MASK, "zero-count word written");
    }
}

void test_convert_timing_matches_input() {
    TapeSignal s = make_test_signal(0.0);
    size_t first = write_raw(RAW_PATH, s.halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));

    std::vector<Half> expected(s.halves.begin() + first, s.halves.end());
    CompareResult r = compare_waves(t77_to_halves(read_t77_words(T77_PATH)),
                                    merge_levels(expected));

    // パルスごとの誤差は T77 の1カウント程度、区間内で誤差が積み上がらないこと
    TEST_ASSERT_TRUE_MESSAGE(r.max_pulse_err <= TICK_US + 1.0, "pulse error too large");
    TEST_ASSERT_TRUE_MESSAGE(r.max_drift <= TICK_US + 1.0, "timing drifts within a block");
    // 無音はRAWにms単位(切り捨て)で記録されるので、最大1ms短くなる
    TEST_ASSERT_TRUE(r.min_gap_err >= -1000.0 - TICK_US);
    TEST_ASSERT_TRUE(r.max_gap_err <= TICK_US + 5.0);
}

static void assert_header_names(const T77HeaderNames& names) {
    TEST_ASSERT_EQUAL_INT(2, names.count);
    TEST_ASSERT_EQUAL_MEMORY("GAME    ", names.name[0], T77_NAME_LEN);
    TEST_ASSERT_EQUAL_MEMORY("SCORE01 ", names.name[1], T77_NAME_LEN);
}

void test_convert_extracts_header_names() {
    write_raw(RAW_PATH, make_test_signal(0.0).halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    assert_header_names(t77_last_header_names());
}

void test_convert_extracts_header_names_with_jitter() {
    write_raw(RAW_PATH, make_test_signal(0.1).halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    assert_header_names(t77_last_header_names());
}

void test_convert_extracts_header_names_xm7_timing() {
    // XM7 の出力と同じ半周期の長さ (短い 約230us / 長い 約440us)
    write_raw(RAW_PATH, make_test_signal(0.05, FSK_FM7_XM7).halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    assert_header_names(t77_last_header_names());
}

void test_convert_extracts_header_names_inverted() {
    // 極性が逆 (録音回路で反転) でも読めること
    std::vector<Half> sig = make_test_signal(0.05).halves;
    for (Half& h : sig) {
        h.high = !h.high;
    }
    write_raw(RAW_PATH, sig);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    assert_header_names(t77_last_header_names());
}

void test_convert_extracts_savem_header_name() {
    // マシン語 (SAVEM、ファイルの種類 02) とデータファイル (種類 01、アスキー FF) のヘッダからも
    // 名前を拾う。名前を拾う処理はファイルの種類を見ない
    TapeSignal s(FSK_FM7, 0.05, 11);
    s.ones(60);
    std::vector<uint8_t> h(20, 0);
    memcpy(h.data(), "MCODE   ", T77_NAME_LEN);
    h[8] = 0x02;  // マシン語
    fm7_block(s, 0x00, h);
    s.silence(300);
    // SAVEM のデータ: 00 [長さ2] [アドレス2] [中身] ... FF 00 00 [実行アドレス2]
    std::vector<uint8_t> d = {0x00, 0x00, 0x40, 0x60, 0x00};
    std::vector<uint8_t> body = random_bytes(64, 5);
    d.insert(d.end(), body.begin(), body.end());
    d.insert(d.end(), {0xFF, 0x00, 0x00, 0x60, 0x00});
    fm7_block(s, 0x01, d);
    fm7_block(s, 0xFF, {});
    s.silence(300);
    memcpy(h.data(), "DATA1   ", T77_NAME_LEN);
    h[8] = 0x01;  // データファイル
    h[9] = 0xFF;  // アスキー
    fm7_block(s, 0x00, h);
    s.silence(300);
    fm7_block(s, 0x01, random_bytes(30, 6));
    fm7_block(s, 0xFF, {});
    s.silence(300);
    write_raw(RAW_PATH, s.halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    TEST_ASSERT_EQUAL_INT(2, t77_last_header_names().count);
    TEST_ASSERT_EQUAL_MEMORY("MCODE   ", t77_last_header_names().name[0], T77_NAME_LEN);
    TEST_ASSERT_EQUAL_MEMORY("DATA1   ", t77_last_header_names().name[1], T77_NAME_LEN);
}

void test_convert_ignores_header_with_bad_checksum() {
    TapeSignal s(FSK_FM7);
    s.ones(60);
    std::vector<uint8_t> d(20, 0);
    memcpy(d.data(), "BADSUM  ", T77_NAME_LEN);
    for (int i = 0; i < 16; i++) {
        s.byte(0xFF);
    }
    s.bytes({0x01, 0x3C, 0x00, 20});
    s.bytes(d);
    s.byte(0x55);  // 正しいチェックサムではない
    s.silence(300);
    fm7_header(s, "GOOD");
    s.silence(300);
    write_raw(RAW_PATH, s.halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    TEST_ASSERT_EQUAL_INT(1, t77_last_header_names().count);
    TEST_ASSERT_EQUAL_MEMORY("GOOD    ", t77_last_header_names().name[0], T77_NAME_LEN);
}

void test_convert_without_header_finds_no_names() {
    TapeSignal s(FSK_FM7);
    s.ones(60);
    fm7_block(s, 0x01, random_bytes(50, 4));
    s.silence(300);
    write_raw(RAW_PATH, s.halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    TEST_ASSERT_EQUAL_INT(0, t77_last_header_names().count);
}

void test_convert_reports_write_error() {
    write_raw(RAW_PATH, make_test_signal(0.0).halves);
    mock_sd_write_budget = 100;  // 途中でSDへの書き込みを失敗させる
    TEST_ASSERT_FALSE(convert_raw_to_t77(RAW_PATH, T77_PATH));
}

// ============================================================================
// 再生 (PlayerT77)
// ============================================================================
void test_player_roundtrip_matches_t77() {
    write_raw(RAW_PATH, make_test_signal(0.0).halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    std::vector<Half> expected = t77_to_halves(read_t77_words(T77_PATH));

    // Core1の消費量を変えて、キューが一杯になる状況も含めて確認する
    const uint32_t consume[] = {1, 40, 600, 5000};
    for (uint32_t c : consume) {
        CompareResult r = compare_waves(fold_dummies(play_t77(c, c)), expected);
        TEST_ASSERT_TRUE_MESSAGE(r.max_pulse_err <= 1.5, "pulse error too large");
        TEST_ASSERT_TRUE_MESSAGE(r.max_drift <= 1.5, "timing drifts within a block");
        // 長い無音は60msごとに分割され、間に約7usのダミーが入る
        TEST_ASSERT_TRUE(r.min_gap_err >= -1.5);
        TEST_ASSERT_TRUE(r.max_gap_err <= 7.0 * (500 / 60 + 1) + 1.5);
    }
}

void test_player_skips_zero_count_words() {
    // 0x0000 / 0x8000 は長さを持たないので読み飛ばし、再生を続ける
    write_t77({0x0000, 0x8000 | 23, 0x0000, 23, 0x8000, 0x8000 | 46, 46, 0x8000 | 23});
    std::vector<Half> wave = fold_dummies(play_t77(5000, 1));

    TEST_ASSERT_EQUAL_UINT32(5, wave.size());
    const bool levels[] = {true, false, true, false, true};
    const double counts[] = {23, 23, 46, 46, 23};
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(levels[i], wave[i].high);
        TEST_ASSERT_DOUBLE_WITHIN(1.0, counts[i] * TICK_US, wave[i].us);
    }
}

void test_player_keeps_polarity_when_first_level_is_low() {
    write_t77({46, 0x8000 | 23, 23, 0x8000 | 23});
    std::vector<Half> wave = fold_dummies(play_t77(5000, 1));

    TEST_ASSERT_EQUAL_UINT32(4, wave.size());
    TEST_ASSERT_FALSE(wave[0].high);
    TEST_ASSERT_TRUE(wave[1].high);
    TEST_ASSERT_FALSE(wave[2].high);
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 46 * TICK_US, wave[0].us);
}

void test_player_splits_long_silence_without_flipping() {
    // 約3秒の無音 (0x7FFF カウント x 10)
    std::vector<uint16_t> words = {0x8000 | 23};
    for (int i = 0; i < 10; i++) {
        words.push_back(0x7FFF);
    }
    words.push_back(0x8000 | 23);
    words.push_back(23);
    write_t77(words);

    std::vector<Half> wave = fold_dummies(play_t77(3, 1));
    TEST_ASSERT_EQUAL_UINT32(4, wave.size());
    TEST_ASSERT_TRUE(wave[0].high);
    TEST_ASSERT_FALSE(wave[1].high);
    TEST_ASSERT_TRUE(wave[2].high);

    double silence_us = 0x7FFF * 10 * TICK_US;
    int splits = (int)(silence_us / 60000);
    TEST_ASSERT_DOUBLE_WITHIN(7.0 * splits + 1.5, silence_us, wave[1].us);
}

// ============================================================================
// 実物のデータとの突き合わせ (tools/external/xm7/data/)
// ============================================================================
static std::string xm7_data_path(const char* name) { return external_path("xm7", "data", name); }

static bool has_xm7_data() {
    std::vector<uint8_t> d;
    const char* names[] = {"xm7_save.t77", "basic_i.wav", "basic_i_wav2t77.t77"};
    for (const char* n : names) {
        if (!read_host_file(xm7_data_path(n), d)) {
            return false;
        }
    }
    return true;
}

static std::vector<uint8_t> read_xm7_file(const char* name) {
    std::vector<uint8_t> d;
    TEST_ASSERT_TRUE_MESSAGE(read_host_file(xm7_data_path(name), d), name);
    return d;
}

static std::vector<uint16_t> t77_words_of(const std::vector<uint8_t>& d) {
    TEST_ASSERT_TRUE(d.size() > T77_HEADER_LEN);
    TEST_ASSERT_EQUAL_MEMORY(T77_HEADER_STR, d.data(), T77_HEADER_LEN);
    std::vector<uint16_t> w;
    for (size_t i = T77_HEADER_LEN; i + 1 < d.size(); i += 2) {
        w.push_back((uint16_t)((d[i] << 8) | d[i + 1]));
    }
    return w;
}

// FM-7 のブロック (01 3C [種別] [長さ] [データ] [チェックサム])
struct Fm7Block {
    uint8_t type;
    std::vector<uint8_t> data;
    bool sum_ok;
};

// 波形を参照デコーダで読み、ブロックを取り出す
static std::vector<Fm7Block> fm7_blocks_of(const std::vector<Half>& wave) {
    std::vector<uint8_t> bytes;
    for (const auto& b : decode_fsk(wave, FSK_FM7).blocks) {
        bytes.insert(bytes.end(), b.begin(), b.end());
    }
    std::vector<Fm7Block> blocks;
    for (size_t i = 0; i + 4 < bytes.size();) {
        if (bytes[i] != 0x01 || bytes[i + 1] != 0x3C) {
            i++;
            continue;
        }
        uint8_t type = bytes[i + 2];
        uint8_t len = bytes[i + 3];
        if (i + 4 + len >= bytes.size()) {
            break;
        }
        Fm7Block b;
        b.type = type;
        b.data.assign(bytes.begin() + i + 4, bytes.begin() + i + 4 + len);
        uint8_t sum = type + len;
        for (uint8_t v : b.data) {
            sum += v;
        }
        b.sum_ok = (sum == bytes[i + 4 + len]);
        blocks.push_back(b);
        i += 5 + len;
    }
    return blocks;
}

static void assert_blocks_equal(const std::vector<Fm7Block>& expected,
                                const std::vector<Fm7Block>& actual) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected.size(), actual.size(), "block count");
    for (size_t i = 0; i < expected.size(); i++) {
        TEST_ASSERT_TRUE_MESSAGE(actual[i].sum_ok, "checksum");
        TEST_ASSERT_EQUAL_UINT8(expected[i].type, actual[i].type);
        TEST_ASSERT_EQUAL_UINT32(expected[i].data.size(), actual[i].data.size());
        if (!expected[i].data.empty()) {
            TEST_ASSERT_EQUAL_MEMORY(expected[i].data.data(), actual[i].data.data(),
                                     expected[i].data.size());
        }
    }
}

// XM7 で SAVE した T77 の中身 (ヘッダ "LOOP7"、データ、終端の3ブロック)
static std::vector<Fm7Block> xm7_save_blocks() {
    std::vector<Fm7Block> b =
        fm7_blocks_of(t77_to_halves(t77_words_of(read_xm7_file("xm7_save.t77"))));
    TEST_ASSERT_EQUAL_UINT32(3, b.size());
    return b;
}

static void assert_loop7_name() {
    const T77HeaderNames& names = t77_last_header_names();
    TEST_ASSERT_EQUAL_INT(1, names.count);
    TEST_ASSERT_EQUAL_MEMORY("LOOP7   ", names.name[0], T77_NAME_LEN);
}

void test_xm7_save_reference_decode() {
    if (!has_xm7_data()) {
        TEST_IGNORE_MESSAGE("no data: run python tools/fetch_tools.py xm7");
    }
    // テスト側の参照デコーダが、XM7 の T77 を正しく読めることの確認
    std::vector<Fm7Block> b = xm7_save_blocks();
    for (const Fm7Block& blk : b) {
        TEST_ASSERT_TRUE(blk.sum_ok);
    }
    TEST_ASSERT_EQUAL_UINT8(0x00, b[0].type);
    TEST_ASSERT_EQUAL_MEMORY("LOOP7   ", b[0].data.data(), T77_NAME_LEN);
    TEST_ASSERT_EQUAL_UINT8(0x01, b[1].type);
    TEST_ASSERT_EQUAL_UINT8(0xFF, b[2].type);
}

void test_xm7_save_record_and_convert() {
    if (!has_xm7_data()) {
        TEST_IGNORE_MESSAGE("no data: run python tools/fetch_tools.py xm7");
    }
    // XM7 の T77 の信号を録音して変換すると、同じ波形・同じ中身の T77 になる
    std::vector<Half> sig = t77_to_halves(t77_words_of(read_xm7_file("xm7_save.t77")));
    size_t first = write_raw(RAW_PATH, sig);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    assert_loop7_name();

    xcheck_dump("pico_rec_xm7.t77", SD.get(T77_PATH));
    std::vector<Half> ours = t77_to_halves(read_t77_words(T77_PATH));
    assert_blocks_equal(xm7_save_blocks(), fm7_blocks_of(ours));

    std::vector<Half> expected(sig.begin() + first, sig.end());
    CompareResult r = compare_waves(ours, merge_levels(expected));
    TEST_ASSERT_TRUE_MESSAGE(r.max_pulse_err <= TICK_US + 1.0, "pulse error too large");
    TEST_ASSERT_TRUE_MESSAGE(r.max_drift <= TICK_US + 1.0, "timing drifts within a block");
}

void test_xm7_save_playback() {
    if (!has_xm7_data()) {
        TEST_IGNORE_MESSAGE("no data: run python tools/fetch_tools.py xm7");
    }
    // XM7 の T77 を再生した波形が、T77 の時間どおりで、同じ中身として読める
    std::vector<uint8_t> d = read_xm7_file("xm7_save.t77");
    SD.put(T77_PATH, d);
    std::vector<Half> expected = t77_to_halves(t77_words_of(d));
    std::vector<Half> wave = fold_dummies(play_t77(600, 3));
    xcheck_dump("pico_play_xm7.wav", wave_to_wav(wave));

    CompareResult r = compare_waves(wave, expected);
    TEST_ASSERT_TRUE_MESSAGE(r.max_pulse_err <= 1.5, "pulse error too large");
    TEST_ASSERT_TRUE_MESSAGE(r.max_drift <= 1.5, "timing drifts within a block");
    assert_blocks_equal(xm7_save_blocks(), fm7_blocks_of(wave));
}

void test_wav_record_matches_wav2t77() {
    if (!has_xm7_data()) {
        TEST_IGNORE_MESSAGE("no data: run python tools/fetch_tools.py xm7");
    }
    // 同じ WAV を、PicoCmt の録音・変換と、公開ツール wav2t77 で T77 にして比べる
    std::vector<Half> sig = wav_to_halves(read_xm7_file("basic_i.wav"));
    write_raw(RAW_PATH, sig);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    assert_loop7_name();
    xcheck_dump("pico_rec_wav.t77", SD.get(T77_PATH));

    std::vector<Half> ours = t77_to_halves(read_t77_words(T77_PATH));
    std::vector<Half> theirs =
        t77_to_halves(t77_words_of(read_xm7_file("basic_i_wav2t77.t77")));

    // 中身: wav2t77 の結果とも、XM7 で SAVE した元の T77 とも一致する
    std::vector<Fm7Block> ref = fm7_blocks_of(theirs);
    assert_blocks_equal(ref, fm7_blocks_of(ours));
    assert_blocks_equal(xm7_save_blocks(), ref);

    // 時間: 短い/長い半周期の平均が、wav2t77 の結果と 2% 以内
    HalfStats a = measure_halves(ours, FSK_FM7);
    HalfStats b = measure_halves(theirs, FSK_FM7);
    char msg[160];
    snprintf(msg, sizeof(msg), "PicoCmt short %.1f long %.1f us / wav2t77 short %.1f long %.1f us",
             a.mean_short, a.mean_long, b.mean_short, b.mean_long);
    TEST_MESSAGE(msg);
    TEST_ASSERT_DOUBLE_WITHIN(b.mean_short * 0.02, b.mean_short, a.mean_short);
    TEST_ASSERT_DOUBLE_WITHIN(b.mean_long * 0.02, b.mean_long, a.mean_long);
}

void test_synthetic_for_crosscheck() {
    // 合成した FM-7 の信号 (ファイル名 GAME / SCORE01) を録音・変換し、再生する
    write_raw(RAW_PATH, make_test_signal(0.05, FSK_FM7_XM7).halves);
    TEST_ASSERT_TRUE(convert_raw_to_t77(RAW_PATH, T77_PATH));
    assert_header_names(t77_last_header_names());
    xcheck_dump("pico_rec_synth.t77", SD.get(T77_PATH));
    xcheck_dump("pico_play_synth.wav", wave_to_wav(fold_dummies(play_t77(600, 5))));
}

void setUp() {
    SD.clear();
    mock_millis_value = 0;
    mock_sd_write_budget = SIZE_MAX;
}

void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_convert_writes_valid_t77);
    RUN_TEST(test_convert_timing_matches_input);
    RUN_TEST(test_convert_extracts_header_names);
    RUN_TEST(test_convert_extracts_header_names_with_jitter);
    RUN_TEST(test_convert_extracts_header_names_xm7_timing);
    RUN_TEST(test_convert_extracts_header_names_inverted);
    RUN_TEST(test_convert_extracts_savem_header_name);
    RUN_TEST(test_convert_ignores_header_with_bad_checksum);
    RUN_TEST(test_convert_without_header_finds_no_names);
    RUN_TEST(test_convert_reports_write_error);
    RUN_TEST(test_player_roundtrip_matches_t77);
    RUN_TEST(test_player_skips_zero_count_words);
    RUN_TEST(test_player_keeps_polarity_when_first_level_is_low);
    RUN_TEST(test_player_splits_long_silence_without_flipping);
    RUN_TEST(test_xm7_save_reference_decode);
    RUN_TEST(test_xm7_save_record_and_convert);
    RUN_TEST(test_xm7_save_playback);
    RUN_TEST(test_wav_record_matches_wav2t77);
    RUN_TEST(test_synthetic_for_crosscheck);
    return UNITY_END();
}
