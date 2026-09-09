#include "player_t88.h"

#include <string.h>

#include "cmt_globals.h"
#include "cmt_hw_config.h"

static uint16_t t88_ticks_to_us_u16(uint32_t ticks) {
    uint64_t us = ((uint64_t)ticks * 1000000ULL + 2400ULL) / 4800ULL;
    if (us == 0) {
        us = 1;
    }
    if (us > 65535ULL) {
        us = 65535ULL;
    }
    return (uint16_t)us;
}

PlayerT88::PlayerT88() {
    cmt_file = nullptr;
    buffered_reader = nullptr;
}

PlayerT88::~PlayerT88() {
    if (buffered_reader) {
        delete buffered_reader;
    }
}

void PlayerT88::set_playback_log(const char* text) {
    if (text == NULL) {
        snprintf(playback_last_log, sizeof(playback_last_log), "idle");
        return;
    }
    snprintf(playback_last_log, sizeof(playback_last_log), "%s", text);
}

bool PlayerT88::read_u16_le(uint16_t* out) {
    return buffered_reader->read_word(*out);
}

bool PlayerT88::read_u32_le(uint32_t* out) {
    uint8_t b[4];
    if (buffered_reader->read(b, 4) != 4) {
        return false;
    }
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
    return true;
}

bool PlayerT88::skip_bytes(uint32_t count) {
    uint32_t pos = buffered_reader->position();
    uint32_t size = cmt_file->size();
    if (pos > size) {
        return false;
    }
    if (count > (size - pos)) {
        return buffered_reader->seek(size);
    }
    return buffered_reader->seek(pos + count);
}

bool PlayerT88::is_t88_file() {
    if (!cmt_file || cmt_file->size() < 24) {
        return false;
    }
    uint8_t sig[24];
    if (!buffered_reader->seek(0)) {
        return false;
    }
    if (buffered_reader->read(sig, 24) != 24) {
        return false;
    }
    return memcmp(sig, T88_SIGNATURE, 24) == 0;
}

bool PlayerT88::init(File* f, const char* filename) {
    cmt_file = f;

    // 既存のバッファがあれば破棄して再生成
    if (buffered_reader) {
        delete buffered_reader;
    }
    buffered_reader = new SdBufferedRead(cmt_file, 4096);

    if (!is_t88_file()) {
        return false;
    }

    playback_t88_end_reached = false;
    playback_source_exhausted = false;
    playback_tag_id = 0;
    playback_tag_ticks_remaining = 0;
    playback_data_tag_active = false;
    playback_data_bytes_remaining = 0;
    playback_data_extra_skip = 0;
    playback_data_actual_type = 0;
    playback_data_frame_bit_index = -1;
    playback_data_bit_active = false;
    playback_queue_last_progress_ms = millis();
    playback_true_file_pos = 24;
    playback_current_start_ticks = 0;
    playback_lock_start_ticks = false;

    init_position_tracking();

    waiting_for_silence = false;
    silence_start_ms = 0;
    silence_target_ms = 0;

    set_playback_log("T88 Play");

    if (buffered_reader->seek(24) && buffered_reader->available() >= 12) {
        uint8_t first_header[4];
        if (buffered_reader->read(first_header, 4) == 4) {
            uint16_t first_id =
                (uint16_t)first_header[0] | ((uint16_t)first_header[1] << 8);
            if (first_id == 0x0100 || first_id == 0x0102 ||
                first_id == 0x0103) {
                uint8_t ticks_buf[8];
                if (buffered_reader->read(ticks_buf, 8) == 8) {
                    uint32_t first_len = (uint32_t)ticks_buf[0] |
                                         ((uint32_t)ticks_buf[1] << 8) |
                                         ((uint32_t)ticks_buf[2] << 16) |
                                         ((uint32_t)ticks_buf[3] << 24);
                    if (first_len > 1000) {
                        buffered_reader->seek(36);
                        playback_true_file_pos = 36;
                    } else {
                        buffered_reader->seek(24);
                    }
                }
            } else {
                buffered_reader->seek(24);
            }
        }
    } else {
        buffered_reader->seek(24);
    }

    return true;
}

