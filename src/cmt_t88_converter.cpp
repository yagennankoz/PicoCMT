#include "cmt_t88_converter.h"

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

enum TagType {
    TAG_TYPE_NONE = 0,
    TAG_TYPE_BLANK,
    TAG_TYPE_MARK,
    TAG_TYPE_SPACE,
    TAG_TYPE_DATA
};

static bool write_t88_header(SdBufferedWrite& writer) {
    if (!writer.write_bytes((const uint8_t*)T88_SIGNATURE, 24)) {
        return false;
    }
    if (!writer.write_u16(T88_TAG_VERSION)) {
        return false;
    }
    if (!writer.write_u16(0x0002)) {
        return false;
    }
    if (!writer.write_u16(T88_VERSION_1_0)) {
        return false;
    }
    return true;
}

static bool write_t88_carrier_tag(SdBufferedWrite& writer, uint16_t tag_id,
                                  uint32_t start_ticks, uint32_t len_ticks) {
    if (len_ticks == 0) {
        return true;
    }
    if (!writer.write_u16(tag_id)) {
        return false;
    }
    if (!writer.write_u16(0x0008)) {
        return false;
    }
    if (!writer.write_u32(start_ticks)) {
        return false;
    }
    if (!writer.write_u32(len_ticks)) {
        return false;
    }
    return true;
}

static bool write_t88_data_tag(SdBufferedWrite& writer, uint32_t start_ticks,
                               uint32_t len_ticks, const uint8_t* payload,
                               uint16_t actual_len, uint16_t actual_type) {
    if (actual_len == 0) {
        return true;
    }
    uint16_t tag_size = 12 + actual_len;
    if (!writer.write_u16(T88_TAG_DATA)) {
        return false;
    }
    if (!writer.write_u16(tag_size)) {
        return false;
    }
    if (!writer.write_u32(start_ticks)) {
        return false;
    }
    if (!writer.write_u32(len_ticks)) {
        return false;
    }
    if (!writer.write_u16(actual_len)) {
        return false;
    }
    if (!writer.write_u16(actual_type)) {
        return false;
    }
    if (!writer.write_bytes(payload, actual_len)) {
        return false;
    }
    return true;
}

static bool write_t88_end_tag(SdBufferedWrite& writer) {
    if (!writer.write_u16(T88_TAG_END)) {
        return false;
    }
    if (!writer.write_u16(0x0000)) {
        return false;
    }
    return true;
}

class T88TagWriter {
   private:
    SdBufferedWrite& writer;
    uint16_t pending_tag_id;
    uint32_t pending_start_ticks;
    uint32_t pending_len_ticks;
    const uint32_t NOISE_THRESHOLD = 96;
    bool end_written;

    bool commit_pending() {
        if (pending_len_ticks > 0) {
            if (pending_len_ticks >= 24) {
                if (!write_t88_carrier_tag(writer, pending_tag_id,
                                           pending_start_ticks,
                                           pending_len_ticks)) {
                    return false;
                }
            }
            pending_len_ticks = 0;
            pending_tag_id = 0;
        }
        return true;
    }

   public:
    T88TagWriter(SdBufferedWrite& w)
        : writer(w),
          pending_tag_id(0),
          pending_start_ticks(0),
          pending_len_ticks(0),
          end_written(false) {}
    ~T88TagWriter() { write_end(); }

    bool write_carrier(uint16_t tag_id, uint32_t start_ticks,
                       uint32_t len_ticks) {
        if (len_ticks == 0) {
            return true;
        }
        if (pending_len_ticks > 0) {
            if (pending_tag_id == tag_id) {
                pending_len_ticks += len_ticks;
                return true;
            } else {
                if (pending_tag_id == T88_TAG_BLANK &&
                    pending_len_ticks < NOISE_THRESHOLD &&
                    tag_id != T88_TAG_BLANK) {
                    pending_tag_id = tag_id;
                    pending_len_ticks += len_ticks;
                    return true;
                }
                if (tag_id == T88_TAG_BLANK && len_ticks < NOISE_THRESHOLD &&
                    pending_tag_id != T88_TAG_BLANK) {
                    pending_len_ticks += len_ticks;
                    return true;
                }
                if (!commit_pending()) {
                    return false;
                }
            }
        }
        pending_tag_id = tag_id;
        pending_start_ticks = start_ticks;
        pending_len_ticks = len_ticks;
        return true;
    }

    bool write_data(uint32_t start_ticks, uint32_t len_ticks,
                    const uint8_t* payload, uint16_t actual_len,
                    uint16_t actual_type) {
        if (actual_len == 0) {
            return true;
        }
        if (!commit_pending()) {
            return false;
        }
        return write_t88_data_tag(writer, start_ticks, len_ticks, payload,
                                  actual_len, actual_type);
    }

    bool write_end() {
        if (end_written) {
            return true;
        }
        bool res = true;
        if (!commit_pending()) {
            res = false;
        }
        if (!write_t88_end_tag(writer)) {
            res = false;
        }
        end_written = true;
        return res;
    }
};

