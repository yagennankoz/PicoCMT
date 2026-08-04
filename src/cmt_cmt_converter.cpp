#include "cmt_cmt_converter.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>

#include <functional>

#include "cmt_fsk_demodulator.h"
#include "cmt_globals.h"
#include "cmt_sd_buffer.h"
#include "cmt_ui.h"
#include "hardware/watchdog.h"

#define CONV_MAX_DATA_PAYLOAD 32768

bool convert_raw_to_cmt(const char* raw_path, const char* cmt_path) {
    if (raw_path == nullptr || cmt_path == nullptr) {
        return false;
    }
    File raw_file = SD.open(raw_path, FILE_READ);
    if (!raw_file) {
        return false;
    }
    uint32_t total_size = raw_file.size();
    int last_percent = -1;

    if (SD.exists(cmt_path)) {
        SD.remove(cmt_path);
    }
    File cmt_file = SD.open(cmt_path, FILE_WRITE);
    if (!cmt_file) {
        raw_file.close();
        return false;
    }

    SdBufferedRead* reader = new SdBufferedRead(&raw_file);
    SdBufferedWrite* writer = new SdBufferedWrite(&cmt_file);

    // データペイロードを保持するバッファを動的に確保
    uint8_t* data_payload = new uint8_t[CONV_MAX_DATA_PAYLOAD];
    uint16_t data_payload_count = 0;

    auto cleanup = [&]() {
        delete[] data_payload;  // バッファを解放
        delete writer;
        delete reader;
        raw_file.close();
        cmt_file.close();
    };

    draw_conversion_progress(0);

    FskDemodulator demod;
    uint64_t last_data_end_us = 0;
    bool in_data_block = false;

    // データブロックをファイルに書き出すためのヘルパー関数
    auto flush_data_block = [&]() {
        if (data_payload_count > 0) {
            writer->write_bytes(data_payload, data_payload_count);
            data_payload_count = 0;
        }
        in_data_block = false;
    };

    // デコードされたバイトは、即時書き込まずバッファに溜める
    demod.on_byte_decoded = [&](uint8_t byte_val) {
        if (data_payload_count < CONV_MAX_DATA_PAYLOAD) {
            data_payload[data_payload_count++] = byte_val;
        }
        last_data_end_us = demod.current_time_us;
    };

    uint16_t raw_word = 0;
    uint32_t loop_count = 0;

    while (reader->read_word(raw_word)) {
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

        // 無音区間が来たら、それまでのデータブロックを書き出す
        if (raw_word == 0xFFFF) {
            flush_data_block();
            uint16_t ms_word = 100;
            if (!reader->read_word(ms_word)) {
                ms_word = 100;
            }
            loop_count++;
            demod.advance_gap(ms_word * 1000);
            continue;
        }

        bool is_high = (raw_word & 0x8000) != 0;
        uint32_t pulse_us = pio_ticks_to_us((uint32_t)(raw_word & 0x7FFF));

        demod.feed_pulse(is_high, pulse_us);

        // デモジュレータがUARTデータ伝送状態か (アイドル状態でないか)
        // をチェック
        bool is_in_uart_data = (demod.uart_state != 0);

        if (is_in_uart_data) {
            in_data_block = true;
        } else {
            // アイドル状態に戻った場合
            if (in_data_block) {
                // データブロックの直後か、最後のデータから一定時間経過したらブロックを確定する
                if (demod.current_time_us - last_data_end_us >
                    100000) {  // 100ms
                    flush_data_block();
                }
            }
        }
    }

    // ファイル末尾に残っている可能性のあるデータを書き出す
    flush_data_block();

    writer->flush();
    draw_conversion_progress(100);

    cleanup();
    return true;
}