void PlayerT88::process(queue_t* play_queue) {
    if (waiting_for_silence) {
        if (millis() - silence_start_ms >= silence_target_ms) {
            waiting_for_silence = false;
        } else {
            return;
        }
    }

    fill_queue(play_queue);

    record_current_position(playback_true_file_pos);

    if (playback_queue_last_progress_ms == 0) {
        playback_queue_last_progress_ms = millis();
    }

    if (playback_t88_end_reached && !playback_source_exhausted) {
        playback_source_exhausted = true;
    }
}

bool PlayerT88::is_end_reached() { return playback_t88_end_reached; }
bool PlayerT88::is_exhausted() { return playback_source_exhausted; }

void PlayerT88::get_status(PlaybackStatus* out) {
    out->playback_t88_mode = true;
    out->playback_t88_end_reached = playback_t88_end_reached;
    out->playback_source_exhausted = playback_source_exhausted;
    out->playback_tag_id = playback_tag_id;
    out->playback_tag_ticks_remaining = playback_tag_ticks_remaining;
    out->playback_data_tag_active = playback_data_tag_active;
    out->playback_data_bytes_remaining = playback_data_bytes_remaining;
    out->playback_data_extra_skip = playback_data_extra_skip;
    out->playback_data_actual_type = playback_data_actual_type;
    out->playback_data_bit_active = playback_data_bit_active;
    out->queue_progress_age_ms =
        (playback_queue_last_progress_ms == 0)
            ? 0
            : (millis() - playback_queue_last_progress_ms);
    out->file_position =
        buffered_reader ? buffered_reader->display_position() : 0;
    out->file_size = cmt_file ? cmt_file->size() : 0;
    snprintf(out->playback_log, sizeof(out->playback_log), "%s",
             playback_last_log);
}

bool PlayerT88::emit_pulses_from_current_tag(queue_t* q) {
    if (playback_tag_ticks_remaining == 0) {
        return true;
    }
    uint32_t queue_free = PLAY_QUEUE_LENGTH - queue_get_level(q);
    if (queue_free == 0) {
        return false;
    }

    if (playback_tag_id == T88_TAG_BLANK) {
        uint16_t pulse_us = t88_ticks_to_us_u16(playback_tag_ticks_remaining);
        if (queue_try_add(q, &pulse_us)) {
            total_enqueued_pulses++;
            playback_tag_ticks_remaining = 0;
            return true;
        }
        return false;
    }

    uint32_t half_ticks = (playback_tag_id == T88_TAG_MARK) ? 1 : 2;
    while (playback_tag_ticks_remaining > 0 && queue_free > 0) {
        uint32_t chunk_ticks = playback_tag_ticks_remaining;
        if (chunk_ticks > half_ticks) {
            chunk_ticks = half_ticks;
        }
        uint16_t pulse_us = t88_ticks_to_us_u16(chunk_ticks);
        if (!queue_try_add(q, &pulse_us)) {
            return false;
        }
        total_enqueued_pulses++;
        playback_tag_ticks_remaining -= chunk_ticks;
        queue_free--;
    }
    return playback_tag_ticks_remaining == 0;
}

