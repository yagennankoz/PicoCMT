// ============================================================================
// CAS (MSX) 変換・再生のテスト (pio test -e native)
// ============================================================================
#include <unity.h>

#include "cmt_cas_converter.h"
#include "crosscheck_utils.h"
#include "player_cas.h"
#include "tape_test_utils.h"

volatile uint32_t playback_base_time_ms = 0;
volatile uint32_t playback_tag_start_system_ms = 0;
volatile bool playback_time_valid = false;
void draw_conversion_progress(int) {}

static const char* RAW_PATH = "/RAW_TEMP.BIN";
static const char* CAS_PATH = "/OUT.CAS";

static const uint8_t CAS_SIG[8] = {0x1F, 0xA6, 0xDE, 0xBA, 0xCC, 0x13, 0x7D, 0x74};

// MSX BASIC のファイルヘッダ (D3 x10 + ファイル名6文字)
static std::vector<uint8_t> msx_header(const char* name) {
    std::vector<uint8_t> h(10, 0xD3);
    h.insert(h.end(), name, name + 6);
    return h;
}

// テープ上のブロック: ヘッダ / データ(300バイト) / ヘッダ / データ(50バイト)
// 2つ目のヘッダの前で、シグネチャを8バイト境界に合わせる埋め草が必要になる
static std::vector<std::vector<uint8_t>> msx_blocks() {
    return {msx_header("GAME  "), random_bytes(300, 9), msx_header("SAVE2 "),
            random_bytes(50, 10)};
}

