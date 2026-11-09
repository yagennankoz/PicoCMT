// ============================================================================
// 波形をそのまま再生するプレーヤー(RAW・WAV)のテスト(pio test -e native)
// ============================================================================
#include <unity.h>

#include "cmt_t88_converter.h"
#include "cmt_wav_converter.h"
#include "cmt_wav_reader.h"
#include "crosscheck_utils.h"
#include "player_wave.h"
#include "tape_test_utils.h"

volatile uint32_t playback_base_time_ms = 0;
volatile uint32_t playback_tag_start_system_ms = 0;
volatile bool playback_time_valid = false;
void draw_conversion_progress(int) {}

static const std::vector<uint8_t> BLOCK_A = random_bytes(300, 1);
static const std::vector<uint8_t> BLOCK_B = random_bytes(200, 2);

// 1200ボーのテープ:リーダー、データ、無音、リーダー、データ
static TapeSignal make_signal(double jitter, double gap_ms = 800) {
    TapeSignal s(FSK_1200, jitter, 5);
    s.leader_ms(1000);
    s.bytes(BLOCK_A);
    s.leader_ms(200);
    s.silence(gap_ms);
    s.leader_ms(500);
    s.bytes(BLOCK_B);
    s.leader_ms(200);
    s.silence(300);
    return s;
}

static void assert_blocks(const std::vector<Half>& wave, const char* what) {
    FskDecodeResult r = decode_fsk(wave, FSK_1200);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)r.blocks.size(), what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.framing_errors, what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(BLOCK_A.size(), r.blocks[0].size(), what);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(BLOCK_A.data(), r.blocks[0].data(), BLOCK_A.size(), what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(BLOCK_B.size(), r.blocks[1].size(), what);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(BLOCK_B.data(), r.blocks[1].data(), BLOCK_B.size(), what);
}

// 半周期の平均の長さが元(208.3us/416.7us)とほぼ同じ
static void assert_half_lengths(const std::vector<Half>& wave, double tol_us) {
    HalfStats st = measure_halves(wave, FSK_1200);
    TEST_ASSERT_DOUBLE_WITHIN(tol_us, FSK_1200.short_half_us, st.mean_short);
    TEST_ASSERT_DOUBLE_WITHIN(tol_us, FSK_1200.long_us(), st.mean_long);
}

static void assert_t88_data(const char* path);

static std::vector<Half> play(CmtPlayer& player, const char* path) {
    File f = SD.open(path, FILE_READ);
    TEST_ASSERT_TRUE(player.init(&f, path));
    return run_playback(player);
}

static double total_us(const std::vector<Half>& w) {
    double t = 0;
    for (const Half& h : w) {
        t += h.us;
    }
    return t;
}

// ----------------------------------------------------------------------------
// RAW
// ----------------------------------------------------------------------------
void test_raw_playback() {
    // 録音したRAWをそのまま再生すると、元のテープと同じ波形(バイト・半周期の長さ)になる
    TapeSignal s = make_signal(0.05);
    write_raw("/A.RAW", s.halves);
    PlayerRaw player;
    std::vector<Half> played = play(player, "/A.RAW");
    assert_blocks(played, "RAW");
    assert_half_lengths(played, 2.0);
}

void test_raw_playback_keeps_silence_length() {
    // 無音(0xFFFF+ミリ秒)は同じ長さで出す
    TapeSignal s = make_signal(0.0, 3000);
    write_raw("/A.RAW", s.halves);
    PlayerRaw player;
    std::vector<Half> played = play(player, "/A.RAW");
    assert_blocks(played, "RAW long silence");
    // 録音の最初(トリガまで)の分を除いて、全体の長さがほぼ同じ
    TEST_ASSERT_DOUBLE_WITHIN(60000, total_us(s.halves), total_us(played));
}

