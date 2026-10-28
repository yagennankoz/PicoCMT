// ============================================================================
// T88 (PC-8801 タグ形式) 変換・再生のテスト (pio test -e native)
// ============================================================================
#include <unity.h>

#include "cmt_t88_converter.h"
#include "crosscheck_utils.h"
#include "player_t88.h"
#include "tape_test_utils.h"

volatile uint32_t playback_base_time_ms = 0;
volatile uint32_t playback_tag_start_system_ms = 0;
volatile bool playback_time_valid = false;
void draw_conversion_progress(int) {}

static const char* RAW_PATH = "/RAW_TEMP.BIN";
static const char* T88_PATH = "/OUT.T88";

struct T88Tag {
    uint16_t id;
    uint32_t start;
    uint32_t len;
    uint16_t data_type;
    std::vector<uint8_t> payload;
};

// strict = false は外から渡されたファイル用 (END の後の余りを許す)
static std::vector<T88Tag> parse_t88(const std::vector<uint8_t>& d, bool strict = true) {
    TEST_ASSERT_TRUE_MESSAGE(d.size() >= 24, "T88 too short");
    TEST_ASSERT_EQUAL_MEMORY(T88_SIGNATURE, d.data(), 24);

    auto u16 = [&](size_t p) { return (uint16_t)(d[p] | (d[p + 1] << 8)); };
    auto u32 = [&](size_t p) { return (uint32_t)(u16(p) | ((uint32_t)u16(p + 2) << 16)); };

    std::vector<T88Tag> tags;
    size_t pos = 24;
    while (pos + 4 <= d.size()) {
        T88Tag t = {u16(pos), 0, 0, 0, {}};
        uint16_t size = u16(pos + 2);
        TEST_ASSERT_TRUE_MESSAGE(pos + 4 + size <= d.size(), "T88 tag overruns file");
        if ((t.id == T88_TAG_BLANK || t.id == T88_TAG_SPACE || t.id == T88_TAG_MARK ||
             t.id == T88_TAG_DATA) &&
            size >= 8) {
            t.start = u32(pos + 4);
            t.len = u32(pos + 8);
        }
        if (t.id == T88_TAG_DATA) {
            uint16_t actual_len = u16(pos + 12);
            t.data_type = u16(pos + 14);
            t.payload.assign(d.begin() + pos + 16, d.begin() + pos + 16 + actual_len);
        }
        tags.push_back(t);
        pos += 4 + size;
        if (t.id == T88_TAG_END) {
            break;
        }
    }
    if (strict) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(d.size(), pos, "trailing bytes after END tag");
    }
    return tags;
}

// リーダー + データ + 短いマーク + 無音 を繰り返す
static TapeSignal make_signal(const FskFormat& f,
                              const std::vector<std::vector<uint8_t>>& blocks,
                              double silence_ms = 500) {
    TapeSignal s(f, 0.0, 3);
    for (const auto& b : blocks) {
        s.leader_ms(1000);
        s.bytes(b);
        s.leader_ms(300);
        s.silence(silence_ms);
    }
    return s;
}

static std::vector<std::vector<uint8_t>> test_blocks() {
    return {random_bytes(100, 1), random_bytes(300, 2)};
}

// ============================================================================
// 変換 (convert_raw_to_t88)
// ============================================================================
static std::vector<T88Tag> convert(const TapeSignal& s) {
    write_raw(RAW_PATH, s.halves);
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, T88_PATH));
    return parse_t88(SD.get(T88_PATH));
}

