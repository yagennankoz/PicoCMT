// ============================================================================
// FskDemodulator (CMT/T88/T77 の変換で共通) の単体テスト (pio test -e native)
// ============================================================================
#include <unity.h>

#include "cmt_fsk_demodulator.h"
#include "tape_test_utils.h"

volatile uint32_t playback_base_time_ms = 0;
volatile uint32_t playback_tag_start_system_ms = 0;
volatile bool playback_time_valid = false;
void draw_conversion_progress(int) {}

static std::vector<uint8_t> demodulate(const std::vector<Half>& sig, FskDemodulator& demod) {
    std::vector<uint8_t> out;
    demod.on_byte_decoded = [&](uint8_t b) { out.push_back(b); };
    for (const Half& h : sig) {
        demod.feed_pulse(h.high, (uint32_t)lround(h.us));
    }
    return out;
}

static void check_decode(const FskFormat& f, double jitter) {
    std::vector<uint8_t> data = random_bytes(500, 4);
    TapeSignal s(f, jitter, 11);
    s.leader_ms(500);
    s.bytes(data);
    s.leader_ms(100);
    FskDemodulator demod;
    assert_bytes_equal(data, demodulate(s.halves, demod), "decoded");
}

static void check_decode_first_byte(const FskFormat& f, uint8_t first) {
    std::vector<uint8_t> data = random_bytes(100, 4);
    data[0] = first;
    TapeSignal s(f, 0.0, 11);
    s.leader_ms(500);
    s.bytes(data);
    s.leader_ms(100);
    FskDemodulator demod;
    assert_bytes_equal(data, demodulate(s.halves, demod), "decoded");
}

void test_decode_1200() { check_decode(FSK_1200, 0.0); }
void test_decode_600() { check_decode(FSK_600, 0.0); }
void test_decode_1200_with_jitter() { check_decode(FSK_1200, 0.08); }

// 600ボーのボーレート判定が先頭バイトに左右されないこと
// (0x3A = マシン語のヘッダ ':'、0xD3 = BASIC のヘッダ)
void test_decode_600_first_byte_3a() { check_decode_first_byte(FSK_600, 0x3A); }
void test_decode_600_first_byte_d3() { check_decode_first_byte(FSK_600, 0xD3); }
void test_decode_1200_first_byte_3a() { check_decode_first_byte(FSK_1200, 0x3A); }

void test_decode_short_600_block_before_gap() {
    // 判定用の半周期がたまる前に無音で終わる短いブロックも取りこぼさない
    std::vector<uint8_t> data = {0x3A};  // 約68半周期 (判定に必要な128半周期より少ない)
    TapeSignal s(FSK_600);
    s.leader_ms(300);
    s.bytes(data);
    FskDemodulator demod;
    std::vector<uint8_t> out;
    demod.on_byte_decoded = [&](uint8_t b) { out.push_back(b); };
    for (const Half& h : s.halves) {
        demod.feed_pulse(h.high, (uint32_t)lround(h.us));
    }
    demod.advance_gap(100000);
    assert_bytes_equal(data, out, "decoded");
}

void test_time_base_includes_long_pulses() {
    // 2ms を超えるパルス (ドロップアウト) も経過時間に含めること
    TapeSignal s(FSK_1200);
    s.leader_ms(100);
    s.pulse(5000);
    s.leader_ms(100);
    FskDemodulator demod;
    demodulate(s.halves, demod);

    uint64_t total = 0;
    for (const Half& h : s.halves) {
        total += (uint64_t)lround(h.us);
    }
    TEST_ASSERT_EQUAL_UINT64(total, demod.current_time_us);
}

