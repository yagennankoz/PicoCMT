// ============================================================================
// P6T (PC-6001) 変換・再生のテスト (pio test -e native)
//   PC-6001 の CSAVE に近い信号 → 録音PIOモデル → RAW → convert_raw_to_p6t() → P6T
//   → PlayerP6T → 再生PIOモデル → 波形
// ============================================================================
#include <unity.h>

#include "cmt_p6t_converter.h"
#include "cmt_p6t_format.h"
#include "crosscheck_utils.h"
#include "player_p6t.h"
#include "tape_test_utils.h"

volatile uint32_t playback_base_time_ms = 0;
volatile uint32_t playback_tag_start_system_ms = 0;
volatile bool playback_time_valid = false;
void draw_conversion_progress(int) {}

static const char* RAW_PATH = "/RAW_TEMP.BIN";
static const char* P6T_PATH = "/OUT.P6T";

// PC-6001 の BASIC のファイル: ヘッダ (D3 x10 + ファイル名6文字) と、プログラム + 00 x12
static std::vector<uint8_t> p6_header(const char* name) {
    std::vector<uint8_t> h(10, 0xD3);
    h.insert(h.end(), name, name + 6);
    return h;
}

static std::vector<uint8_t> p6_program(size_t n, uint32_t seed) {
    std::vector<uint8_t> p = random_bytes(n, seed);
    p.insert(p.end(), 12, 0x00);
    return p;
}

struct TestFile {
    const char* name;
    const FskFormat* fmt;
    int baud;
    double leader_ms;      // ヘッダの前のピー音
    double mid_leader_ms;  // ヘッダとプログラムの間のピー音
    double silence_ms;     // ファイルの後の無音
    std::vector<uint8_t> header;
    std::vector<uint8_t> program;
};

static std::vector<TestFile> test_files() {
    return {
        {"GAME  ", &FSK_P6_1200, 1200, 3000, 500, 1000, p6_header("GAME  "), p6_program(200, 1)},
        {"SAVE2 ", &FSK_P6_600, 600, 2000, 400, 500, p6_header("SAVE2 "), p6_program(100, 2)},
    };
}

// CSAVE と同じ並び: 長いピー音 → ヘッダ → 短いピー音 → プログラム → 終わりのピー音 → 無音
static std::vector<Half> make_signal(double speed = 1.0, double jitter = 0.0) {
    std::vector<Half> out;
    for (const TestFile& f : test_files()) {
        TapeSignal s(at_speed(*f.fmt, speed), jitter, 5);
        s.leader_ms(f.leader_ms);
        s.bytes(f.header);
        s.leader_ms(f.mid_leader_ms);
        s.bytes(f.program);
        s.leader_ms(200);
        s.silence(f.silence_ms);
        out.insert(out.end(), s.halves.begin(), s.halves.end());
    }
    return out;
}

static std::vector<uint8_t> expected_data() {
    std::vector<uint8_t> d;
    for (const TestFile& f : test_files()) {
        d.insert(d.end(), f.header.begin(), f.header.end());
        d.insert(d.end(), f.program.begin(), f.program.end());
    }
    return d;
}

static P6TInfo read_info(const char* path) {
    File f = SD.open(path, FILE_READ);
    P6TInfo info;
    TEST_ASSERT_TRUE(p6t_read_info(f, info));
    f.close();
    return info;
}

static std::vector<uint8_t> data_part(const char* path, const P6TInfo& info) {
    std::vector<uint8_t> d = SD.get(path);
    TEST_ASSERT_TRUE(d.size() >= info.data_size);
    return std::vector<uint8_t>(d.begin(), d.begin() + info.data_size);
}