static void check_data_tags(const std::vector<T88Tag>& tags,
                            const std::vector<std::vector<uint8_t>>& blocks,
                            uint16_t data_type) {
    std::vector<const T88Tag*> data;
    for (const T88Tag& t : tags) {
        if (t.id == T88_TAG_DATA) {
            data.push_back(&t);
        }
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(blocks.size(), data.size(), "DATA tag count");
    for (size_t i = 0; i < blocks.size(); i++) {
        TEST_ASSERT_EQUAL_HEX16(data_type, data[i]->data_type);
        assert_bytes_equal(blocks[i], data[i]->payload, "DATA payload");
    }
}

void test_convert_structure_1200() {
    auto blocks = test_blocks();
    std::vector<T88Tag> tags = convert(make_signal(FSK_1200, blocks));

    TEST_ASSERT_EQUAL_HEX16(T88_TAG_VERSION, tags.front().id);
    TEST_ASSERT_EQUAL_HEX16(T88_TAG_END, tags.back().id);
    check_data_tags(tags, blocks, 0x01CC);

    // 無音 500ms (= 2400 ticks) が BLANK タグになる (RAWの ms 切り捨て分を許容)
    int blanks = 0;
    for (const T88Tag& t : tags) {
        if (t.id == T88_TAG_BLANK) {
            TEST_ASSERT_UINT32_WITHIN(10, 2400, t.len);
            blanks++;
        }
    }
    TEST_ASSERT_EQUAL_INT(2, blanks);
}

void test_convert_structure_600() {
    auto blocks = test_blocks();
    check_data_tags(convert(make_signal(FSK_600, blocks)), blocks, 0x00CC);
}

void test_convert_tag_times_follow_signal() {
    // ブロック間に 5ms のドロップアウト(無音扱いにならない長さのパルス)を入れる
    auto blocks = test_blocks();
    TapeSignal s(FSK_1200, 0.0, 3);
    s.leader_ms(1000);
    s.bytes(blocks[0]);
    s.leader_ms(300);
    s.pulse(5000);
    s.leader_ms(1000);
    s.bytes(blocks[1]);
    s.leader_ms(300);
    s.silence(500);
    size_t first = write_raw(RAW_PATH, s.halves);
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, T88_PATH));
    std::vector<T88Tag> tags = parse_t88(SD.get(T88_PATH));

    // 各タグの開始時刻 + 長さ が次のタグの開始時刻と一致し、
    // 全体の長さが録音した信号の長さと一致すること
    double signal_us = 0;
    for (size_t i = first; i < s.halves.size(); i++) {
        signal_us += s.halves[i].us;
    }
    uint32_t end_ticks = 0;
    for (const T88Tag& t : tags) {
        if (t.len == 0) {
            continue;
        }
        TEST_ASSERT_UINT32_WITHIN_MESSAGE(2, end_ticks, t.start, "tags are not contiguous");
        end_ticks = t.start + t.len;
    }
    double expected_ticks = signal_us * 4800.0 / 1e6;
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(expected_ticks * 0.002 + 6, expected_ticks, end_ticks,
                                      "total length differs from the signal");
}

void test_convert_reports_write_error() {
    write_raw(RAW_PATH, make_signal(FSK_1200, test_blocks()).halves);
    mock_sd_write_budget = 100;  // 途中でSDへの書き込みを失敗させる
    TEST_ASSERT_FALSE(convert_raw_to_t88(RAW_PATH, T88_PATH));
}

// ============================================================================
// 再生 (PlayerT88)
// ============================================================================
static std::vector<Half> play_converted(const std::vector<std::vector<uint8_t>>& blocks,
                                        double silence_ms = 500) {
    convert(make_signal(FSK_1200, blocks, silence_ms));
    File f = SD.open(T88_PATH, FILE_READ);
    PlayerT88 player;
    TEST_ASSERT_TRUE(player.init(&f, T88_PATH));
    return run_playback(player);
}

void test_player_outputs_data_bytes() {
    auto blocks = test_blocks();
    FskDecodeResult r = decode_fsk(play_converted(blocks), FSK_1200);
    std::vector<uint8_t> decoded;
    for (const auto& b : r.blocks) {
        decoded.insert(decoded.end(), b.begin(), b.end());
    }
    std::vector<uint8_t> expected = blocks[0];
    expected.insert(expected.end(), blocks[1].begin(), blocks[1].end());
    assert_bytes_equal(expected, decoded, "decoded");
    TEST_ASSERT_EQUAL_INT(0, r.framing_errors);
}

void test_player_pulse_timing() {
    HalfStats st = measure_halves(play_converted(test_blocks()), FSK_1200);
    TEST_ASSERT_DOUBLE_WITHIN(208.33 * 0.015, 208.33, st.mean_short);
    TEST_ASSERT_DOUBLE_WITHIN(416.67 * 0.015, 416.67, st.mean_long);
}

void test_player_blank_duration() {
    // BLANK タグ (1000ms) の長さだけ無音が出力されること
    std::vector<double> silences = silence_durations_ms(play_converted(test_blocks(), 1000));
    TEST_ASSERT_TRUE_MESSAGE(silences.size() >= 1, "no silence in output");
    TEST_ASSERT_DOUBLE_WITHIN(100.0, 1000.0, silences[0]);
}

void test_player_keeps_phase_after_blank() {
    // BLANK の後も、最初の半周期は High (無音の値を2つ入れて位相を保つ)
    std::vector<bool> levels = levels_after_silence(play_converted(test_blocks(), 1000));
    TEST_ASSERT_TRUE_MESSAGE(levels.size() >= 1, "no silence in output");
    for (bool high : levels) {
        TEST_ASSERT_TRUE(high);
    }
}

