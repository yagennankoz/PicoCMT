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

// ----------------------------------------------------------------------------
// 600/1200ボーの混在(1つのブロックの途中でボーレートを変えるローダーの形式)
// ----------------------------------------------------------------------------
// データブロックを1つ足す:
//   [Mark]600ボー: FF x4+名前6文字[Mark 48ms]
//   1200ボー: blkno lenH lenL addrH addrL、データ、チェックサム(全バイトのXORがFF)
//   データは256バイトごとに1バイトだけ600ボーで、その前後に約50msのMarkが入る
static void mixed_block(MixedTapeSignal& s, uint8_t blk, uint16_t addr, uint16_t len,
                        uint32_t seed) {
    s.mark_ms(600);
    s.bytes(600, {0xFF, 0xFF, 0xFF, 0xFF, 'M', 'I', 'X', 'E', 'D', '1'});
    s.mark_ms(48);
    std::vector<uint8_t> hdr = {blk, (uint8_t)(len >> 8), (uint8_t)len, (uint8_t)(addr >> 8),
                                (uint8_t)addr};
    uint8_t x = 0;
    for (uint8_t v : hdr) {
        s.byte(1200, v);
        x ^= v;
    }
    // 600ボーになる位置:ヘッダのXORから1ずつ引いて0になったバイト
    uint8_t iy = x;
    std::vector<uint8_t> data = random_bytes(len, seed);
    // 600ボーのバイトには、波形だけでは1200ボーの1〜2バイトと区別できない値も混ぜる
    static const uint8_t ambiguous[] = {0xE6, 0xFE, 0xE0, 0x98};
    int slow = 0;
    for (uint16_t k = 0; k < len; k++) {
        iy = (uint8_t)(iy - 1);
        uint8_t v = data[k];
        if (iy == 0) {
            if (slow % 2 == 0) {
                v = ambiguous[(slow / 2) % 4];
            }
            slow++;
            s.mark_ms(50);
            s.byte(600, v);
            s.mark_ms(50);
        } else {
            s.byte(1200, v);
        }
        x ^= v;
    }
    s.byte(1200, (uint8_t)(0xFF ^ x));
}

// ブロック0は、最後のデータバイトが600ボーで、チェックサムだけがMarkに挟まれる形
static MixedTapeSignal mixed_tape(double speed, double jitter, uint32_t seed) {
    MixedTapeSignal s(speed, jitter, seed);
    s.silence_ms(500);
    s.mark_ms(2000);
    mixed_block(s, 0, 0x0100, 0x0123, seed + 1);
    mixed_block(s, 1, 0x0200, 0x0400, seed + 2);
    mixed_block(s, 2, 0x0E00, 0x0C00, seed + 3);
    s.mark_ms(1000);
    s.silence_ms(500);
    return s;
}

// T88のDATAタグの中身を(バイト,ボーレート)の並びにする
static std::vector<std::pair<uint8_t, int>> data_sequence(const std::vector<T88Tag>& tags) {
    std::vector<std::pair<uint8_t, int>> out;
    for (const T88Tag& t : tags) {
        if (t.id != T88_TAG_DATA) {
            continue;
        }
        int baud = (t.data_type & 0x0100) ? 1200 : 600;
        for (uint8_t v : t.payload) {
            out.push_back({v, baud});
        }
    }
    return out;
}

static void assert_sequence_equal(const std::vector<std::pair<uint8_t, int>>& expected,
                                  const std::vector<std::pair<uint8_t, int>>& actual,
                                  const char* what) {
    size_t n = std::min(expected.size(), actual.size());
    for (size_t i = 0; i < n; i++) {
        if (expected[i] != actual[i]) {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s: index %u expected %02X/%d got %02X/%d", what,
                     (unsigned)i, expected[i].first, expected[i].second, actual[i].first,
                     actual[i].second);
            TEST_FAIL_MESSAGE(msg);
        }
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected.size(), actual.size(), what);
}