// 変換結果のブロック: ヘッダ / プログラム が2ファイルぶん
static void assert_blocks(const P6TInfo& info, double tol_ms) {
    TEST_ASSERT_TRUE(info.has_footer);
    TEST_ASSERT_EQUAL_UINT32(4, info.blocks.size());
    std::vector<TestFile> files = test_files();
    uint32_t offset = 0;
    for (int i = 0; i < 4; i++) {
        const TestFile& f = files[i / 2];
        const P6TBlock& b = info.blocks[i];
        bool header = (i % 2 == 0);
        char msg[48];
        snprintf(msg, sizeof(msg), "block %d", i);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(offset, b.offset, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(header ? f.header.size() : f.program.size(), b.size, msg);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(f.baud, b.baud, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(i / 2, b.id, msg);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(f.name, b.name, 6, msg);
        // 無音は前のファイルの後だけ。ピー音はヘッダの前 (長い) とプログラムの前 (短い)
        double stime = (i == 2) ? files[0].silence_ms : 0;
        double ptime = header ? f.leader_ms : f.mid_leader_ms;
        TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(tol_ms, stime, b.stime_ms, msg);
        TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(tol_ms, ptime, b.ptime_ms, msg);
        offset += b.size;
    }
}

// ============================================================================
// 変換 (convert_raw_to_p6t)
// ============================================================================
void test_convert_blocks() {
    write_raw(RAW_PATH, make_signal());
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
    P6TInfo info = read_info(P6T_PATH);
    // 先頭のピー音は録音の開始 (トリガ) までの分だけ短くなる
    assert_blocks(info, 60);
    assert_bytes_equal(expected_data(), data_part(P6T_PATH, info), "P6T data");
}

void test_convert_tape_speed_and_jitter() {
    for (double speed : {0.95, 1.05}) {
        write_raw(RAW_PATH, make_signal(speed, 0.1));
        TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
        P6TInfo info = read_info(P6T_PATH);
        TEST_ASSERT_EQUAL_UINT32(4, info.blocks.size());
        assert_bytes_equal(expected_data(), data_part(P6T_PATH, info), "P6T data");
        TEST_ASSERT_EQUAL_UINT16(1200, info.blocks[0].baud);
        TEST_ASSERT_EQUAL_UINT16(600, info.blocks[2].baud);
    }
}

void test_footer_layout() {
    // PC6001VX の p6t2.cpp と同じ並び (リトルエンディアン)
    write_raw(RAW_PATH, make_signal());
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
    std::vector<uint8_t> d = SD.get(P6T_PATH);
    uint32_t data_size = (uint32_t)expected_data().size();
    size_t n = d.size();
    uint32_t tail = d[n - 4] | (d[n - 3] << 8) | (d[n - 2] << 16) | ((uint32_t)d[n - 1] << 24);
    TEST_ASSERT_EQUAL_UINT32(data_size, tail);
    // フッタ 11バイト + ブロック 33バイト x4 + サイズ 4バイト
    TEST_ASSERT_EQUAL_UINT32(data_size + 11 + 33 * 4 + 4, n);

    const uint8_t* f = d.data() + data_size;
    const uint8_t head[] = {'P', '6', 2, 4, 0, 1, 1, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_MEMORY(head, f, sizeof(head));
    const uint8_t* b = f + sizeof(head);
    TEST_ASSERT_EQUAL_MEMORY("TI", b, 2);
    TEST_ASSERT_EQUAL_UINT8(0, b[2]);                  // ID
    TEST_ASSERT_EQUAL_MEMORY("GAME  \0", b + 3, 7);    // 名前
    TEST_ASSERT_EQUAL_UINT8(1200 & 0xFF, b[19]);       // ボーレート
    TEST_ASSERT_EQUAL_UINT8(1200 >> 8, b[20]);
    TEST_ASSERT_EQUAL_UINT32(0, b[25] | (b[26] << 8) | (b[27] << 16) | (b[28] << 24));  // オフセット
    TEST_ASSERT_EQUAL_UINT32(16, b[29] | (b[30] << 8) | (b[31] << 16) | (b[32] << 24));  // サイズ
    // 2つ目のブロック (プログラム) は同じ ID・同じ名前
    TEST_ASSERT_EQUAL_MEMORY("TI", b + 33, 2);
    TEST_ASSERT_EQUAL_UINT8(0, b[35]);
    TEST_ASSERT_EQUAL_MEMORY("GAME  \0", b + 36, 7);
}

void test_convert_reports_write_error() {
    write_raw(RAW_PATH, make_signal());
    mock_sd_write_budget = 100;
    TEST_ASSERT_FALSE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
}

// ============================================================================
// 再生 (PlayerP6T)
// ============================================================================
static std::vector<Half> play(const char* path) {
    File f = SD.open(path, FILE_READ);
    PlayerP6T player;
    TEST_ASSERT_TRUE(player.init(&f, path));
    return run_playback(player);
}

void test_player_roundtrip() {
    // 再生した波形を録音・変換すると、同じデータ・同じブロックの P6T に戻る
    write_raw(RAW_PATH, make_signal());
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
    P6TInfo orig = read_info(P6T_PATH);

    write_raw(RAW_PATH, normalize_silence(play(P6T_PATH)));
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, "/RT.P6T"));
    P6TInfo rt = read_info("/RT.P6T");
    assert_bytes_equal(data_part(P6T_PATH, orig), data_part("/RT.P6T", rt), "roundtrip");
    TEST_ASSERT_EQUAL_UINT32(orig.blocks.size(), rt.blocks.size());
    for (size_t i = 0; i < orig.blocks.size(); i++) {
        TEST_ASSERT_EQUAL_UINT16(orig.blocks[i].baud, rt.blocks[i].baud);
        TEST_ASSERT_EQUAL_UINT32(orig.blocks[i].size, rt.blocks[i].size);
        TEST_ASSERT_EQUAL_UINT8(orig.blocks[i].id, rt.blocks[i].id);
        TEST_ASSERT_DOUBLE_WITHIN(30, orig.blocks[i].stime_ms, rt.blocks[i].stime_ms);
        if (i > 0) {  // 先頭は録音のトリガの分だけ短くなる
            TEST_ASSERT_DOUBLE_WITHIN(30, orig.blocks[i].ptime_ms, rt.blocks[i].ptime_ms);
        }
    }
}

// 1ブロックの P6T を作る
static void write_single_block_p6t(const char* path, const std::vector<uint8_t>& data,
                                   uint16_t baud, uint16_t stime, uint16_t ptime) {
    SD.put(path, {});
    File f = SD.open(path, FILE_WRITE);
    SdBufferedWrite w(&f);
    w.write_bytes(data.data(), data.size());
    P6TBlock b;
    b.baud = baud;
    b.stime_ms = stime;
    b.ptime_ms = ptime;
    b.size = (uint32_t)data.size();
    p6t_write_footer(&w, {b}, (uint32_t)data.size());
    w.flush();
    f.close();
}

// ピー音 (短い半周期の連続) のうち 50ms 以上のものの (始まり, 長さ) us
static std::vector<std::pair<double, double>> leaders(const std::vector<Half>& wave) {
    std::vector<std::pair<double, double>> out;
    double t = 0, run = 0;
    for (const Half& h : wave) {
        if (h.us < 312) {
            run += h.us;
        } else {
            if (run >= 50000) {
                out.push_back({t - run, run});
            }
            run = 0;
        }
        t += h.us;
    }
    if (run >= 50000) {
        out.push_back({t - run, run});
    }
    return out;
}

void test_player_uses_block_baud_and_3_stop_bits() {
    // 1バイト = 12ビット (ストップビット3)。ボーレートはブロックの値を使う
    std::vector<uint8_t> data = random_bytes(300, 3);
    for (uint16_t baud : {1200, 600}) {
        write_single_block_p6t(P6T_PATH, data, baud, 0, 1000);
        std::vector<Half> wave = play(P6T_PATH);
        auto l = leaders(wave);
        TEST_ASSERT_EQUAL_UINT32(2, l.size());  // ブロックの前と最後
        TEST_ASSERT_DOUBLE_WITHIN(5000, 1000000, l[0].second);
        double data_us = l[1].first - (l[0].first + l[0].second);
        TEST_ASSERT_DOUBLE_WITHIN(0.05, 12.0, data_us / 1e6 * baud / data.size());

        FskDecodeResult r = decode_fsk(wave, baud == 1200 ? FSK_P6_1200 : FSK_P6_600);
        TEST_ASSERT_EQUAL_UINT32(1, r.blocks.size());
        assert_bytes_equal(data, r.blocks[0], "decoded");
    }
}

void test_player_silence_and_leader_times() {
    // 無音 → ピー音 → データ を、ブロックの時間どおりに出す
    write_raw(RAW_PATH, make_signal());
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
    P6TInfo info = read_info(P6T_PATH);
    std::vector<Half> wave = play(P6T_PATH);

    // 無音は2つ目のファイルのヘッダ (ブロック2) の前だけ
    std::vector<double> silences = silence_durations_ms(wave);
    TEST_ASSERT_EQUAL_UINT32(1, silences.size());
    TEST_ASSERT_DOUBLE_WITHIN(20, info.blocks[2].stime_ms, silences[0]);

    auto l = leaders(normalize_silence(wave));
    TEST_ASSERT_EQUAL_UINT32(5, l.size());  // 各ブロックの前 x4 + 最後
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_DOUBLE_WITHIN(5, info.blocks[i].ptime_ms, l[i].second / 1000);
    }
}