// ============================================================================
// 追記 (t88_append)
// ============================================================================
// DATA タグの中身をつなげたもの
static std::vector<uint8_t> data_bytes(const std::vector<T88Tag>& tags) {
    std::vector<uint8_t> out;
    for (const T88Tag& t : tags) {
        if (t.id == T88_TAG_DATA) {
            out.insert(out.end(), t.payload.begin(), t.payload.end());
        }
    }
    return out;
}

static std::vector<Half> play_t88(const char* path) {
    File f = SD.open(path, FILE_READ);
    PlayerT88 player;
    TEST_ASSERT_TRUE(player.init(&f, path));
    return run_playback(player);
}

void test_append_joins_tags() {
    std::vector<std::vector<uint8_t>> a = {random_bytes(100, 1)};
    std::vector<std::vector<uint8_t>> b = {random_bytes(200, 2), random_bytes(50, 3)};
    convert(make_signal(FSK_1200, a));
    SD.put("/A.T88", SD.get(T88_PATH));
    convert(make_signal(FSK_600, b));
    SD.put("/B.T88", SD.get(T88_PATH));

    TEST_ASSERT_TRUE(t88_append("/A.T88", "/B.T88", "/M.T88"));
    std::vector<T88Tag> tags = parse_t88(SD.get("/M.T88"));

    // VERSION は先頭の1つだけ、END は最後の1つだけ
    int versions = 0, ends = 0;
    for (const T88Tag& t : tags) {
        versions += (t.id == T88_TAG_VERSION);
        ends += (t.id == T88_TAG_END);
    }
    TEST_ASSERT_EQUAL_HEX16(T88_TAG_VERSION, tags.front().id);
    TEST_ASSERT_EQUAL_INT(1, versions);
    TEST_ASSERT_EQUAL_HEX16(T88_TAG_END, tags.back().id);
    TEST_ASSERT_EQUAL_INT(1, ends);

    // データは A → B の順、ボーレートはそれぞれのまま
    std::vector<uint8_t> expected = a[0];
    for (const auto& blk : b) {
        expected.insert(expected.end(), blk.begin(), blk.end());
    }
    assert_bytes_equal(expected, data_bytes(tags), "appended data");
    std::vector<uint16_t> types;
    for (const T88Tag& t : tags) {
        if (t.id == T88_TAG_DATA) {
            types.push_back(t.data_type);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(3, types.size());
    TEST_ASSERT_EQUAL_HEX16(0x01CC, types[0]);
    TEST_ASSERT_EQUAL_HEX16(0x00CC, types[1]);
    TEST_ASSERT_EQUAL_HEX16(0x00CC, types[2]);

    // 時刻は途切れず続く (境目は 2秒の BLANK)
    uint32_t end_ticks = 0;
    bool seen_join = false;
    for (const T88Tag& t : tags) {
        if (t.len == 0) {
            continue;
        }
        TEST_ASSERT_UINT32_WITHIN_MESSAGE(2, end_ticks, t.start, "tags are not contiguous");
        if (t.id == T88_TAG_BLANK && t.len == T88_APPEND_BLANK_TICKS) {
            seen_join = true;
        }
        end_ticks = t.start + t.len;
    }
    TEST_ASSERT_TRUE(seen_join);

    // つないだファイルを再生すると、後ろの 600ボーのデータも出る
    FskDecodeResult r600 = decode_fsk(play_t88("/M.T88"), FSK_600);
    std::vector<uint8_t> tail;
    for (const auto& blk : r600.blocks) {
        tail.insert(tail.end(), blk.begin(), blk.end());
    }
    std::vector<uint8_t> b_all = b[0];
    b_all.insert(b_all.end(), b[1].begin(), b[1].end());
    assert_bytes_end_with(b_all, tail, "600 part");
}

void test_convert_machine_code() {
    // モニタの W で保存したマシン語 (先頭が 0x3A)。600 / 1200 の判定とデータ
    std::vector<std::vector<uint8_t>> blocks = {pc8001_machine_code(0xC000, 600, 31)};
    check_data_tags(convert(make_signal(FSK_1200, blocks)), blocks, 0x01CC);
    check_data_tags(convert(make_signal(FSK_600, blocks)), blocks, 0x00CC);

    // 再生 → 録音 → 変換 で戻る
    std::vector<uint8_t> src = data_bytes(parse_t88(SD.get(T88_PATH)));
    write_raw(RAW_PATH, normalize_silence(play_t88(T88_PATH)));
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, "/RT.T88"));
    assert_bytes_equal(src, data_bytes(parse_t88(SD.get("/RT.T88"))), "T88 machine code");
}