// ----------------------------------------------------------------------------
// WAV
// ----------------------------------------------------------------------------
void test_wav_playback_8bit_mono() {
    TapeSignal s = make_signal(0.05);
    SD.put("/A.WAV", wave_to_wav(s.halves));
    PlayerWav player;
    std::vector<Half> played = play(player, "/A.WAV");
    assert_blocks(played, "WAV 8bit");
    assert_half_lengths(played, 3.0);
    // 全体の長さも元とほぼ同じ(時間の誤差が積み上がらない)
    TEST_ASSERT_DOUBLE_WITHIN(total_us(s.halves) * 0.002, total_us(s.halves), total_us(played));
}

// 8bitモノラルのWAVを、直流分・雑音のある16bitステレオ(先頭のチャンネルが信号)にする
static std::vector<uint8_t> to_16bit_stereo(const std::vector<uint8_t>& wav8, uint32_t rate) {
    WavFormat f;
    auto read_at = [&](uint32_t pos, uint8_t* buf, uint32_t len) -> int {
        if (pos + len > wav8.size()) {
            return -1;
        }
        memcpy(buf, wav8.data() + pos, len);
        return (int)len;
    };
    TEST_ASSERT_TRUE(wav_parse_header(read_at, (uint32_t)wav8.size(), &f));
    std::mt19937 rng(9);
    std::uniform_int_distribution<int> noise(-300, 300);
    std::vector<uint8_t> pcm;
    auto s16 = [&](int v) {
        v = std::max(-32768, std::min(32767, v));
        pcm.push_back((uint8_t)(v & 0xFF));
        pcm.push_back((uint8_t)((v >> 8) & 0xFF));
    };
    for (uint32_t i = 0; i < f.data_size; i++) {
        int v = ((int)wav8[f.data_offset + i] - 128) * 60 + 4000 + noise(rng);
        s16(v);
        s16(noise(rng));
    }
    std::vector<uint8_t> d;
    auto str = [&](const char* c) { d.insert(d.end(), c, c + 4); };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) {
            d.push_back((uint8_t)(v >> (8 * i)));
        }
    };
    auto u16 = [&](uint16_t v) {
        d.push_back((uint8_t)v);
        d.push_back((uint8_t)(v >> 8));
    };
    str("RIFF");
    u32(4 + 24 + 12 + 8 + (uint32_t)pcm.size());
    str("WAVE");
    str("fmt ");
    u32(16);
    u16(1);
    u16(2);
    u32(rate);
    u32(rate * 4);
    u16(4);
    u16(16);
    // 知らないチャンク(LISTなど)は読み飛ばす
    str("LIST");
    u32(4);
    str("INFO");
    str("data");
    u32((uint32_t)pcm.size());
    d.insert(d.end(), pcm.begin(), pcm.end());
    return d;
}

void test_wav_playback_16bit_stereo_with_dc_offset() {
    TapeSignal s = make_signal(0.05);
    SD.put("/B.WAV", to_16bit_stereo(wave_to_wav(s.halves, 48000), 48000));
    PlayerWav player;
    std::vector<Half> played = play(player, "/B.WAV");
    assert_blocks(played, "WAV 16bit stereo");
    assert_half_lengths(played, 3.0);
}

void test_wav_playback_long_silence() {
    // 長い無音(10秒)でも止まらずに最後まで再生し、長さもほぼ同じ
    TapeSignal s = make_signal(0.0, 10000);
    SD.put("/A.WAV", wave_to_wav(s.halves));
    PlayerWav player;
    std::vector<Half> played = play(player, "/A.WAV");
    assert_blocks(played, "WAV long silence");
    TEST_ASSERT_DOUBLE_WITHIN(total_us(s.halves) * 0.002, total_us(s.halves), total_us(played));
}

void test_wav_rejects_non_pcm() {
    TapeSignal s = make_signal(0.0);
    std::vector<uint8_t> wav = wave_to_wav(s.halves);
    wav[20] = 3;  // 浮動小数点
    SD.put("/F.WAV", wav);
    File f = SD.open("/F.WAV", FILE_READ);
    PlayerWav player;
    TEST_ASSERT_FALSE(player.init(&f, "/F.WAV"));
}