void test_player_plain_p6_as_one_block() {
    // フッタの無いファイルは、PC6001VX と同じく 1ブロック (無音3.4秒、ピー音6.8秒、1200ボー)。
    // ヘッダの後にプログラムが続くので、間にピー音 (1秒) を入れる
    std::vector<uint8_t> data = p6_header("PLAIN ");
    std::vector<uint8_t> prog = p6_program(100, 4);
    data.insert(data.end(), prog.begin(), prog.end());
    SD.put(P6T_PATH, data);

    P6TInfo info = read_info(P6T_PATH);
    TEST_ASSERT_FALSE(info.has_footer);
    TEST_ASSERT_EQUAL_UINT32(1, info.blocks.size());

    std::vector<Half> wave = play(P6T_PATH);
    auto l = leaders(normalize_silence(wave));
    TEST_ASSERT_EQUAL_UINT32(3, l.size());
    TEST_ASSERT_DOUBLE_WITHIN(5, 6800, l[0].second / 1000);
    TEST_ASSERT_DOUBLE_WITHIN(5, 1000, l[1].second / 1000);
    FskDecodeResult r = decode_fsk(wave, FSK_P6_1200);
    std::vector<uint8_t> decoded;
    for (const auto& b : r.blocks) {
        decoded.insert(decoded.end(), b.begin(), b.end());
    }
    assert_bytes_equal(data, decoded, "plain P6");
}