static void check_convert_mixed(double speed, double jitter, uint32_t seed) {
    MixedTapeSignal s = mixed_tape(speed, jitter, seed);
    write_raw(RAW_PATH, s.halves);
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, T88_PATH));
    assert_sequence_equal(s.byte_sequence(), data_sequence(parse_t88(SD.get(T88_PATH))),
                          "mixed baud");
}

void test_convert_mixed_baud() { check_convert_mixed(1.0, 0.0, 10); }
// テープが4%遅く、揺らぎもある(実際の録音で約4%遅いものがあった)
void test_convert_mixed_baud_slow_tape() { check_convert_mixed(0.96, 0.08, 20); }

// MixedTapeSignalの並びからT88を組み立てる(時間は1/4800秒)
static std::vector<uint8_t> build_t88(const MixedTapeSignal& s) {
    std::vector<uint8_t> d(T88_SIGNATURE, T88_SIGNATURE + 24);
    auto u16 = [&](uint32_t v) {
        d.push_back(v & 0xFF);
        d.push_back((v >> 8) & 0xFF);
    };
    auto u32 = [&](uint32_t v) {
        u16(v & 0xFFFF);
        u16(v >> 16);
    };
    u16(T88_TAG_VERSION);
    u16(2);
    u16(0x0100);
    uint32_t pos = 0;
    for (const auto& g : s.segs) {
        if (g.kind == MixedTapeSignal::Seg::DATA) {
            uint32_t len = (uint32_t)g.bytes.size() * (g.baud == 600 ? 88 : 44);
            u16(T88_TAG_DATA);
            u16(12 + g.bytes.size());
            u32(pos);
            u32(len);
            u16(g.bytes.size());
            u16((g.baud == 600 ? 0 : T88_TYPE_1200) |
                (g.even_parity ? T88_MODE_8E1 : T88_MODE_8N2));
            d.insert(d.end(), g.bytes.begin(), g.bytes.end());
            pos += len;
        } else {
            uint32_t len = (uint32_t)(g.ms * 4.8 + 0.5);
            u16(g.kind == MixedTapeSignal::Seg::MARK ? T88_TAG_MARK : T88_TAG_BLANK);
            u16(8);
            u32(pos);
            u32(len);
            pos += len;
        }
    }
    u16(T88_TAG_END);
    u16(0);
    return d;
}

// 波形を「短い/長い半周期の連続」の並びにする。長いMark(40半周期以上)と無音は長さを比べない
static std::vector<std::pair<int, int>> fsk_runs(const std::vector<Half>& w) {
    std::vector<std::pair<int, int>> runs;
    for (const Half& h : w) {
        if (h.us > 2000) {
            continue;
        }
        int c = (h.us >= 312) ? 1 : 0;
        if (!runs.empty() && runs.back().first == c) {
            runs.back().second++;
        } else {
            runs.push_back({c, 1});
        }
    }
    std::vector<std::pair<int, int>> out;
    for (auto r : runs) {
        if (r.first == 0 && r.second >= 40) {
            r.second = -1;
        }
        if (!out.empty() && r.second == -1 && out.back().second == -1) {
            continue;
        }
        out.push_back(r);
    }
    // 最初のデータの直前のMarkから比べる(先頭のリーダーの扱いの違いは見ない)
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i].first == 1) {
            out.erase(out.begin(), out.begin() + (i == 0 ? 0 : i - 1));
            break;
        }
    }
    return out;
}