void test_wav_playback_then_record_to_t88() {
    // WAVを再生した波形を録音してT88にすると、元のデータに戻る
    TapeSignal s = make_signal(0.05);
    SD.put("/A.WAV", wave_to_wav(s.halves));
    PlayerWav player;
    std::vector<Half> played = play(player, "/A.WAV");
    write_raw("/RAW_TEMP.BIN", normalize_silence(played));
    TEST_ASSERT_TRUE(convert_raw_to_t88("/RAW_TEMP.BIN", "/OUT.T88"));
    assert_t88_data("/OUT.T88");
}

// ----------------------------------------------------------------------------
// WAVからの変換
// ----------------------------------------------------------------------------
static void check_wav_to_t88(const std::vector<uint8_t>& wav) {
    SD.put("/A.WAV", wav);
    TEST_ASSERT_TRUE(convert_wav_to_raw("/A.WAV", "/W.RAW"));
    TEST_ASSERT_TRUE(convert_raw_to_t88("/W.RAW", "/OUT.T88"));
    assert_t88_data("/OUT.T88");
}

void test_wav_to_raw_to_t88() {
    // WAVをRAWにしてからT88に変換すると、元のデータに戻る
    check_wav_to_t88(wave_to_wav(make_signal(0.05).halves));
}

void test_wav_to_raw_16bit_stereo() {
    check_wav_to_t88(to_16bit_stereo(wave_to_wav(make_signal(0.05).halves, 48000), 48000));
}

void test_wav_to_raw_matches_recording() {
    // WAVから作ったRAWを再生すると、元のテープと同じ波形になる(録音のRAWと同じ扱い)
    TapeSignal s = make_signal(0.05, 3000);
    SD.put("/A.WAV", wave_to_wav(s.halves));
    TEST_ASSERT_TRUE(convert_wav_to_raw("/A.WAV", "/W.RAW"));
    PlayerRaw player;
    std::vector<Half> played = play(player, "/W.RAW");
    assert_blocks(played, "WAV->RAW");
    assert_half_lengths(played, 3.0);
}

void test_wav_to_raw_rejects_non_pcm() {
    std::vector<uint8_t> wav = wave_to_wav(make_signal(0.0).halves);
    wav[20] = 3;  // 浮動小数点
    SD.put("/F.WAV", wav);
    TEST_ASSERT_FALSE(convert_wav_to_raw("/F.WAV", "/W.RAW"));
    TEST_ASSERT_FALSE(SD.exists("/W.RAW"));
}

// T88のDATAの中身が元のデータ(BLOCK_A+BLOCK_B)と同じ
static void assert_t88_data(const char* path) {
    std::vector<uint8_t> t88 = SD.get(path);
    std::vector<uint8_t> data;
    for (size_t p = 24; p + 4 <= t88.size();) {
        uint16_t id = (uint16_t)(t88[p] | (t88[p + 1] << 8));
        uint16_t len = (uint16_t)(t88[p + 2] | (t88[p + 3] << 8));
        if (id == T88_TAG_DATA) {
            data.insert(data.end(), t88.begin() + p + 4 + 12, t88.begin() + p + 4 + len);
        }
        if (id == T88_TAG_END) {
            break;
        }
        p += 4 + len;
    }
    std::vector<uint8_t> expected = BLOCK_A;
    expected.insert(expected.end(), BLOCK_B.begin(), BLOCK_B.end());
    TEST_ASSERT_EQUAL_UINT32(expected.size(), data.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), data.data(), expected.size());
}

void setUp() {
    SD.clear();
    mock_millis_value = 0;
    mock_sd_write_budget = SIZE_MAX;
}

void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_raw_playback);
    RUN_TEST(test_raw_playback_keeps_silence_length);
    RUN_TEST(test_wav_playback_8bit_mono);
    RUN_TEST(test_wav_playback_16bit_stereo_with_dc_offset);
    RUN_TEST(test_wav_playback_long_silence);
    RUN_TEST(test_wav_rejects_non_pcm);
    RUN_TEST(test_wav_playback_then_record_to_t88);
    RUN_TEST(test_wav_to_raw_to_t88);
    RUN_TEST(test_wav_to_raw_16bit_stereo);
    RUN_TEST(test_wav_to_raw_matches_recording);
    RUN_TEST(test_wav_to_raw_rejects_non_pcm);
    return UNITY_END();
}