bool convert_raw_to_t88(const char* raw_path, const char* t88_path) {
    if (raw_path == nullptr || t88_path == nullptr) {
        return false;
    }
    File raw_file = SD.open(raw_path, FILE_READ);
    if (!raw_file) {
        return false;
    }
    uint32_t total_size = raw_file.size();
    int last_percent = -1;

    if (SD.exists(t88_path)) {
        SD.remove(t88_path);
    }
    File t88_file = SD.open(t88_path, FILE_WRITE);
    if (!t88_file) {
        raw_file.close();
        return false;
    }

    SdBufferedRead* reader = new SdBufferedRead(&raw_file);
    SdBufferedWrite* writer = new SdBufferedWrite(&t88_file);
    T88TagWriter* tag_writer = new T88TagWriter(*writer);
    uint8_t* data_payload = new uint8_t[CONV_MAX_DATA_PAYLOAD];
    uint16_t data_payload_count = 0;

    auto cleanup = [&]() {
        delete tag_writer;
        delete writer;
        delete reader;
        delete[] data_payload;
        raw_file.close();
        t88_file.close();
    };

    if (!write_t88_header(*writer)) {
        cleanup();
        return false;
    }
    draw_conversion_progress(0);

    FskDemodulator demod;
    TagType block_type = TAG_TYPE_NONE;
    uint64_t block_start_us = 0;
    uint64_t last_data_end_us = 0;

    auto flush_block = [&](uint64_t end_us) -> bool {
        if (block_type == TAG_TYPE_NONE || end_us <= block_start_us) {
            return true;
        }
        uint32_t start_ticks = (uint32_t)((block_start_us * 4800) / 1000000);
        uint32_t end_ticks = (uint32_t)((end_us * 4800) / 1000000);
        uint32_t len_ticks = end_ticks - start_ticks;

        if (len_ticks == 0) {
            block_start_us = end_us;
            return true;
        }

        if (block_type == TAG_TYPE_DATA) {
            uint16_t b_rate = (demod.baud_rate == FskDemodulator::BAUD_1200)
                                  ? 0x01CC
                                  : 0x00CC;
            if (data_payload_count < 2) {
                if (!tag_writer->write_carrier(T88_TAG_MARK, start_ticks,
                                               len_ticks)) {
                    return false;
                }
            } else {
                if (!tag_writer->write_data(start_ticks, len_ticks,
                                            data_payload, data_payload_count,
                                            b_rate)) {
                    return false;
                }
            }
            data_payload_count = 0;
        } else {
            uint16_t tag_id = (block_type == TAG_TYPE_SPACE)   ? T88_TAG_SPACE
                              : (block_type == TAG_TYPE_BLANK) ? T88_TAG_BLANK
                                                               : T88_TAG_MARK;
            if (!tag_writer->write_carrier(tag_id, start_ticks, len_ticks)) {
                return false;
            }
        }
        block_start_us = end_us;
        return true;
    };

    demod.on_byte_decoded = [&](uint8_t byte_val) {
        if (data_payload_count < CONV_MAX_DATA_PAYLOAD) {
            data_payload[data_payload_count++] = byte_val;
        }
        last_data_end_us = demod.current_time_us;

        if (data_payload_count >= CONV_MAX_DATA_PAYLOAD) {
            flush_block(demod.current_time_us);
            block_type = TAG_TYPE_DATA;
        }
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

        if (raw_word == 0xFFFF) {
            uint16_t ms_word = 100;
            if (!reader->read_word(ms_word)) {
                ms_word = 100;
            }
            loop_count++;

            uint64_t silence_us = ms_word * 1000ULL;
            if (!flush_block(demod.current_time_us)) {
                cleanup();
                return false;
            }

            block_type = TAG_TYPE_BLANK;
            block_start_us = demod.current_time_us;
            demod.advance_gap(silence_us);

            if (!flush_block(demod.current_time_us)) {
                cleanup();
                return false;
            }
            block_type = TAG_TYPE_NONE;
            continue;
        }

        bool is_high = (raw_word & 0x8000) != 0;
        uint32_t pulse_us = pio_ticks_to_us((uint32_t)(raw_word & 0x7FFF));

        demod.feed_pulse(is_high, pulse_us);

        bool is_idle = (demod.uart_state == 0);
        bool is_space = (pulse_us >= 312);
        TagType current_type = is_idle
                                   ? (is_space ? TAG_TYPE_SPACE : TAG_TYPE_MARK)
                                   : TAG_TYPE_DATA;

        if (block_type == TAG_TYPE_DATA) {
            if (current_type != TAG_TYPE_DATA) {
                if (demod.current_time_us - last_data_end_us > 100000) {
                    if (!flush_block(last_data_end_us)) {
                        cleanup();
                        return false;
                    }
                    block_type = current_type;
                    block_start_us = last_data_end_us;
                }
            }
        } else {
            if (block_type != current_type && current_type != TAG_TYPE_DATA) {
                if (!flush_block(demod.last_edge_time_us)) {
                    cleanup();
                    return false;
                }
                block_type = current_type;
            } else if (current_type == TAG_TYPE_DATA &&
                       block_type != TAG_TYPE_DATA) {
                if (!flush_block(demod.window_start_us)) {
                    cleanup();
                    return false;
                }
                block_type = TAG_TYPE_DATA;
            }
        }
    }

    if (!flush_block(demod.current_time_us)) {
        cleanup();
        return false;
    }

    tag_writer->write_end();
    writer->flush();

    draw_conversion_progress(100);

    cleanup();
    return true;
}