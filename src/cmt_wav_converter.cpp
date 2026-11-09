#include "cmt_wav_converter.h"

#include <Arduino.h>
#include <SD.h>

#include "cmt_globals.h"
#include "cmt_sd_buffer.h"
#include "cmt_ui.h"
#include "cmt_wav_reader.h"
#include "hardware/watchdog.h"

// 半周期をRAWのワードにして書く
class RawWordWriter {
   public:
    explicit RawWordWriter(SdBufferedWrite& w) : writer_(w) {}

    bool half(bool level, uint64_t us) {
        if (us >= REC_SILENCE_GAP_TICKS) {
            // 録音と同じく、10ms以上は無音として長さ(ms)だけ書く
            uint64_t ms = us / 1000;
            while (ms > 0) {
                uint16_t out_ms = (ms > 65534) ? 65534 : (uint16_t)ms;
                if (!writer_.write_u16(0xFFFF) || !writer_.write_u16(out_ms)) {
                    return false;
                }
                ms -= out_ms;
            }
            return true;
        }
        // 録音PIOの計測値は実際よりREC_PIO_OVERHEAD_US短い(変換で足し戻す)
        uint32_t ticks = (us > REC_PIO_OVERHEAD_US) ? (uint32_t)(us - REC_PIO_OVERHEAD_US) : 1;
        uint16_t w = (ticks >= 0x7FFF) ? 0x7FFE : (uint16_t)ticks;
        if (level) {
            w |= 0x8000;
        }
        return writer_.write_u16(w);
    }

   private:
    SdBufferedWrite& writer_;
};

bool convert_wav_to_raw(const char* wav_path, const char* raw_path) {
    File in = SD.open(wav_path, FILE_READ);
    if (!in) {
        return false;
    }
    SdBufferedRead* reader = new SdBufferedRead(&in, 4096);
    auto read_at = [reader](uint32_t pos, uint8_t* buf, uint32_t len) -> int {
        if (!reader->seek(pos)) {
            return -1;
        }
        return reader->read(buf, len);
    };
    WavFormat fmt;
    if (!wav_parse_header(read_at, (uint32_t)in.size(), &fmt)) {
        delete reader;
        in.close();
        return false;
    }

    if (SD.exists(raw_path)) {
        SD.remove(raw_path);
    }
    File out = SD.open(raw_path, FILE_WRITE);
    if (!out) {
        delete reader;
        in.close();
        return false;
    }
    SdBufferedWrite* writer = new SdBufferedWrite(&out);
    RawWordWriter raw(*writer);

    WavEdgeDetector detector;
    detector.begin(fmt.sample_rate);
    reader->seek(fmt.data_offset);

    // 半周期の長さは、切り替わりの時刻(usに丸めたもの)の差にして誤差を積み上げない
    uint64_t prev_us = 0;
    bool ok = true;
    bool started = false;
    uint8_t frame[32];
    uint32_t n = (fmt.block_align < sizeof(frame)) ? fmt.block_align : sizeof(frame);
    uint32_t total = fmt.data_size / fmt.block_align;
    int last_percent = -1;
    draw_conversion_progress(0);

    for (uint32_t i = 0; ok && i < total; i++) {
        if ((i & 0x0FFF) == 0) {
            watchdog_update();
            int percent = (int)((uint64_t)i * 100 / total);
            if (percent != last_percent) {
                draw_conversion_progress(percent);
                last_percent = percent;
            }
        }
        if (reader->read(frame, n) != (int)n) {
            break;
        }
        if (fmt.block_align > n) {
            reader->seek(reader->position() + (fmt.block_align - n));
        }
        uint64_t edge_q8;
        bool high;
        if (!detector.feed(wav_sample_value(frame, fmt.bits), &edge_q8, &high)) {
            continue;
        }
        uint64_t edge_us = (edge_q8 * 1000000ULL + ((uint64_t)fmt.sample_rate << 7)) /
                           ((uint64_t)fmt.sample_rate << 8);
        if (!started) {
            // 最初に波が来るまでの無音は書かない(録音も波が来てから始まる)
            started = true;
            prev_us = edge_us;
            continue;
        }
        if (edge_us > prev_us) {
            ok = raw.half(high, edge_us - prev_us);
            prev_us = edge_us;
        }
    }
    // 最後の半周期(ファイルの終わりまで)
    if (ok && started && detector.state() != 0) {
        uint64_t end_us = detector.samples() * 1000000ULL / fmt.sample_rate;
        if (end_us > prev_us) {
            ok = raw.half(detector.state() > 0, end_us - prev_us);
        }
    }
    ok = writer->flush() && ok;
    draw_conversion_progress(100);

    delete writer;
    delete reader;
    out.close();
    in.close();
    if (!ok) {
        SD.remove(raw_path);
    }
    return ok;
}
