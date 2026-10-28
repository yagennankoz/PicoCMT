#include "cmt_p6t_converter.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>

#include "cmt_fsk_demodulator.h"
#include "cmt_globals.h"
#include "cmt_p6t_format.h"
#include "cmt_sd_buffer.h"
#include "cmt_ui.h"
#include "hardware/watchdog.h"

static constexpr int HEADER_D3_COUNT = 10;
static constexpr int HEADER_NAME_LEN = 6;
// 復調器がバイトを出すのは1つ目のストップビットの後。バイトの始まりはその 10ビット前
static constexpr int BITS_BEFORE_CALLBACK = 10;

static uint16_t clamp_ms(uint64_t us) {
    uint64_t ms = (us + 500) / 1000;
    return (uint16_t)(ms > 65535 ? 65535 : ms);
}

namespace {

// 変換中のブロックの情報
struct BlockBuilder {
    std::vector<P6TBlock> blocks;
    uint8_t head[HEADER_D3_COUNT + HEADER_NAME_LEN];  // ブロックの先頭16バイト (ヘッダの判定用)
    int next_id = 0;

    void begin(uint32_t offset, int baud, uint64_t silence_us, uint64_t leader_us) {
        P6TBlock b;
        b.offset = offset;
        b.baud = (uint16_t)baud;
        b.stime_ms = clamp_ms(silence_us);
        b.ptime_ms = clamp_ms(leader_us);
        // ID はヘッダのブロックで新しくし (add で決める)、続くプログラムのブロックは同じにする
        b.id = blocks.empty() ? (uint8_t)next_id++ : blocks.back().id;
        blocks.push_back(b);
    }

    void add(uint8_t v) {
        P6TBlock& b = blocks.back();
        if (b.size < sizeof(head)) {
            head[b.size] = v;
        }
        b.size++;
        if (b.size == sizeof(head)) {
            bool is_header = true;
            for (int i = 0; i < HEADER_D3_COUNT; i++) {
                is_header = is_header && (head[i] == 0xD3);
            }
            if (is_header) {
                // ヘッダのブロック: 新しい ID と、ファイル名 (6文字) を付ける
                if (blocks.size() > 1) {
                    b.id = (uint8_t)next_id++;
                }
                memset(b.name, 0, sizeof(b.name));
                memcpy(b.name, head + HEADER_D3_COUNT, HEADER_NAME_LEN);
            }
        }
    }

    // プログラムのブロックにも、同じ ID のヘッダのファイル名を付ける
    void finish() {
        for (size_t i = 1; i < blocks.size(); i++) {
            if (blocks[i].name[0] == '\0' && blocks[i].id == blocks[i - 1].id) {
                memcpy(blocks[i].name, blocks[i - 1].name, sizeof(blocks[i].name));
            }
        }
    }
};

}  // namespace