void test_player_mixed_baud() {
    // 混在のT88を再生すると、DATAタグごとのボーレートで元と同じ波形になる
    MixedTapeSignal s = mixed_tape(1.0, 0.0, 30);
    SD.put(T88_PATH, build_t88(s));
    std::vector<Half> played = play_t88(T88_PATH);
    std::vector<std::pair<int, int>> expected = fsk_runs(s.halves);
    std::vector<std::pair<int, int>> actual = fsk_runs(played);
    size_t n = std::min(expected.size(), actual.size());
    for (size_t i = 0; i < n; i++) {
        if (expected[i] != actual[i]) {
            char msg[96];
            snprintf(msg, sizeof(msg), "played waveform differs at run %u", (unsigned)i);
            TEST_FAIL_MESSAGE(msg);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(expected.size(), actual.size());

    // 再生→録音→変換で元の並びに戻る
    write_raw(RAW_PATH, normalize_silence(played));
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, "/RT.T88"));
    assert_sequence_equal(s.byte_sequence(), data_sequence(parse_t88(SD.get("/RT.T88"))),
                          "mixed baud roundtrip");
}

// パリティ付き(8bit・偶数パリティ・ストップビット1)のデータが混ざったテープ。
// 標準の形のブロックの後、600ボーの同期信号と1200ボーの本体をパリティ付きで、
// 間を約50msのMarkで区切る
static MixedTapeSignal parity_tape(uint32_t seed) {
    MixedTapeSignal s(1.0, 0.0, seed);
    s.silence_ms(500);
    s.mark_ms(2000);
    s.bytes(1200, random_bytes(200, seed));
    s.mark_ms(1000);
    for (int i = 0; i < 3; i++) {
        s.bytes(600, {0xFF, 0xFF, 0xFF, 0xFF, 'P', 'A', 'R', 'I', 'T', 'Y'}, true);
        s.mark_ms(50);
        s.bytes(1200, random_bytes(255, seed + 1 + i), true);
        s.mark_ms(50);
        s.byte(600, (uint8_t)(0x00 + i), true);  // 9ビット目(パリティ)が0の1バイト
        s.mark_ms(50);
    }
    s.mark_ms(1000);
    s.silence_ms(500);
    return s;
}

// DATAタグの型の並び(ボーレート+形)
static std::vector<uint16_t> data_types(const std::vector<T88Tag>& tags) {
    std::vector<uint16_t> out;
    for (const T88Tag& t : tags) {
        if (t.id == T88_TAG_DATA) {
            out.push_back(t.data_type);
        }
    }
    return out;
}

// パリティ付きでも、パリティビットがすべて1なら波形はパリティ無しと同じなので標準の型になる
static std::vector<uint16_t> expected_types(const MixedTapeSignal& s) {
    std::vector<uint16_t> out;
    for (const auto& g : s.segs) {
        if (g.kind == MixedTapeSignal::Seg::DATA) {
            bool has_zero_bit9 = false;
            for (uint8_t v : g.bytes) {
                has_zero_bit9 |= (__builtin_popcount(v) & 1) == 0;
            }
            out.push_back((g.baud == 600 ? 0 : T88_TYPE_1200) |
                          (g.even_parity && has_zero_bit9 ? T88_MODE_8E1 : T88_MODE_8N2));
        }
    }
    return out;
}

static void assert_types_equal(const std::vector<uint16_t>& expected,
                               const std::vector<uint16_t>& actual, const char* what) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected.size(), actual.size(), what);
    for (size_t i = 0; i < expected.size(); i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: DATA %u type", what, (unsigned)i);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(expected[i], actual[i], msg);
    }
}

void test_convert_even_parity() {
    // パリティ付きのデータを、600ボーの同期信号も含めて正しいボーレートで読み、
    // DATAタグの型にパリティ付き(x17C/x07C)を書く
    MixedTapeSignal s = parity_tape(50);
    write_raw(RAW_PATH, s.halves);
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, T88_PATH));
    std::vector<T88Tag> tags = parse_t88(SD.get(T88_PATH));
    assert_sequence_equal(s.byte_sequence(), data_sequence(tags), "even parity");
    assert_types_equal(expected_types(s), data_types(tags), "even parity");
}