// Markが続いたまま(無音を挟まずに)ボーレートが変わっても、長いMarkの後で決め直す
void test_decode_baud_change_within_carrier() {
    MixedTapeSignal s;
    s.mark_ms(500);
    s.bytes(1200, random_bytes(50, 5));
    s.mark_ms(50);
    s.bytes(600, random_bytes(20, 6));
    s.mark_ms(50);
    s.bytes(1200, random_bytes(50, 7));
    s.mark_ms(100);
    FskDemodulator demod;
    std::vector<uint8_t> expected;
    for (auto& e : s.byte_sequence()) {
        expected.push_back(e.first);
    }
    assert_bytes_equal(expected, demodulate(s.halves, demod), "decoded");
}

// Markに挟まれた600ボーの1バイト。1200ボーの2バイトとしても形が崩れない値(0xFFなど)も600ボーとする
void test_decode_single_600_byte_between_marks() {
    const uint8_t values[] = {0xFF, 0xE6, 0xFE, 0xE0, 0x98, 0x00, 0x3A};
    for (uint8_t v : values) {
        MixedTapeSignal s;
        s.mark_ms(500);
        s.bytes(1200, {0x12, 0x34});
        s.mark_ms(50);
        s.byte(600, v);
        s.mark_ms(50);
        s.bytes(1200, {0x56, 0x78});
        s.mark_ms(100);
        FskDemodulator demod;
        std::vector<int> bauds;
        std::vector<uint8_t> out;
        demod.on_byte_decoded = [&](uint8_t b) {
            out.push_back(b);
            bauds.push_back(demod.baud_rate == FskDemodulator::BAUD_600 ? 600 : 1200);
        };
        for (const Half& h : s.halves) {
            demod.feed_pulse(h.high, (uint32_t)lround(h.us));
        }
        char msg[32];
        snprintf(msg, sizeof(msg), "value %02X", v);
        assert_bytes_equal({0x12, 0x34, v, 0x56, 0x78}, out, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(600, bauds[2], msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1200, bauds[3], msg);
    }
}

// 600ボーのブロックの頭が雑音で崩れ、1200ボーにしか出ない並び(Space 2つ・6つ)が出ても、
// 600ボーと読む(実際の録音で、雑音の後の長いブロックを1200ボーと読んでいた)
void test_decode_600_after_noisy_start() {
    std::vector<uint8_t> data = random_bytes(40, 8);
    TapeSignal s(FSK_600, 0.0, 11);
    s.leader_ms(500);
    s.bytes({data[0], data[1]});
    // 実際の録音の崩れた所の半周期(us)
    const double noise[] = {387, 165, 162, 159, 378, 165, 162, 168, 430, 430, 430, 430,
                            430, 430, 208, 208, 208, 208, 208, 208, 208, 208, 360, 168,
                            168, 168, 418, 419};
    for (double us : noise) {
        s.pulse(us);
    }
    s.bytes(std::vector<uint8_t>(data.begin() + 2, data.end()));
    s.leader_ms(100);
    FskDemodulator demod;
    std::vector<uint8_t> out = demodulate(s.halves, demod);
    TEST_ASSERT_EQUAL_INT(FskDemodulator::BAUD_600, demod.baud_rate);
    // 雑音の所のバイトは崩れてよい。その後のデータが読めていること
    TEST_ASSERT_TRUE(out.size() >= 30);
    std::vector<uint8_t> tail(out.end() - 30, out.end());
    assert_bytes_equal(std::vector<uint8_t>(data.end() - 30, data.end()), tail, "after noise");
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_decode_1200);
    RUN_TEST(test_decode_600);
    RUN_TEST(test_decode_1200_with_jitter);
    RUN_TEST(test_decode_600_first_byte_3a);
    RUN_TEST(test_decode_600_first_byte_d3);
    RUN_TEST(test_decode_1200_first_byte_3a);
    RUN_TEST(test_decode_short_600_block_before_gap);
    RUN_TEST(test_time_base_includes_long_pulses);
    RUN_TEST(test_decode_baud_change_within_carrier);
    RUN_TEST(test_decode_single_600_byte_between_marks);
    RUN_TEST(test_decode_600_after_noisy_start);
    return UNITY_END();
}
