#include "cmt_t77_converter.h"

#include <Arduino.h>
#include <SD.h>

#include "cmt_fm7_demodulator.h"
#include "cmt_globals.h"
#include "cmt_sd_buffer.h"
#include "cmt_t77_format.h"
#include "cmt_ui.h"
#include "hardware/watchdog.h"

// 録音PIO(cmt_rec.pio)が計測値に含めないサイクル分の補正 (0.5us単位)
//   High: mov isr / push / mov x の3サイクル = 1.5us
//   Low : mov isr / push / mov x / エッジ検出時の jmp pin の4サイクル = 2.0us
static constexpr uint32_t REC_PIO_OVERHEAD_HIGH_HALF_US = 3;
static constexpr uint32_t REC_PIO_OVERHEAD_LOW_HALF_US = 4;

namespace {

// レベルと時間を受け取り、T77のワード列として書き出す
// 累積時間から各パルスのカウント数を決めるので、丸め誤差が積み上がらない
class T77Writer {
   public:
    explicit T77Writer(SdBufferedWrite* w) : writer(w) {}

    bool put(bool high, uint64_t half_us) {
        elapsed_half_us += half_us;
        uint64_t target = (elapsed_half_us + T77_TICK_US) / (2 * T77_TICK_US);
        if (target <= written_ticks) {
            target = written_ticks + 1;  // 最低1カウント (差分は次のパルスで吸収)
        }
        uint64_t ticks = target - written_ticks;
        written_ticks = target;

        while (ticks > 0) {
            uint16_t chunk =
                (ticks > T77_COUNT_MASK) ? T77_COUNT_MASK : (uint16_t)ticks;
            uint16_t out_val = chunk | (high ? T77_LEVEL_BIT : 0);
            uint8_t buf[2] = {(uint8_t)(out_val >> 8), (uint8_t)(out_val & 0xFF)};
            if (!writer->write_bytes(buf, 2)) {
                return false;
            }
            ticks -= chunk;
        }
        return true;
    }

   private:
    SdBufferedWrite* writer;
    uint64_t elapsed_half_us = 0;
    uint64_t written_ticks = 0;
};

// 復調したバイト列から FM-7 のヘッダブロックを探し、ファイル名を拾う
//   01 3C [種別=00(ヘッダ)] [長さ] [ファイル名 8バイト ...] [チェックサム]
//   チェックサム = 種別 + 長さ + データ の下位8bit。合ったブロックだけ使う
class T77HeaderNameFinder {
   public:
    explicit T77HeaderNameFinder(T77HeaderNames* out) : names(out) {
        names->count = 0;
    }

    void reset() { state = 0; }

    void on_byte(uint8_t b) {
        switch (state) {
            case 0:  // 同期 1バイト目
                state = (b == 0x01) ? 1 : 0;
                break;
            case 1:  // 同期 2バイト目
                state = (b == 0x3C) ? 2 : (b == 0x01 ? 1 : 0);
                break;
            case 2:  // ブロック種別 (ヘッダのみ対象)
                state = (b == 0x00) ? 3 : (b == 0x01 ? 1 : 0);
                break;
            case 3:  // ブロック長
                len = b;
                sum = b;  // 種別(00) + 長さ
                pos = 0;
                state = (len >= T77_NAME_LEN) ? 4 : 0;
                break;
            case 4:  // データ (先頭8バイトがファイル名)
                if (pos < T77_NAME_LEN) {
                    buf[pos] = (char)b;
                }
                sum += b;
                if (++pos >= len) {
                    state = 5;
                }
                break;
            case 5:  // チェックサム
                if (b == sum && names->count < T77_MAX_HEADER_NAMES) {
                    memcpy(names->name[names->count++], buf, T77_NAME_LEN);
                }
                state = 0;
                break;
        }
    }

   private:
    T77HeaderNames* names;
    int state = 0;
    int pos = 0;
    int len = 0;
    uint8_t sum = 0;
    char buf[T77_NAME_LEN];
};

}  // namespace

static T77HeaderNames last_header_names = {};

const T77HeaderNames& t77_last_header_names() { return last_header_names; }

bool convert_raw_to_t77(const char* raw_path, const char* out_path) {
    last_header_names.count = 0;

    if (raw_path == nullptr || out_path == nullptr) {
        return false;
    }

    File raw_file = SD.open(raw_path, FILE_READ);
    if (!raw_file) {
        return false;
    }

    if (SD.exists(out_path)) {
        SD.remove(out_path);
    }
    File t77_file = SD.open(out_path, FILE_WRITE);
    if (!t77_file) {
        raw_file.close();
        return false;
    }

    uint32_t total_size = raw_file.size();
    int last_percent = -1;

    SdBufferedRead* reader = new SdBufferedRead(&raw_file);
    SdBufferedWrite* writer = new SdBufferedWrite(&t77_file);

    auto cleanup = [&]() {
        delete writer;
        delete reader;
        raw_file.close();
        t77_file.close();
    };

    // T77の固定ヘッダ (16バイト) と、XM7 と同じくその直後の 0x0000
    static const uint8_t zero_word[2] = {0x00, 0x00};
    if (!writer->write_bytes((const uint8_t*)T77_HEADER_STR, T77_HEADER_LEN) ||
        !writer->write_bytes(zero_word, sizeof(zero_word))) {
        cleanup();
        return false;
    }

    T77Writer t77(writer);

    // ファイル名抽出用に、同じパルスを復調器にも通す
    T77HeaderNameFinder name_finder(&last_header_names);
    Fm7Demodulator demod;
    demod.on_byte_decoded = [&](uint8_t byte_val) { name_finder.on_byte(byte_val); };

    uint16_t raw_word = 0;
    uint32_t loop_count = 0;
    bool last_high = false;
    bool ok = true;

    // バッファ経由で高速読み込み
    while (ok && reader->read_word(raw_word)) {
        loop_count++;

        if ((loop_count & 0x0FFF) == 0) {
            watchdog_update();
            if (total_size > 0) {
                int percent = ((loop_count * 2) * 100) / total_size;
                if (percent > 100) {
                    percent = 100;
                }
                if (percent != last_percent) {
                    writer->flush();
                    draw_conversion_progress(percent);
                    last_percent = percent;
                }
            }
        }

        // --- 無音ギャップの処理 ---
        if (raw_word == 0xFFFF) {
            uint16_t gap_ms;
            if (!reader->read_word(gap_ms)) {
                break;
            }
            loop_count++;

            // RAWには無音中のレベルが残っていないが、録音PIOはH/Lを交互に
            // 計測するので、直前のパルスと逆のレベルになる
            last_high = !last_high;
            ok = t77.put(last_high, (uint64_t)gap_ms * 2000ULL);

            demod.reset();
            name_finder.reset();
            continue;
        }

        // --- 通常のパルス処理 ---
        bool is_high = (raw_word & 0x8000) != 0;
        uint32_t us = pio_ticks_to_us(raw_word & 0x7FFF);
        uint64_t half_us = (uint64_t)us * 2 + (is_high ? REC_PIO_OVERHEAD_HIGH_HALF_US
                                                       : REC_PIO_OVERHEAD_LOW_HALF_US);
        ok = t77.put(is_high, half_us);
        last_high = is_high;

        demod.feed_half((uint32_t)(half_us / 2));
    }

    if (ok) {
        ok = writer->flush();
    }
    cleanup();

    delay(50);
    draw_conversion_progress(100);
    return ok;
}