void test_convert_n88_monitor_named() {
    // N88-BASIC のモニタの W名前: ヘッダ ("$$$" + 名前) と データの間のピー音は約70ms。
    // ここでも DATA タグを分ける (X88000 の記録と同じ形)
    std::vector<std::vector<uint8_t>> blocks = {n88_monitor_header("MTEST "),
                                                pc8001_machine_code(0xC000, 32, 5)};
    TapeSignal s(FSK_1200, 0.05, 3);
    s.leader_ms(1000);
    s.bytes(blocks[0]);
    s.leader_ms(68);
    s.bytes(blocks[1]);
    s.leader_ms(800);
    s.silence(500);
    check_data_tags(convert(s), blocks, 0x01CC);

    // 再生 → 録音 → 変換 でも分かれたまま
    write_raw(RAW_PATH, normalize_silence(play_t88(T88_PATH)));
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, "/RT.T88"));
    check_data_tags(parse_t88(SD.get("/RT.T88")), blocks, 0x01CC);
}

void test_append_rejects_non_t88() {
    convert(make_signal(FSK_1200, test_blocks()));
    SD.put("/A.T88", SD.get(T88_PATH));
    SD.put("/B.CMT", random_bytes(100, 9));
    TEST_ASSERT_FALSE(t88_append("/A.T88", "/B.CMT", "/M.T88"));
    TEST_ASSERT_FALSE(t88_append("/B.CMT", "/A.T88", "/M.T88"));
}

// ============================================================================
// 外から渡された T88 (エミュレータで作ったものなど) とのクロスチェック
//   python tools/crosscheck.py t88-roundtrip / t88-append。環境変数が無ければスキップ
// ============================================================================
// 再生 → 録音 → 変換 して、DATA タグの中身が元と同じか確かめる
static void roundtrip_t88(const char* in_path, const char* out_name) {
    std::vector<uint8_t> src = data_bytes(parse_t88(SD.get(in_path), false));
    write_raw(RAW_PATH, normalize_silence(play_t88(in_path)));
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, T88_PATH));
    xcheck_dump(out_name, SD.get(T88_PATH));
    assert_bytes_equal(src, data_bytes(parse_t88(SD.get(T88_PATH))), "T88 roundtrip");
}

//   XCHECK_IN_DIR/input.t88 → XCHECK_OUT_DIR/pico_roundtrip.t88
void test_xcheck_roundtrip_input_t88() {
    std::vector<uint8_t> in;
    if (!xcheck_input("input.t88", in)) {
        TEST_IGNORE_MESSAGE("no input.t88 (XCHECK_IN_DIR)");
    }
    SD.put("/IN.T88", in);
    roundtrip_t88("/IN.T88", "pico_roundtrip.t88");
}

//   XCHECK_IN_DIR/append_a.t88 + append_b.t88
//   → XCHECK_OUT_DIR/pico_append.t88 (つないだだけ) / pico_append_roundtrip.t88
void test_xcheck_append_input_t88() {
    std::vector<uint8_t> a, b;
    if (!xcheck_input("append_a.t88", a) || !xcheck_input("append_b.t88", b)) {
        TEST_IGNORE_MESSAGE("no append_a.t88 / append_b.t88 (XCHECK_IN_DIR)");
    }
    SD.put("/A.T88", a);
    SD.put("/B.T88", b);
    TEST_ASSERT_TRUE(t88_append("/A.T88", "/B.T88", "/M.T88"));
    xcheck_dump("pico_append.t88", SD.get("/M.T88"));

    std::vector<uint8_t> expected = data_bytes(parse_t88(a, false));
    std::vector<uint8_t> b_data = data_bytes(parse_t88(b, false));
    expected.insert(expected.end(), b_data.begin(), b_data.end());
    assert_bytes_equal(expected, data_bytes(parse_t88(SD.get("/M.T88"))), "appended");
    roundtrip_t88("/M.T88", "pico_append_roundtrip.t88");
}

void setUp() {
    SD.clear();
    mock_millis_value = 0;
    mock_sd_write_budget = SIZE_MAX;
}

void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_convert_structure_1200);
    RUN_TEST(test_convert_structure_600);
    RUN_TEST(test_convert_tag_times_follow_signal);
    RUN_TEST(test_convert_reports_write_error);
    RUN_TEST(test_player_outputs_data_bytes);
    RUN_TEST(test_player_pulse_timing);
    RUN_TEST(test_player_blank_duration);
    RUN_TEST(test_player_keeps_phase_after_blank);
    RUN_TEST(test_append_joins_tags);
    RUN_TEST(test_append_rejects_non_t88);
    RUN_TEST(test_convert_machine_code);
    RUN_TEST(test_convert_n88_monitor_named);
    RUN_TEST(test_xcheck_roundtrip_input_t88);
    RUN_TEST(test_xcheck_append_input_t88);
    return UNITY_END();
}