void test_player_keeps_phase() {
    // 無音の後もピー音は High の半周期から始まる
    write_raw(RAW_PATH, make_signal());
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
    // (無音に入れる値が、キューの空き不足で1つしか入らないと位相が入れ替わっていた)
    std::vector<bool> levels = levels_after_silence(play(P6T_PATH));
    TEST_ASSERT_EQUAL_UINT32(1, levels.size());
    for (bool high : levels) {
        TEST_ASSERT_TRUE(high);
    }
}

void test_player_seek_moves_by_block() {
    write_raw(RAW_PATH, make_signal());
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
    File f = SD.open(P6T_PATH, FILE_READ);
    PlayerP6T player;
    TEST_ASSERT_TRUE(player.init(&f, P6T_PATH));
    queue_t q;
    queue_init(&q, sizeof(uint16_t), PLAY_QUEUE_LENGTH);

    // 次のブロックへ 3回進めると最後のブロック。それ以上は進めない
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(player.seek(1, &q));
    }
    TEST_ASSERT_FALSE(player.seek(1, &q));
    // 最後のブロック (プログラム, 112バイト) だけが再生される
    std::vector<Half> wave = run_playback(player);
    FskDecodeResult r = decode_fsk(wave, FSK_P6_600);
    TEST_ASSERT_EQUAL_UINT32(1, r.blocks.size());
    assert_bytes_equal(test_files()[1].program, r.blocks[0], "last block");
}

// ============================================================================
// 追記 (p6t_append)
// ============================================================================
void test_append_merges_blocks() {
    write_raw(RAW_PATH, make_signal());
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, "/A.P6T"));
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, "/B.P6T"));
    TEST_ASSERT_TRUE(p6t_append("/A.P6T", "/B.P6T", "/M.P6T"));

    P6TInfo a = read_info("/A.P6T");
    P6TInfo m = read_info("/M.P6T");
    TEST_ASSERT_EQUAL_UINT32(8, m.blocks.size());
    TEST_ASSERT_EQUAL_UINT32(a.data_size * 2, m.data_size);
    std::vector<uint8_t> twice = expected_data();
    std::vector<uint8_t> once = expected_data();
    twice.insert(twice.end(), once.begin(), once.end());
    assert_bytes_equal(twice, data_part("/M.P6T", m), "merged");
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(a.blocks[i].offset + a.data_size, m.blocks[i + 4].offset);
        TEST_ASSERT_EQUAL_UINT8(a.blocks[i].id + 2, m.blocks[i + 4].id);  // ID は後ろに続ける
        TEST_ASSERT_EQUAL_UINT16(a.blocks[i].baud, m.blocks[i + 4].baud);
    }
}

// ============================================================================
// 公開ツールとのクロスチェック (python tools/crosscheck.py p6t)
//   環境変数 XCHECK_OUT_DIR が無ければスキップ
// ============================================================================
void test_xcheck_dump_for_p6t() {
    if (xcheck_dir("XCHECK_OUT_DIR").empty()) {
        TEST_IGNORE_MESSAGE("XCHECK_OUT_DIR not set");
    }
    // 録音・変換した P6T と、そのデータ (PC6001VX と同じ読み方で確かめる)
    write_raw(RAW_PATH, make_signal());
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
    xcheck_dump("pico_rec.p6t", SD.get(P6T_PATH));
    xcheck_dump("p6t_rec_ref.bin", expected_data());

    // 1ファイル (ヘッダ + プログラム) の P6T を再生した波形 (PapicomTapeConvertor で読む)
    const TestFile f = test_files()[0];
    std::vector<uint8_t> data = f.header;
    data.insert(data.end(), f.program.begin(), f.program.end());
    SD.put(P6T_PATH, {});
    File out = SD.open(P6T_PATH, FILE_WRITE);
    SdBufferedWrite w(&out);
    w.write_bytes(data.data(), data.size());
    P6TBlock h, p;
    h.stime_ms = 1000;
    h.ptime_ms = 3000;
    h.size = (uint32_t)f.header.size();
    p.ptime_ms = 500;
    p.offset = h.size;
    p.size = (uint32_t)f.program.size();
    p6t_write_footer(&w, {h, p}, (uint32_t)data.size());
    w.flush();
    out.close();
    xcheck_dump("pico_play_p6t.wav", wave_to_wav(play(P6T_PATH), 48000));
    xcheck_dump("p6t_play_ref.bin", data);
}