// CASファイル: 各ブロックの前にシグネチャ。シグネチャは8の倍数の位置に置き、
// 足りない分は直前に 0x00 を埋める
static std::vector<uint8_t> cas_file(const std::vector<std::vector<uint8_t>>& blocks) {
    std::vector<uint8_t> out;
    for (const auto& b : blocks) {
        while (out.size() % 8 != 0) {
            out.push_back(0x00);
        }
        out.insert(out.end(), CAS_SIG, CAS_SIG + 8);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

// マシン語 (BSAVE): ヘッダ D0 x10 + 名前、データ = 開始・終了・実行アドレス + 中身
static std::vector<std::vector<uint8_t>> msx_binary_blocks() {
    std::vector<uint8_t> h(10, 0xD0);
    const char* name = "MCODE ";
    h.insert(h.end(), name, name + 6);
    std::vector<uint8_t> d = {0x00, 0xC0, 0xFF, 0xC0, 0x00, 0xC0};
    std::vector<uint8_t> body = random_bytes(256, 21);
    d.insert(d.end(), body.begin(), body.end());
    return {h, d};
}

// アスキー形式 (SAVE "CAS:"): ヘッダ EA x10 + 名前、データは 256バイトずつのブロックが続き、
// 最後のブロックは 0x1A で埋める
static std::vector<std::vector<uint8_t>> msx_ascii_blocks() {
    std::vector<uint8_t> h(10, 0xEA);
    const char* name = "TEXT  ";
    h.insert(h.end(), name, name + 6);
    std::vector<std::vector<uint8_t>> out = {h};
    for (int i = 0; i < 3; i++) {
        std::vector<uint8_t> b(256, 0x1A);
        size_t n = (i < 2) ? 256 : 100;
        for (size_t k = 0; k < n; k++) {
            b[k] = (uint8_t)(0x20 + (k * 7 + i) % 0x5F);  // 表示できる文字
        }
        out.push_back(b);
    }
    return out;
}

// ヘッダブロック (同じ値が10個続いて始まる) か
static bool is_header_block(const std::vector<uint8_t>& b) {
    if (b.size() < 10) {
        return false;
    }
    for (int i = 1; i < 10; i++) {
        if (b[i] != b[0]) {
            return false;
        }
    }
    return b[0] == 0xD3 || b[0] == 0xD0 || b[0] == 0xEA;
}

// MSX の録音に近い信号: ヘッダはロングヘッダ、データはショートヘッダの後に続き、
// 各ブロックの後に無音が入る
static TapeSignal make_signal(const FskFormat& f, double jitter,
                              const std::vector<std::vector<uint8_t>>& blocks) {
    TapeSignal s(f, jitter, 3);
    for (size_t i = 0; i < blocks.size(); i++) {
        s.leader_ms(is_header_block(blocks[i]) ? 2000 : 1700);
        s.bytes(blocks[i]);
        s.leader_ms(100);
        s.silence(500);
    }
    return s;
}

static TapeSignal make_signal(const FskFormat& f, double jitter = 0.0) {
    return make_signal(f, jitter, msx_blocks());
}

// ============================================================================
// 変換 (convert_raw_to_cas)
// ============================================================================
static void check_convert(const FskFormat& f, double jitter) {
    write_raw(RAW_PATH, make_signal(f, jitter).halves);
    TEST_ASSERT_TRUE(convert_raw_to_cas(RAW_PATH, CAS_PATH));
    assert_bytes_equal(cas_file(msx_blocks()), SD.get(CAS_PATH), "CAS");
}

void test_convert_1200() { check_convert(FSK_1200, 0.0); }
void test_convert_2400() { check_convert(FSK_2400, 0.0); }
void test_convert_1200_with_jitter() { check_convert(FSK_1200, 0.05); }

void test_convert_tape_speed_and_jitter() {
    // テープ速度 ±5% と揺らぎ ±10% (Mark と Space が混ざった半周期の組で
    // ビットの長さを測ってしまい、2400ボーで崩れていた)
    for (const FskFormat* f : {&FSK_1200, &FSK_2400}) {
        for (double speed : {0.95, 1.05}) {
            check_convert(at_speed(*f, speed), 0.1);
        }
    }
}

void test_convert_binary_and_ascii() {
    // マシン語 (BSAVE、D0) とアスキー形式 (EA、データが複数ブロック) も同じように読める
    for (const auto& blocks : {msx_binary_blocks(), msx_ascii_blocks()}) {
        for (const FskFormat* f : {&FSK_1200, &FSK_2400}) {
            write_raw(RAW_PATH, make_signal(*f, 0.05, blocks).halves);
            TEST_ASSERT_TRUE(convert_raw_to_cas(RAW_PATH, CAS_PATH));
            assert_bytes_equal(cas_file(blocks), SD.get(CAS_PATH), "CAS");
        }
    }
}

void test_convert_reports_write_error() {
    write_raw(RAW_PATH, make_signal(FSK_1200).halves);
    mock_sd_write_budget = 100;  // 途中でSDへの書き込みを失敗させる
    TEST_ASSERT_FALSE(convert_raw_to_cas(RAW_PATH, CAS_PATH));
}

// ============================================================================
// 再生 (PlayerCAS)
// ============================================================================
static std::vector<Half> play_cas(int baud,
                                  const std::vector<std::vector<uint8_t>>& blocks = msx_blocks()) {
    SD.put(CAS_PATH, cas_file(blocks));
    File f = SD.open(CAS_PATH, FILE_READ);
    PlayerCAS player;
    player.set_baud_rate(baud);
    TEST_ASSERT_TRUE(player.init(&f, CAS_PATH));
    return run_playback(player);
}

static void check_player_bytes(int baud, const FskFormat& f) {
    FskDecodeResult r = decode_fsk(play_cas(baud), f);
    std::vector<uint8_t> decoded;
    for (const auto& b : r.blocks) {
        decoded.insert(decoded.end(), b.begin(), b.end());
    }
    // シグネチャを除いた全バイト。埋め草の 0x00 はブロック末尾のデータとして出力される
    // (MSXのBIOSはブロックの長さ分だけ読むので影響しない)
    std::vector<uint8_t> expected;
    std::vector<uint8_t> file = cas_file(msx_blocks());
    for (size_t i = 0; i < file.size();) {
        if (i + 8 <= file.size() && memcmp(&file[i], CAS_SIG, 8) == 0) {
            i += 8;
        } else {
            expected.push_back(file[i++]);
        }
    }
    assert_bytes_equal(expected, decoded, "decoded");
    TEST_ASSERT_EQUAL_INT(0, r.framing_errors);
}

void test_player_1200_outputs_bytes() { check_player_bytes(1200, FSK_1200); }
void test_player_2400_outputs_bytes() { check_player_bytes(2400, FSK_2400); }

void test_player_blank_between_blocks() {
    // 各ブロックの前に約1秒の無音が入る
    std::vector<double> silences = silence_durations_ms(play_cas(1200));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(msx_blocks().size(), silences.size(), "silence count");
    for (double ms : silences) {
        TEST_ASSERT_DOUBLE_WITHIN(100.0, 1000.0, ms);
    }
}

void test_player_blocks_start_with_same_phase() {
    // どのブロックも High の半周期から始まる (無音の値を1つだけ入れていたため、
    // ブロックごとに位相が入れ替わり、wav2cas で読めなかった)
    for (int baud : {1200, 2400}) {
        std::vector<bool> levels = levels_after_silence(play_cas(baud));
        TEST_ASSERT_EQUAL_UINT32(msx_blocks().size(), levels.size());
        for (bool high : levels) {
            TEST_ASSERT_TRUE(high);
        }
    }
}

void test_player_pulse_timing() {
    HalfStats st = measure_halves(play_cas(1200), FSK_1200);
    TEST_ASSERT_DOUBLE_WITHIN(208.33 * 0.015, 208.33, st.mean_short);
    TEST_ASSERT_DOUBLE_WITHIN(416.67 * 0.015, 416.67, st.mean_long);

    st = measure_halves(play_cas(2400), FSK_2400);
    TEST_ASSERT_DOUBLE_WITHIN(104.17 * 0.015, 104.17, st.mean_short);
    TEST_ASSERT_DOUBLE_WITHIN(208.33 * 0.015, 208.33, st.mean_long);
}

void test_player_then_convert_roundtrip() {
    // 再生した波形を録音・変換すると元のCASに戻る (無音代わりのパルスは本物の無音にする)
    write_raw(RAW_PATH, normalize_silence(play_cas(1200)));
    TEST_ASSERT_TRUE(convert_raw_to_cas(RAW_PATH, "/RT.CAS"));
    assert_bytes_equal(cas_file(msx_blocks()), SD.get("/RT.CAS"), "roundtrip");
}

void test_player_binary_and_ascii_roundtrip() {
    // マシン語・アスキー形式も、再生 → 録音 → 変換 で元の CAS に戻る。
    // ヘッダの前はロングヘッダ (約2.4秒以上)、データの前はショートヘッダ
    for (const auto& blocks : {msx_binary_blocks(), msx_ascii_blocks()}) {
        for (int baud : {1200, 2400}) {
            std::vector<Half> wave = play_cas(baud, blocks);
            write_raw(RAW_PATH, normalize_silence(wave));
            TEST_ASSERT_TRUE(convert_raw_to_cas(RAW_PATH, "/RT.CAS"));
            assert_bytes_equal(cas_file(blocks), SD.get("/RT.CAS"), "roundtrip");
        }
    }
}

// ============================================================================
// 公開ツール castools (cas2wav / wav2cas) とのクロスチェック (python tools/crosscheck.py cas)
//   環境変数 XCHECK_OUT_DIR / XCHECK_IN_DIR が無ければスキップ
// ============================================================================
void test_xcheck_dump_for_castools() {
    if (xcheck_dir("XCHECK_OUT_DIR").empty()) {
        TEST_IGNORE_MESSAGE("XCHECK_OUT_DIR not set");
    }
    // 基準の CAS と、それを PicoCmt で再生した波形 (wav2cas で読む)
    xcheck_dump("cas_ref.cas", cas_file(msx_blocks()));
    xcheck_dump("pico_play_cas_1200.wav", wave_to_wav(normalize_silence(play_cas(1200)), 96000));
    xcheck_dump("pico_play_cas_2400.wav", wave_to_wav(normalize_silence(play_cas(2400)), 96000));

    // PicoCmt で録音・変換した CAS (cas2wav → wav2cas に通す)
    const FskFormat* formats[] = {&FSK_1200, &FSK_2400};
    const char* names[] = {"pico_rec_cas_1200.cas", "pico_rec_cas_2400.cas"};
    for (int i = 0; i < 2; i++) {
        write_raw(RAW_PATH, make_signal(*formats[i], 0.05).halves);
        TEST_ASSERT_TRUE(convert_raw_to_cas(RAW_PATH, CAS_PATH));
        assert_bytes_equal(cas_file(msx_blocks()), SD.get(CAS_PATH), names[i]);
        xcheck_dump(names[i], SD.get(CAS_PATH));
    }

    // マシン語 (BSAVE) とアスキー形式: 基準の CAS と、PicoCmt で再生した波形
    xcheck_dump("cas_bin.cas", cas_file(msx_binary_blocks()));
    xcheck_dump("cas_ascii.cas", cas_file(msx_ascii_blocks()));
    for (int baud : {1200, 2400}) {
        char name[48];
        snprintf(name, sizeof(name), "pico_play_cas_bin_%d.wav", baud);
        xcheck_dump(name, wave_to_wav(normalize_silence(play_cas(baud, msx_binary_blocks())), 96000));
        snprintf(name, sizeof(name), "pico_play_cas_ascii_%d.wav", baud);
        xcheck_dump(name, wave_to_wav(normalize_silence(play_cas(baud, msx_ascii_blocks())), 96000));
    }
}

// cas2wav が基準の CAS から作った WAV を、PicoCmt で録音・変換する
static void check_record_cas2wav(const char* wav_name, const char* out_name,
                                 const std::vector<std::vector<uint8_t>>& blocks = msx_blocks()) {
    std::vector<uint8_t> wav;
    if (!xcheck_input(wav_name, wav)) {
        TEST_IGNORE_MESSAGE("no input from cas2wav (XCHECK_IN_DIR)");
    }
    write_raw(RAW_PATH, wav_to_halves(wav));
    TEST_ASSERT_TRUE(convert_raw_to_cas(RAW_PATH, CAS_PATH));
    xcheck_dump(out_name, SD.get(CAS_PATH));
    assert_bytes_equal(cas_file(blocks), SD.get(CAS_PATH), wav_name);
}

void test_xcheck_record_cas2wav_1200() {
    check_record_cas2wav("cas2wav_1200.wav", "pico_from_cas2wav_1200.cas");
}
void test_xcheck_record_cas2wav_2400() {
    check_record_cas2wav("cas2wav_2400.wav", "pico_from_cas2wav_2400.cas");
}
void test_xcheck_record_cas2wav_bin_ascii() {
    // マシン語 (cas_bin.cas) とアスキー形式 (cas_ascii.cas) を cas2wav にしたもの
    const char* kinds[] = {"bin", "ascii"};
    for (int k = 0; k < 2; k++) {
        for (int baud : {1200, 2400}) {
            char wav[48], out[64];
            snprintf(wav, sizeof(wav), "cas2wav_%s_%d.wav", kinds[k], baud);
            snprintf(out, sizeof(out), "pico_from_cas2wav_%s_%d.cas", kinds[k], baud);
            check_record_cas2wav(wav, out, k == 0 ? msx_binary_blocks() : msx_ascii_blocks());
        }
    }
}

// 外から渡された CAS (エミュレータで作ったもの) を 再生 → 録音 → 変換 し、元と一致するか
//   (python tools/crosscheck.py cas-roundtrip)
static void roundtrip_input_cas(const char* in_name, int baud, const char* out_name) {
    std::vector<uint8_t> in;
    if (!xcheck_input(in_name, in)) {
        TEST_IGNORE_MESSAGE("no input CAS (XCHECK_IN_DIR)");
    }
    SD.put("/IN.CAS", in);
    File f = SD.open("/IN.CAS", FILE_READ);
    PlayerCAS player;
    player.set_baud_rate(baud);
    TEST_ASSERT_TRUE(player.init(&f, "/IN.CAS"));
    write_raw(RAW_PATH, normalize_silence(run_playback(player)));
    TEST_ASSERT_TRUE(convert_raw_to_cas(RAW_PATH, "/RT.CAS"));
    xcheck_dump(out_name, SD.get("/RT.CAS"));
    assert_bytes_equal(in, SD.get("/RT.CAS"), "CAS roundtrip");
}

void test_xcheck_roundtrip_input_cas() {
    roundtrip_input_cas("input.cas", 1200, "pico_roundtrip.cas");
}

void test_xcheck_roundtrip_input_cas_2400() {
    roundtrip_input_cas("input_2400.cas", 2400, "pico_roundtrip_2400.cas");
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
    RUN_TEST(test_convert_2400);
    RUN_TEST(test_convert_1200_with_jitter);
    RUN_TEST(test_convert_tape_speed_and_jitter);
    RUN_TEST(test_convert_binary_and_ascii);
    RUN_TEST(test_convert_reports_write_error);
    RUN_TEST(test_player_1200_outputs_bytes);
    RUN_TEST(test_player_2400_outputs_bytes);
    RUN_TEST(test_player_blank_between_blocks);
    RUN_TEST(test_player_blocks_start_with_same_phase);
    RUN_TEST(test_player_pulse_timing);
    RUN_TEST(test_player_then_convert_roundtrip);
    RUN_TEST(test_player_binary_and_ascii_roundtrip);
    RUN_TEST(test_xcheck_dump_for_castools);
    RUN_TEST(test_xcheck_record_cas2wav_1200);
    RUN_TEST(test_xcheck_record_cas2wav_2400);
    RUN_TEST(test_xcheck_record_cas2wav_bin_ascii);
    RUN_TEST(test_xcheck_roundtrip_input_cas);
    RUN_TEST(test_xcheck_roundtrip_input_cas_2400);
    return UNITY_END();
}