void test_player_even_parity() {
    // 型がパリティ付きのDATAは、パリティビットとストップビット1つで再生し、元と同じ波形になる
    MixedTapeSignal s = parity_tape(60);
    SD.put(T88_PATH, build_t88(s));
    std::vector<Half> played = play_t88(T88_PATH);
    std::vector<std::pair<int, int>> expected = fsk_runs(s.halves);
    std::vector<std::pair<int, int>> actual = fsk_runs(played);
    size_t n = std::min(expected.size(), actual.size());
    for (size_t i = 0; i < n; i++) {
        if (expected[i] != actual[i]) {
            char msg[96];
            snprintf(msg, sizeof(msg), "played waveform differs at run %u", (unsigned)i);
            TEST_FAIL_MESSAGE(msg);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(expected.size(), actual.size());

    // 再生→録音→変換で元のバイトと型に戻る
    write_raw(RAW_PATH, normalize_silence(played));
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, "/RT.T88"));
    std::vector<T88Tag> tags = parse_t88(SD.get("/RT.T88"));
    assert_sequence_equal(s.byte_sequence(), data_sequence(tags), "even parity roundtrip");
    assert_types_equal(expected_types(s), data_types(tags), "even parity roundtrip");
}

void test_t88_frame_from_type() {
    T88Frame f = t88_frame_from_type(0x01CC);
    TEST_ASSERT_EQUAL_INT(8, f.data_bits);
    TEST_ASSERT_EQUAL_INT(0, f.parity);
    TEST_ASSERT_EQUAL_INT(4, f.stop_half_bits);
    f = t88_frame_from_type(0x007C);
    TEST_ASSERT_EQUAL_INT(8, f.data_bits);
    TEST_ASSERT_EQUAL_INT(2, f.parity);
    TEST_ASSERT_EQUAL_INT(2, f.stop_half_bits);
    f = t88_frame_from_type(0x015C);
    TEST_ASSERT_EQUAL_INT(1, f.parity);
    f = t88_frame_from_type(0x0188);  // 7bit・パリティ無し・ストップビット1.5
    TEST_ASSERT_EQUAL_INT(7, f.data_bits);
    TEST_ASSERT_EQUAL_INT(3, f.stop_half_bits);
    // 下位バイトが0のもの、ストップビット長が00のものは標準の形
    f = t88_frame_from_type(0x0100);
    TEST_ASSERT_EQUAL_INT(8, f.data_bits);
    TEST_ASSERT_EQUAL_INT(0, f.parity);
    TEST_ASSERT_EQUAL_INT(4, f.stop_half_bits);
    f = t88_frame_from_type(0x003C);
    TEST_ASSERT_EQUAL_INT(0, f.parity);
    TEST_ASSERT_EQUAL_INT(4, f.stop_half_bits);
}

void test_convert_drops_stretched_noise_after_trailer() {
    // トレーラー(長いMark)の終わりでモーターが止まりかけ、音が低くなった所がデータに見える。
    // バイト数のわりに間延びした短いDATAは雑音としてMARKにする
    std::vector<uint8_t> data = random_bytes(100, 41);
    TapeSignal s(FSK_1200, 0.0, 3);
    s.leader_ms(1000);
    s.bytes(data);
    s.leader_ms(1600);
    // 2バイトで約48ms (見込みの約18msの2倍を超える)
    s.bytes({0x00});
    s.leader_ms(30);
    s.bytes({0xFE});
    s.leader_ms(100);
    s.silence(500);
    write_raw(RAW_PATH, s.halves);
    TEST_ASSERT_TRUE(convert_raw_to_t88(RAW_PATH, T88_PATH));
    assert_bytes_equal(data, data_bytes(parse_t88(SD.get(T88_PATH))), "data only");
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
    RUN_TEST(test_convert_mixed_baud);
    RUN_TEST(test_convert_mixed_baud_slow_tape);
    RUN_TEST(test_player_mixed_baud);
    RUN_TEST(test_convert_even_parity);
    RUN_TEST(test_player_even_parity);
    RUN_TEST(test_t88_frame_from_type);
    RUN_TEST(test_convert_drops_stretched_noise_after_trailer);
    return UNITY_END();
}