// 外から渡された P6T (エミュレータで作ったものなど) を、PicoCmt で再生 → 録音 → 変換する
//   XCHECK_IN_DIR/input.p6t → XCHECK_OUT_DIR/pico_roundtrip.p6t (python tools/crosscheck.py p6t-roundtrip)
void test_xcheck_roundtrip_input_p6t() {
    std::vector<uint8_t> in;
    if (!xcheck_input("input.p6t", in)) {
        TEST_IGNORE_MESSAGE("no input.p6t (XCHECK_IN_DIR)");
    }
    SD.put("/IN.P6T", in);
    P6TInfo src = read_info("/IN.P6T");
    std::vector<uint8_t> src_data = data_part("/IN.P6T", src);

    write_raw(RAW_PATH, normalize_silence(play("/IN.P6T")));
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
    xcheck_dump("pico_roundtrip.p6t", SD.get(P6T_PATH));

    P6TInfo rt = read_info(P6T_PATH);
    assert_bytes_equal(src_data, data_part(P6T_PATH, rt), "roundtrip data");
}

// 外から渡された2つの P6T を PicoCmt の追記処理で結合し、さらに 再生 → 録音 → 変換 する
//   XCHECK_IN_DIR/append_a.p6t + append_b.p6t
//   → XCHECK_OUT_DIR/pico_append.p6t (結合しただけ) / pico_append_roundtrip.p6t
//   (python tools/crosscheck.py p6t-append)
void test_xcheck_append_input_p6t() {
    std::vector<uint8_t> a, b;
    if (!xcheck_input("append_a.p6t", a) || !xcheck_input("append_b.p6t", b)) {
        TEST_IGNORE_MESSAGE("no append_a.p6t / append_b.p6t (XCHECK_IN_DIR)");
    }
    SD.put("/A.P6T", a);
    SD.put("/B.P6T", b);
    TEST_ASSERT_TRUE(p6t_append("/A.P6T", "/B.P6T", "/M.P6T"));
    xcheck_dump("pico_append.p6t", SD.get("/M.P6T"));

    std::vector<uint8_t> expected = data_part("/A.P6T", read_info("/A.P6T"));
    std::vector<uint8_t> b_data = data_part("/B.P6T", read_info("/B.P6T"));
    expected.insert(expected.end(), b_data.begin(), b_data.end());
    assert_bytes_equal(expected, data_part("/M.P6T", read_info("/M.P6T")), "appended");

    write_raw(RAW_PATH, normalize_silence(play("/M.P6T")));
    TEST_ASSERT_TRUE(convert_raw_to_p6t(RAW_PATH, P6T_PATH));
    xcheck_dump("pico_append_roundtrip.p6t", SD.get(P6T_PATH));
    assert_bytes_equal(expected, data_part(P6T_PATH, read_info(P6T_PATH)), "roundtrip");
}

void setUp() {
    SD.clear();
    mock_millis_value = 0;
    mock_sd_write_budget = SIZE_MAX;
}

void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_convert_blocks);
    RUN_TEST(test_convert_tape_speed_and_jitter);
    RUN_TEST(test_footer_layout);
    RUN_TEST(test_convert_reports_write_error);
    RUN_TEST(test_player_roundtrip);
    RUN_TEST(test_player_uses_block_baud_and_3_stop_bits);
    RUN_TEST(test_player_silence_and_leader_times);
    RUN_TEST(test_player_plain_p6_as_one_block);
    RUN_TEST(test_player_keeps_phase);
    RUN_TEST(test_player_seek_moves_by_block);
    RUN_TEST(test_append_merges_blocks);
    RUN_TEST(test_xcheck_dump_for_p6t);
    RUN_TEST(test_xcheck_roundtrip_input_p6t);
    RUN_TEST(test_xcheck_append_input_p6t);
    return UNITY_END();
}
