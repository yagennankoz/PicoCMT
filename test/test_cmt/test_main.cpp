// ============================================================================
// CMT (PC-8001/8801 ベタ形式) 変換・再生のテスト (pio test -e native)
// ============================================================================
#include <unity.h>

#include "cmt_cmt_converter.h"
#include "crosscheck_utils.h"
#include "player_cmt.h"
#include "tape_test_utils.h"

volatile uint32_t playback_base_time_ms = 0;
volatile uint32_t playback_tag_start_system_ms = 0;
volatile bool playback_time_valid = false;
void draw_conversion_progress(int) {}

static const char* RAW_PATH = "/RAW_TEMP.BIN";
static const char* CMT_PATH = "/OUT.CMT";

static std::vector<uint8_t> concat(const std::vector<std::vector<uint8_t>>& blocks) {
    std::vector<uint8_t> out;
    for (const auto& b : blocks) {
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

// リーダー + データ + 短いマーク + 無音 を繰り返す
static TapeSignal make_signal(const FskFormat& f,
                              const std::vector<std::vector<uint8_t>>& blocks,
                              double jitter = 0.0) {
    TapeSignal s(f, jitter, 3);
    for (const auto& b : blocks) {
        s.leader_ms(1000);
        s.bytes(b);
        s.leader_ms(200);
        s.silence(500);
    }
    return s;
}

static void check_convert(const FskFormat& f, double jitter) {
    std::vector<std::vector<uint8_t>> blocks = {random_bytes(100, 1), random_bytes(300, 2)};
    write_raw(RAW_PATH, make_signal(f, blocks, jitter).halves);
    TEST_ASSERT_TRUE(convert_raw_to_cmt(RAW_PATH, CMT_PATH));
    assert_bytes_equal(concat(blocks), SD.get(CMT_PATH), "CMT");
}

// ============================================================================
// 変換 (convert_raw_to_cmt)
// ============================================================================
void test_convert_1200() { check_convert(FSK_1200, 0.0); }
void test_convert_600() { check_convert(FSK_600, 0.0); }
void test_convert_1200_with_jitter() { check_convert(FSK_1200, 0.05); }

void test_convert_tape_speed_and_jitter() {
    // テープ速度 ±5% と揺らぎ ±10% (Mark と Space が混ざった半周期の組で
    // ビットの長さを測ってしまい、テープが遅いと崩れていた)
    for (const FskFormat* f : {&FSK_1200, &FSK_600}) {
        for (double speed : {0.95, 1.05}) {
            check_convert(at_speed(*f, speed), 0.1);
        }
    }
}

void test_convert_long_block_without_gap() {
    // 無音を挟まない 40000 バイトのブロック (変換バッファ 32768 バイトを超える)
    std::vector<std::vector<uint8_t>> blocks = {random_bytes(40000, 5)};
    write_raw(RAW_PATH, make_signal(FSK_1200, blocks).halves);
    TEST_ASSERT_TRUE(convert_raw_to_cmt(RAW_PATH, CMT_PATH));
    assert_bytes_equal(blocks[0], SD.get(CMT_PATH), "CMT");
}

void test_convert_machine_code() {
    // モニタの W で保存したマシン語 (先頭が 0x3A、名前もピー音の区切りも無い)
    std::vector<uint8_t> data = pc8001_machine_code(0xC000, 600, 31);
    for (const FskFormat* f : {&FSK_1200, &FSK_600}) {
        write_raw(RAW_PATH, make_signal(*f, {data}, 0.05).halves);
        TEST_ASSERT_TRUE(convert_raw_to_cmt(RAW_PATH, CMT_PATH));
        assert_bytes_equal(data, SD.get(CMT_PATH), "CMT machine code");
    }
}

void test_convert_reports_write_error() {
    write_raw(RAW_PATH, make_signal(FSK_1200, {random_bytes(40000, 5)}).halves);
    mock_sd_write_budget = 100;  // 途中でSDへの書き込みを失敗させる
    TEST_ASSERT_FALSE(convert_raw_to_cmt(RAW_PATH, CMT_PATH));
}

// ============================================================================
// 再生 (PlayerCMT)
// ============================================================================
static std::vector<Half> play_file(const char* path, const std::vector<uint8_t>& data, int baud) {
    SD.put(path, data);
    File f = SD.open(path, FILE_READ);
    PlayerCMT player;
    player.set_baud_rate(baud);
    TEST_ASSERT_TRUE(player.init(&f, path));
    return run_playback(player);
}

static std::vector<Half> play_cmt(const std::vector<uint8_t>& data, int baud) {
    return play_file(CMT_PATH, data, baud);
}

// ピー音 (短い半周期の連続) のうち、min_ms 以上のものの長さ (ms)
static std::vector<double> leader_durations_ms(const std::vector<Half>& wave,
                                               double min_ms = 500) {
    std::vector<double> out;
    double run = 0;
    for (const Half& h : wave) {
        if (h.us < 312) {
            run += h.us;
            continue;
        }
        if (run >= min_ms * 1000) {
            out.push_back(run / 1000);
        }
        run = 0;
    }
    if (run >= min_ms * 1000) {
        out.push_back(run / 1000);
    }
    return out;
}

static void check_player_output(int baud, const FskFormat& f) {
    std::vector<uint8_t> data = random_bytes(200, 7);
    FskDecodeResult r = decode_fsk(play_cmt(data, baud), f);
    TEST_ASSERT_EQUAL_UINT32(1, r.blocks.size());

    // 先頭にダミーの 0x00 x8 が付く
    std::vector<uint8_t> expected(8, 0x00);
    expected.insert(expected.end(), data.begin(), data.end());
    assert_bytes_end_with(expected, r.blocks[0], "decoded");
}

void test_player_1200_outputs_file_bytes() { check_player_output(1200, FSK_1200); }
void test_player_600_outputs_file_bytes() { check_player_output(600, FSK_600); }

void test_player_pulse_timing() {
    HalfStats st = measure_halves(play_cmt(random_bytes(200, 7), 1200), FSK_1200);
    // 2400Hz / 1200Hz の半周期が公称値から 1.5% 以内
    TEST_ASSERT_DOUBLE_WITHIN(208.33 * 0.015, 208.33, st.mean_short);
    TEST_ASSERT_DOUBLE_WITHIN(416.67 * 0.015, 416.67, st.mean_long);
}

// ============================================================================
// 公開ツール PapicomTapeConvertor (WAV → P6) とのクロスチェック (python tools/crosscheck.py p6)
//   P6 は CMT と同じ処理で再生する。環境変数 XCHECK_OUT_DIR が無ければスキップ
// ============================================================================
// PC-6001 の BASIC のファイル: ヘッダ (D3 x10 + ファイル名6文字)、プログラム、終端の 00
static std::vector<uint8_t> p6_file() {
    std::vector<uint8_t> d(10, 0xD3);
    const char* name = "PICO  ";
    d.insert(d.end(), name, name + 6);
    std::vector<uint8_t> prog = random_bytes(300, 11);
    d.insert(d.end(), prog.begin(), prog.end());
    d.insert(d.end(), 12, 0x00);
    return d;
}

void test_player_p6_leader_after_header() {
    // PC-6001 の CSAVE と同じく、ヘッダ (D3 x10 + ファイル名) の後にピー音を入れる
    const FskFormat* formats[] = {&FSK_1200, &FSK_600};
    const int bauds[] = {1200, 600};
    for (int i = 0; i < 2; i++) {
        std::vector<uint8_t> data = p6_file();
        std::vector<Half> wave = play_file("/OUT.P6", data, bauds[i]);

        // ピー音は 先頭 (3秒)、ヘッダの後 (1秒)、最後 (2秒)
        std::vector<double> leaders = leader_durations_ms(wave);
        TEST_ASSERT_EQUAL_UINT32(3, leaders.size());
        TEST_ASSERT_DOUBLE_WITHIN(50, 3000, leaders[0]);
        TEST_ASSERT_DOUBLE_WITHIN(50, 1000, leaders[1]);
        TEST_ASSERT_DOUBLE_WITHIN(50, 2000, leaders[2]);

        // PC-6001 の CSAVE と同じくピー音から始まり (1200Hz とダミーの 0x00 は無し)、
        // 中身はファイルのとおり
        TEST_ASSERT_TRUE(wave[0].us < 312);
        FskDecodeResult r = decode_fsk(wave, *formats[i]);
        TEST_ASSERT_EQUAL_UINT32(1, r.blocks.size());
        assert_bytes_equal(data, r.blocks[0], "P6 decoded");
        TEST_ASSERT_EQUAL_INT(0, r.framing_errors);
    }

    // CMT (PC-8801) の長いピー音は 先頭と最後の2つ
    TEST_ASSERT_EQUAL_UINT32(2, leader_durations_ms(play_cmt(p6_file(), 1200)).size());
}

void test_player_cmt_leader_after_header() {
    // PC-8801 の BASIC と同じく、ヘッダ (D3 x10 + ファイル名) の後に約0.21秒のピー音を入れる
    // (X88000 が記録した実機の BASIC の T88 で 0.21秒)
    const FskFormat* formats[] = {&FSK_1200, &FSK_600};
    const int bauds[] = {1200, 600};
    for (int i = 0; i < 2; i++) {
        std::vector<uint8_t> data = p6_file();
        std::vector<Half> wave = play_cmt(data, bauds[i]);
        std::vector<double> leaders = leader_durations_ms(wave, 100);
        TEST_ASSERT_EQUAL_UINT32(3, leaders.size());
        TEST_ASSERT_DOUBLE_WITHIN(50, 3000, leaders[0]);
        TEST_ASSERT_DOUBLE_WITHIN(15, 208, leaders[1]);
        TEST_ASSERT_DOUBLE_WITHIN(50, 2000, leaders[2]);

        FskDecodeResult r = decode_fsk(wave, *formats[i]);
        TEST_ASSERT_EQUAL_UINT32(1, r.blocks.size());
        std::vector<uint8_t> expected(8, 0x00);
        expected.insert(expected.end(), data.begin(), data.end());
        assert_bytes_end_with(expected, r.blocks[0], "CMT decoded");
    }
    // ヘッダの無いデータ (マシン語など) では入れない
    TEST_ASSERT_EQUAL_UINT32(
        2, leader_durations_ms(play_cmt(random_bytes(200, 7), 1200), 100).size());
}

void test_player_cmt_leader_after_monitor_header() {
    // N88-BASIC のモニタの W名前 のヘッダ ("$$$" + 名前) の後にもピー音を入れる
    std::vector<uint8_t> data = n88_monitor_header("MTEST ");
    std::vector<uint8_t> code = pc8001_machine_code(0xC000, 64, 9);
    data.insert(data.end(), code.begin(), code.end());
    for (int baud : {1200, 600}) {
        std::vector<Half> wave = play_cmt(data, baud);
        std::vector<double> leaders = leader_durations_ms(wave, 100);
        TEST_ASSERT_EQUAL_UINT32(3, leaders.size());
        TEST_ASSERT_DOUBLE_WITHIN(15, 208, leaders[1]);

        // 再生 → 録音 → 変換 で戻る (先頭にダミーの 0x00 x8)
        write_raw(RAW_PATH, wave);
        TEST_ASSERT_TRUE(convert_raw_to_cmt(RAW_PATH, "/RT.CMT"));
        std::vector<uint8_t> expected(8, 0x00);
        expected.insert(expected.end(), data.begin(), data.end());
        assert_bytes_equal(expected, SD.get("/RT.CMT"), "CMT monitor roundtrip");
    }

    // データの途中の "$$$" (前が 00 でない) では入れない
    std::vector<uint8_t> plain = random_bytes(100, 3);
    plain[0] = 0x41;
    std::vector<uint8_t> dollars = {0x41, 0x24, 0x24, 0x24, 'A', 'B', 'C', 'D', 'E', 'F'};
    plain.insert(plain.begin() + 50, dollars.begin(), dollars.end());
    TEST_ASSERT_EQUAL_UINT32(2, leader_durations_ms(play_cmt(plain, 1200), 100).size());

    // P6 では "$$$" をヘッダとして扱わない
    TEST_ASSERT_EQUAL_UINT32(2, leader_durations_ms(play_file("/OUT.P6", n88_monitor_header("MTEST "), 1200), 100).size());
}

// 1バイトあたりのビット数 (スタート1 + データ8 + ストップ)。
// 最初のピー音の終わりから最後のピー音の始まりまでの時間 (途中の0.1秒以上のピー音を除く) から求める
static double bits_per_byte(const std::vector<Half>& wave, size_t bytes, int baud) {
    std::vector<std::pair<double, double>> leaders;  // (始まり, 長さ) us
    double t = 0, run = 0;
    for (const Half& h : wave) {
        if (h.us < 312) {
            run += h.us;
        } else {
            if (run >= 100000) {
                leaders.push_back({t - run, run});
            }
            run = 0;
        }
        t += h.us;
    }
    if (run >= 100000) {
        leaders.push_back({t - run, run});
    }
    TEST_ASSERT_TRUE(leaders.size() >= 2);
    double start = leaders.front().first + leaders.front().second;
    double end = leaders.back().first;
    double middle = 0;
    for (size_t k = 1; k + 1 < leaders.size(); k++) {
        middle += leaders[k].second;
    }
    return (end - start - middle) / 1e6 * baud / bytes;
}

void test_player_p6_uses_3_stop_bits() {
    // PC-6001 はストップビット3 (12ビット/バイト)、PC-8801 は2 (11ビット/バイト)
    size_t bytes = p6_file().size();
    for (int baud : {1200, 600}) {
        TEST_ASSERT_DOUBLE_WITHIN(0.05, 12.0,
                                  bits_per_byte(play_file("/OUT.P6", p6_file(), baud), bytes, baud));
        // CMT はダミーの 0x00 x8 が付く
        TEST_ASSERT_DOUBLE_WITHIN(
            0.05, 11.0, bits_per_byte(play_file("/OUT.CMT", p6_file(), baud), 8 + bytes, baud));
    }
}

void test_player_machine_code_roundtrip() {
    // マシン語の CMT も、再生 → 録音 → 変換 で戻る (先頭にダミーの 0x00 x8 が付く)。
    // ヘッダが無いので、途中にピー音は入らない
    std::vector<uint8_t> data = pc8001_machine_code(0xC000, 600, 31);
    for (int baud : {1200, 600}) {
        std::vector<Half> wave = play_cmt(data, baud);
        TEST_ASSERT_EQUAL_UINT32(2, leader_durations_ms(wave, 100).size());
        write_raw(RAW_PATH, wave);
        TEST_ASSERT_TRUE(convert_raw_to_cmt(RAW_PATH, "/RT.CMT"));
        std::vector<uint8_t> expected(8, 0x00);
        expected.insert(expected.end(), data.begin(), data.end());
        assert_bytes_equal(expected, SD.get("/RT.CMT"), "CMT machine code roundtrip");
    }
}

void test_player_p6_roundtrip() {
    // P6 を再生した波形を録音・変換すると、元の P6 とまったく同じになる
    for (int baud : {1200, 600}) {
        std::vector<uint8_t> data = p6_file();
        write_raw(RAW_PATH, play_file("/OUT.P6", data, baud));
        TEST_ASSERT_TRUE(convert_raw_to_cmt(RAW_PATH, "/RT.P6"));
        assert_bytes_equal(data, SD.get("/RT.P6"), "P6 roundtrip");
    }
}

// 外から渡された P6 (エミュレータで作ったものなど) を、PicoCmt で再生 → 録音 → 変換する
//   XCHECK_IN_DIR/input.p6 → XCHECK_OUT_DIR/pico_roundtrip.p6 (python tools/crosscheck.py p6-roundtrip)
void test_xcheck_roundtrip_input_p6() {
    std::vector<uint8_t> in;
    if (!xcheck_input("input.p6", in)) {
        TEST_IGNORE_MESSAGE("no input.p6 (XCHECK_IN_DIR)");
    }
    write_raw(RAW_PATH, play_file("/IN.P6", in, 1200));
    TEST_ASSERT_TRUE(convert_raw_to_cmt(RAW_PATH, "/RT.P6"));
    xcheck_dump("pico_roundtrip.p6", SD.get("/RT.P6"));
    assert_bytes_equal(in, SD.get("/RT.P6"), "P6 roundtrip");
}

// 外から渡された CMT (エミュレータで作ったものなど) を、PicoCmt で再生 → 録音 → 変換する
//   XCHECK_IN_DIR/input.cmt (1200ボー) / input_600.cmt (600ボー)
//   → XCHECK_OUT_DIR/pico_roundtrip.cmt / pico_roundtrip_600.cmt
//   (python tools/crosscheck.py cmt-roundtrip)
//   CMT の再生は先頭にダミーの 0x00 x8 を出すので、結果はその分だけ長くなる
static void roundtrip_input_cmt(const char* in_name, int baud, const char* out_name) {
    std::vector<uint8_t> in;
    if (!xcheck_input(in_name, in)) {
        TEST_IGNORE_MESSAGE("no input CMT (XCHECK_IN_DIR)");
    }
    write_raw(RAW_PATH, play_file("/IN.CMT", in, baud));
    TEST_ASSERT_TRUE(convert_raw_to_cmt(RAW_PATH, "/RT.CMT"));
    xcheck_dump(out_name, SD.get("/RT.CMT"));
    std::vector<uint8_t> expected(8, 0x00);
    expected.insert(expected.end(), in.begin(), in.end());
    assert_bytes_equal(expected, SD.get("/RT.CMT"), "CMT roundtrip");
}

void test_xcheck_roundtrip_input_cmt() {
    roundtrip_input_cmt("input.cmt", 1200, "pico_roundtrip.cmt");
}

void test_xcheck_roundtrip_input_cmt_600() {
    roundtrip_input_cmt("input_600.cmt", 600, "pico_roundtrip_600.cmt");
}

void test_xcheck_dump_for_papicom() {
    if (xcheck_dir("XCHECK_OUT_DIR").empty()) {
        TEST_IGNORE_MESSAGE("XCHECK_OUT_DIR not set");
    }
    std::vector<uint8_t> data = p6_file();
    xcheck_dump("p6_ref.p6", data);
    xcheck_dump("pico_play_p6_1200.wav", wave_to_wav(play_file("/OUT.P6", data, 1200), 48000));
}

void setUp() {
    SD.clear();
    mock_millis_value = 0;
    mock_sd_write_budget = SIZE_MAX;
}

void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_convert_1200);
    RUN_TEST(test_convert_600);
    RUN_TEST(test_convert_1200_with_jitter);
    RUN_TEST(test_convert_tape_speed_and_jitter);
    RUN_TEST(test_convert_long_block_without_gap);
    RUN_TEST(test_convert_machine_code);
    RUN_TEST(test_convert_reports_write_error);
    RUN_TEST(test_player_1200_outputs_file_bytes);
    RUN_TEST(test_player_600_outputs_file_bytes);
    RUN_TEST(test_player_pulse_timing);
    RUN_TEST(test_player_p6_leader_after_header);
    RUN_TEST(test_player_cmt_leader_after_header);
    RUN_TEST(test_player_cmt_leader_after_monitor_header);
    RUN_TEST(test_player_p6_uses_3_stop_bits);
    RUN_TEST(test_player_machine_code_roundtrip);
    RUN_TEST(test_player_p6_roundtrip);
    RUN_TEST(test_xcheck_roundtrip_input_p6);
    RUN_TEST(test_xcheck_roundtrip_input_cmt);
    RUN_TEST(test_xcheck_roundtrip_input_cmt_600);
    RUN_TEST(test_xcheck_dump_for_papicom);
    return UNITY_END();
}