bool PlayerT88::begin_next_data_tag_bit() {
    while (true) {
        if (playback_data_bytes_remaining == 0 &&
            playback_data_frame_bit_index < 0) {
            playback_data_tag_active = false;
            playback_data_frame_bit_index = -1;
            playback_data_bit_active = false;
            if (playback_data_extra_skip > 0) {
                buffered_reader->seek(buffered_reader->position() +
                                      playback_data_extra_skip);
                playback_data_extra_skip = 0;
            }
            return false;
        }

        if (playback_data_frame_bit_index < 0) {
            uint8_t b = 0;
            if (!buffered_reader->read_byte(b)) {
                playback_data_bytes_remaining = 0;
                continue;
            }
            playback_data_current_byte = b;
            playback_data_bytes_remaining--;
            playback_data_frame_bit_index = 0;
        }

        int bit_index = playback_data_frame_bit_index;
        playback_data_frame_bit_index++;

        if (bit_index == 0) {
            playback_data_bit_value = 0;
            return true;
        }
        if (bit_index >= 1 && bit_index <= 8) {
            playback_data_bit_value =
                (playback_data_current_byte >> (bit_index - 1)) & 0x01;
            return true;
        }
        if (bit_index == 9 || bit_index == 10) {
            playback_data_bit_value = 1;
            if (bit_index == 10) {
                playback_data_frame_bit_index = -1;
            }
            return true;
        }
        playback_data_frame_bit_index = -1;
    }
}

bool PlayerT88::emit_pulses_from_data_tag(queue_t* q) {
    uint32_t queue_free = PLAY_QUEUE_LENGTH - queue_get_level(q);
    if (queue_free == 0) {
        return false;
    }

    uint32_t bit_ticks = (playback_data_actual_type & 0x0100) ? 4 : 8;

    while (queue_free > 0) {
        if (!playback_data_bit_active) {
            if (!begin_next_data_tag_bit()) {
                return true;
            }
            playback_data_bit_ticks_remaining = bit_ticks;
            playback_data_bit_active = true;
        }

        uint32_t half_ticks = playback_data_bit_value ? 1 : 2;
        uint32_t emit_ticks = playback_data_bit_ticks_remaining;
        if (emit_ticks > half_ticks) {
            emit_ticks = half_ticks;
        }

        uint16_t pulse_us = t88_ticks_to_us_u16(emit_ticks);
        if (!queue_try_add(q, &pulse_us)) {
            return false;
        }

        total_enqueued_pulses++;

        playback_data_bit_ticks_remaining -= emit_ticks;
        if (playback_data_bit_ticks_remaining == 0) {
            playback_data_bit_active = false;
        }
        queue_free--;
    }
    return false;
}