bool convert_raw_to_p6t(const char* raw_path, const char* p6t_path) {
    if (raw_path == nullptr || p6t_path == nullptr) {
        return false;
    }
    File raw_file = SD.open(raw_path, FILE_READ);
    if (!raw_file) {
        return false;
    }
    uint32_t total_size = raw_file.size();
    int last_percent = -1;

    if (SD.exists(p6t_path)) {
        SD.remove(p6t_path);
    }
    File out_file = SD.open(p6t_path, FILE_WRITE);
    if (!out_file) {
        raw_file.close();
        return false;
    }

    SdBufferedRead* reader = new SdBufferedRead(&raw_file);
    SdBufferedWrite* writer = new SdBufferedWrite(&out_file);
    BlockBuilder builder;

    draw_conversion_progress(0);

    FskDemodulator demod;
    bool write_ok = true;
    uint32_t data_size = 0;

    bool have_byte = false;
    uint64_t last_byte_end_us = 0;  // 直前のバイトを出した時刻
    uint64_t silence_us = 0;        // 直前のバイトの後の無音の合計
    uint64_t signal_start_us = 0;   // 無音の後で信号が戻った時刻

    demod.on_byte_decoded = [&](uint8_t v) {
        int baud = (demod.baud_rate == FskDemodulator::BAUD_600) ? 600 : 1200;
        uint64_t now = demod.current_time_us;
        uint64_t byte_us = (uint64_t)BITS_BEFORE_CALLBACK * 1000000ULL / baud;
        uint64_t byte_start = (now > byte_us) ? now - byte_us : 0;

        // 無音の後、または長いピー音の後なら新しいブロック
        uint64_t leader_from = (!have_byte || silence_us > 0) ? signal_start_us : last_byte_end_us;
        uint64_t leader_us = (byte_start > leader_from) ? byte_start - leader_from : 0;
        bool new_block = !have_byte || silence_us > 0 ||
                         leader_us > (uint64_t)P6T_SPLIT_LEADER_MS * 1000ULL;
        if (new_block && builder.blocks.size() < P6T_MAX_BLOCKS) {
            builder.begin(data_size, baud, silence_us, leader_us);
        }
        builder.add(v);

        if (!writer->write_bytes(&v, 1)) {
            write_ok = false;
        }
        data_size++;
        have_byte = true;
        last_byte_end_us = now;
        silence_us = 0;
    };

    uint16_t raw_word = 0;
    uint32_t loop_count = 0;

    while (write_ok && reader->read_word(raw_word)) {
        loop_count++;
        if ((loop_count & 0x01FF) == 0) {
            watchdog_update();
            if (total_size > 0) {
                int percent = ((loop_count * 2) * 100) / total_size;
                if (percent > 100) {
                    percent = 100;
                }
                if (percent != last_percent) {
                    draw_conversion_progress(percent);
                    last_percent = percent;
                }
            }
        }

        if (raw_word == 0x0000) {
            continue;
        }

        if (raw_word == 0xFFFF) {
            uint16_t ms_word = 100;
            if (!reader->read_word(ms_word)) {
                ms_word = 100;
            }
            loop_count++;
            demod.advance_gap((uint32_t)ms_word * 1000);  // ためていたバイトを先に出す
            if (have_byte) {
                silence_us += (uint64_t)ms_word * 1000;
            }
            signal_start_us = demod.current_time_us;
            continue;
        }

        uint32_t pulse_us = raw_pulse_us(raw_word);
        if (pulse_us > 2000) {
            // 長いパルス (短い無音) も無音として数える。ためていたバイトを先に出させる
            demod.feed_pulse((raw_word & 0x8000) != 0, pulse_us);
            if (have_byte) {
                silence_us += pulse_us;
            }
            signal_start_us = demod.current_time_us;
            continue;
        }
        demod.feed_pulse((raw_word & 0x8000) != 0, pulse_us);
    }
    demod.finish();

    builder.finish();
    if (write_ok && !builder.blocks.empty()) {
        write_ok = p6t_write_footer(writer, builder.blocks, data_size);
    }
    if (!writer->flush()) {
        write_ok = false;
    }
    draw_conversion_progress(100);

    delete writer;
    delete reader;
    raw_file.close();
    out_file.close();
    return write_ok && !builder.blocks.empty();
}

bool p6t_append(const char* base_path, const char* add_path, const char* out_path) {
    File base = SD.open(base_path, FILE_READ);
    File add = SD.open(add_path, FILE_READ);
    if (!base || !add) {
        return false;
    }
    P6TInfo a, b;
    bool ok = p6t_read_info(base, a) && p6t_read_info(add, b) &&
              a.blocks.size() + b.blocks.size() <= P6T_MAX_BLOCKS;

    File out;
    if (ok) {
        if (SD.exists(out_path)) {
            SD.remove(out_path);
        }
        out = SD.open(out_path, FILE_WRITE);
        ok = (bool)out;
    }

    if (ok) {
        SdBufferedWrite* writer = new SdBufferedWrite(&out);
        uint8_t buf[512];
        // ベタイメージを順に書く (base → add)
        struct Part {
            File* f;
            uint32_t size;
        } parts[2] = {{&base, a.data_size}, {&add, b.data_size}};
        for (const Part& p : parts) {
            p.f->seek(0);
            uint32_t left = p.size;
            while (ok && left > 0) {
                watchdog_update();
                uint32_t n = (left > sizeof(buf)) ? sizeof(buf) : left;
                ok = (p.f->read(buf, n) == (int)n) && writer->write_bytes(buf, n);
                left -= n;
            }
        }

        // ブロックの情報: add の位置をずらし、ID を base の後に続ける
        std::vector<P6TBlock> blocks = a.blocks;
        int id_base = 0;
        for (const P6TBlock& blk : a.blocks) {
            id_base = (blk.id + 1 > id_base) ? blk.id + 1 : id_base;
        }
        for (P6TBlock blk : b.blocks) {
            blk.offset += a.data_size;
            blk.id = (uint8_t)(blk.id + id_base);
            blocks.push_back(blk);
        }
        ok = ok && p6t_write_footer(writer, blocks, a.data_size + b.data_size);
        ok = writer->flush() && ok;
        delete writer;
        out.close();
    }
    base.close();
    add.close();
    return ok;
}