void PlayerT88::fill_queue(queue_t* q) {
    if (playback_t88_end_reached) {
        return;
    }
    uint32_t iterations = 0;

    while ((PLAY_QUEUE_LENGTH - queue_get_level(q)) > (SD_READ_SIZE / 2)) {
        if (++iterations > 64) {
            break;
        }

        if (!playback_data_tag_active && playback_tag_ticks_remaining == 0 &&
            buffered_reader->available() <= 0) {
            playback_t88_end_reached = true;
            break;
        }

        if (playback_data_tag_active) {
            if (!emit_pulses_from_data_tag(q)) {
                break;
            }
            continue;
        }

        if (playback_tag_ticks_remaining > 0) {
            if (!emit_pulses_from_current_tag(q)) {
                break;
            }
            continue;
        }

        uint32_t current_loop_pos = buffered_reader->position();
        playback_true_file_pos = current_loop_pos;

        uint16_t tag_id = 0;
        uint16_t tag_size = 0;
        uint8_t tag_header[4];
        if (buffered_reader->read(tag_header, 4) != 4) {
            playback_t88_end_reached = true;
            break;
        }
        tag_id = (uint16_t)tag_header[0] | ((uint16_t)tag_header[1] << 8);
        tag_size = (uint16_t)tag_header[2] | ((uint16_t)tag_header[3] << 8);

        if (tag_id == T88_TAG_END ||
            buffered_reader->position() == current_loop_pos) {
            playback_t88_end_reached = true;
            break;
        }

        if (tag_id == T88_TAG_VERSION) {
            if (!skip_bytes(tag_size)) {
                playback_t88_end_reached = true;
            }
            continue;
        }

        if (tag_id == T88_TAG_BLANK || tag_id == T88_TAG_SPACE ||
            tag_id == T88_TAG_MARK) {
            if (tag_size < 8) {
                buffered_reader->seek(current_loop_pos + 4 + tag_size);
                continue;
            }
            uint32_t start_ticks, len_ticks;
            uint8_t ticks_buf[8];
            if (buffered_reader->read(ticks_buf, 8) != 8) {
                playback_t88_end_reached = true;
                break;
            }
            start_ticks =
                (uint32_t)ticks_buf[0] | ((uint32_t)ticks_buf[1] << 8) |
                ((uint32_t)ticks_buf[2] << 16) | ((uint32_t)ticks_buf[3] << 24);
            len_ticks = (uint32_t)ticks_buf[4] | ((uint32_t)ticks_buf[5] << 8) |
                        ((uint32_t)ticks_buf[6] << 16) |
                        ((uint32_t)ticks_buf[7] << 24);

            if (!playback_lock_start_ticks) {
                playback_current_start_ticks = start_ticks;
            }

            if (tag_size > 8) {
                if (!skip_bytes(tag_size - 8)) {
                    playback_t88_end_reached = true;
                    break;
                }
            }

            if (tag_id == T88_TAG_BLANK) {
                uint32_t gap_us =
                    (uint32_t)(((uint64_t)len_ticks * 1000000ULL) / 4800ULL);
                if (gap_us >= 5000) {
                    uint16_t us_val = 2;
                    queue_try_add(q, &us_val);
                    waiting_for_silence = true;
                    silence_start_ms = millis();
                    silence_target_ms = gap_us / 1000;
                    playback_tag_ticks_remaining = 0;
                    break;
                }
            }

            playback_tag_id = tag_id;
            playback_tag_ticks_remaining = len_ticks;
            continue;
        }

        if (tag_id == T88_TAG_DATA) {
            if (tag_size < 12) {
                buffered_reader->seek(current_loop_pos + 4 + tag_size);
                continue;
            }
            uint8_t data_buf[12];
            if (buffered_reader->read(data_buf, 12) != 12) {
                playback_t88_end_reached = true;
                break;
            }

            uint16_t actual_len =
                (uint16_t)data_buf[8] | ((uint16_t)data_buf[9] << 8);
            uint16_t actual_type =
                (uint16_t)data_buf[10] | ((uint16_t)data_buf[11] << 8);

            uint32_t payload_area_len = (uint32_t)tag_size - 12;
            uint32_t payload_len = actual_len;
            if (payload_len > payload_area_len) {
                payload_len = payload_area_len;
            }

            playback_data_tag_active = (payload_len > 0);
            playback_data_bytes_remaining = payload_len;
            playback_data_extra_skip = payload_area_len - payload_len;
            playback_data_actual_type = actual_type;
            playback_data_frame_bit_index = -1;
            playback_data_bit_active = false;

            if (!playback_data_tag_active && playback_data_extra_skip > 0) {
                if (!skip_bytes(playback_data_extra_skip)) {
                    playback_t88_end_reached = true;
                }
                playback_data_extra_skip = 0;
            }
            continue;
        }

        if (tag_size > 0 && !skip_bytes(tag_size)) {
            playback_t88_end_reached = true;
            break;
        }
    }
}

bool PlayerT88::scan_for_blank_tag(uint32_t current_pos, int direction,
                                   uint32_t* target_pos) {
    if (target_pos == NULL || !buffered_reader) {
        return false;
    }

    uint32_t saved_pos = buffered_reader->position();
    uint32_t best_prev_pos = 0;
    uint32_t current_track_pos = 0;

    if (!buffered_reader->seek(24)) {
        buffered_reader->seek(saved_pos);
        return false;
    }

    uint32_t scan_start_ms = millis();
    bool timeout_occurred = false;

    while (buffered_reader->position() < cmt_file->size()) {
        watchdog_update();
        if (millis() - scan_start_ms > 200) {
            timeout_occurred = true;
            break;
        }

        uint32_t tag_pos = buffered_reader->position();
        uint8_t scan_header[4];
        if (buffered_reader->read(scan_header, 4) != 4) {
            break;
        }
        uint16_t tag_id =
            (uint16_t)scan_header[0] | ((uint16_t)scan_header[1] << 8);
        uint16_t tag_size =
            (uint16_t)scan_header[2] | ((uint16_t)scan_header[3] << 8);

        if (tag_id == T88_TAG_BLANK) {
            if (tag_pos > playback_true_file_pos) {
                if (direction > 0) {
                    *target_pos = tag_pos;
                    buffered_reader->seek(saved_pos);
                    return true;
                }
                break;
            } else {
                best_prev_pos = current_track_pos;
                current_track_pos = tag_pos;
            }
        }

        if (tag_id == T88_TAG_END) {
            break;
        } else if (tag_id == T88_TAG_VERSION) {
            if (!skip_bytes(tag_size)) {
                break;
            }
        } else if (tag_id == T88_TAG_BLANK || tag_id == T88_TAG_SPACE ||
                   tag_id == T88_TAG_MARK) {
            if (tag_size < 8) {
                if (tag_size > 0 && !skip_bytes(tag_size)) {
                    break;
                }
                continue;
            }
            if (!skip_bytes(8)) {
                break;
            }
            if (tag_size > 8) {
                if (!skip_bytes(tag_size - 8)) {
                    break;
                }
            }
        } else if (tag_id == T88_TAG_DATA) {
            if (!skip_bytes(tag_size)) {
                break;
            }
        } else {
            if (tag_size > 0 && !skip_bytes(tag_size)) {
                break;
            }
        }
    }

    buffered_reader->seek(saved_pos);
    if (timeout_occurred) {
        set_playback_log("SEEK_ERR");
        return false;
    }

    if (direction < 0) {
        bool is_elapsed_3s =
            (millis() - playback_queue_last_progress_ms >= 3000);
        if (is_elapsed_3s) {
            if (current_track_pos != 0) {
                *target_pos = current_track_pos;
                return true;
            }
        } else {
            if (best_prev_pos != 0) {
                *target_pos = best_prev_pos;
                return true;
            }
        }
        *target_pos = 24;
        return true;
    }
    return false;
}

bool PlayerT88::seek(int direction, queue_t* q) {
    uint32_t current_pos = buffered_reader->position();
    uint32_t target_pos = 0;

    if (!scan_for_blank_tag(current_pos, direction, &target_pos)) {
        set_playback_log("SEEK ERR");
        return false;
    }

    if (!buffered_reader->seek(target_pos)) {
        return false;
    }

    uint32_t target_start_ticks = 0;

    // 先頭巻き戻し時は無条件で0にリセット。
    // それ以外の場合は時間タグか判定してからTicksを取得する。
    if (target_pos == 24) {
        target_start_ticks = 0;
    } else {
        uint16_t tag_id = 0, tag_size = 0;
        if (read_u16_le(&tag_id) && read_u16_le(&tag_size)) {
            if (tag_id >= 0x0100 && tag_size >= 8) {
                read_u32_le(&target_start_ticks);
            }
        }
    }

    // タグヘッダ確認後にシーク先へポインタを戻す
    buffered_reader->seek(target_pos);

    playback_current_start_ticks = target_start_ticks;
    playback_lock_start_ticks = true;

    playback_base_time_ms =
        (uint32_t)(((uint64_t)target_start_ticks * 1000ULL) / 4800ULL);
    playback_tag_start_system_ms = millis();
    playback_time_valid = true;

    playback_t88_end_reached = false;
    playback_source_exhausted = false;
    playback_tag_id = 0;
    playback_tag_ticks_remaining = 0;
    playback_data_tag_active = false;
    playback_data_bytes_remaining = 0;
    playback_data_extra_skip = 0;
    playback_data_actual_type = 0;
    playback_data_frame_bit_index = -1;
    playback_data_bit_active = false;
    playback_true_file_pos = target_pos;
    playback_queue_last_progress_ms = millis();

    waiting_for_silence = false;

    fill_queue(q);

    playback_lock_start_ticks = false;
    return true;
}

void PlayerT88::stop() {}