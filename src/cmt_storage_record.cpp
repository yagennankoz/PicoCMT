#include "cmt_storage_record.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>

#include <functional>

#include "cmt_cas_converter.h"
#include "cmt_cmt_converter.h"
#include "cmt_globals.h"
#include "cmt_t88_converter.h"
#include "cmt_ui.h"
#include "pico/util/queue.h"

#define REC_WRITE_BUF_SIZE 2048
static uint16_t rec_sector_buffer[REC_WRITE_BUF_SIZE];
static uint32_t rec_buffer_idx = 0;

#define SILENCE_TIMEOUT_MS 8000

volatile uint32_t rec_last_sound_time_ms = 0;
volatile uint32_t rec_start_time_ms = 0;
volatile bool rec_triggered = false;

static unsigned long silence_start_time_ms = 0;
static bool rec_is_silence_tracking = false;
static File raw_rec_file;

static int rec_valid_pulse_count = 0;

static inline void write_word_to_rec_buffer(uint16_t value) {
    rec_sector_buffer[rec_buffer_idx++] = value;
    if (rec_buffer_idx >= REC_WRITE_BUF_SIZE) {
        if (raw_rec_file) {
            raw_rec_file.write((const uint8_t*)rec_sector_buffer,
                               REC_WRITE_BUF_SIZE * 2);
        }
        rec_buffer_idx = 0;
    }
}

static void flush_rec_buffer() {
    if (rec_buffer_idx > 0 && raw_rec_file) {
        raw_rec_file.write((const uint8_t*)rec_sector_buffer,
                           rec_buffer_idx * 2);
        rec_buffer_idx = 0;
    }
    if (raw_rec_file) {
        raw_rec_file.flush();
    }
}

/**
 * @brief ファイルからファイルへ内容をコピーするヘルパー関数
 */
static bool copy_file_contents(File& dest, File& src) {
    if (!dest || !src) {
        return false;
    }
    uint8_t buffer[512];
    src.seek(0);
    while (src.available()) {
        size_t bytes_read = src.read(buffer, sizeof(buffer));
        if (bytes_read == 0) {
            break;
        }
        if (dest.write(buffer, bytes_read) != bytes_read) {
            return false;  // 書き込み失敗
        }
    }
    return true;
}

/**
 * @brief ファイルからデータブロックのファイル名(プログラム名)を抽出する
 */
static String extract_names_from_file(const char* filepath) {
    File f = SD.open(filepath, FILE_READ);
    if (!f) {
        return "";
    }

    String names = "";
    int match_d3_count = 0;
    int match_p2_state = 0;
    int match_d0_count = 0;
    int extract_count = 0;
    char name_buf[7] = {0};

    while (f.available()) {
        uint8_t c = f.read();

        // 名前抽出中
        if (extract_count > 0) {
            name_buf[6 - extract_count] = (char)c;
            extract_count--;
            if (extract_count == 0) {
                // 不正文字をアンダースコアに置換
                for (int i = 0; i < 6; i++) {
                    char ch = name_buf[i];
                    if (ch < 0x20 || ch > 0x7E || ch == '/' || ch == '\\' ||
                        ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
                        ch == '<' || ch == '>' || ch == '|') {
                        name_buf[i] = '_';
                    }
                }
                // 末尾のスペースとアンダースコアを削除
                for (int i = 5; i >= 0; i--) {
                    if (name_buf[i] == ' ' || name_buf[i] == '_') {
                        name_buf[i] = '\0';
                    } else {
                        break;
                    }
                }

                if (strlen(name_buf) > 0) {
                    String new_name = String(name_buf);
                    // 重複チェック
                    bool exists = false;
                    if (names == new_name) {
                        exists = true;
                    } else if (names.startsWith(new_name + "_")) {
                        exists = true;
                    } else if (names.endsWith("_" + new_name)) {
                        exists = true;
                    } else if (names.indexOf("_" + new_name + "_") != -1) {
                        exists = true;
                    }

                    if (!exists) {
                        if (names.length() > 0) {
                            names += "_";
                        }
                        names += new_name;
                    }
                }
            }
            continue;
        }

        // パターン1: D3 x 10以上 に続く6バイト
        if (c == 0xD3) {
            match_d3_count++;
            match_p2_state = 0;
        } else {
            if (match_d3_count >= 10) {
                name_buf[0] = (char)c;
                extract_count = 5;  // 既に1文字読んだので残り5バイト
                match_d3_count = 0;
                match_p2_state = 0;
                continue;
            }
            match_d3_count = 0;
        }

        // パターン2: 24 24 24 に続く6バイト
        if (c == 0x24) {
            if (match_p2_state == 0) {
                match_p2_state = 1;
            } else if (match_p2_state == 1) {
                match_p2_state = 2;
            } else if (match_p2_state == 2) {
                // マッチ完了、次から抽出開始
                extract_count = 6;
                match_p2_state = 0;
                match_d3_count = 0;
                continue;
            } else {
                match_p2_state = 0;
            }
        } else {
            match_p2_state = 0;
        }

        // パターン3: D0 x 10以上 に続く6バイト
        if (c == 0xD0) {
            match_d0_count++;
            match_p2_state = 0;
        } else {
            if (match_d0_count >= 10) {
                name_buf[0] = (char)c;
                extract_count = 5;  // 既に1文字読んだので残り5バイト
                match_d0_count = 0;
                match_p2_state = 0;
                continue;
            }
            match_d0_count = 0;
        }
    }
    f.close();
    return names;
}

/**
 * @brief
 * 既存のtarget_filenameの末尾に抽出した名前を付加してリネームする（APPEND用）
 */
static void append_names_to_target_filename(const String& names) {
    if (names.length() == 0) {
        return;
    }

    String current(target_filename);
    int dot_idx = current.lastIndexOf('.');
    String ext = "";
    String base = current;
    if (dot_idx > 0) {
        ext = current.substring(dot_idx);
        base = current.substring(0, dot_idx);
    }

    String new_name = base + "_" + names + ext;

    if (current != new_name) {
        if (SD.rename(target_filename, new_name.c_str())) {
            snprintf(target_filename, sizeof(target_filename), "%s",
                     new_name.c_str());
        }
    }
}

/**
 * @brief デフォルトのtarget_filenameを、抽出した名前にリネームする（NEW用）
 */
static void rename_target_filename_with_extracted_names(const String& names) {
    if (names.length() == 0) {
        return;
    }

    String current(target_filename);
    int slash_idx = current.lastIndexOf('/');
    String dir = "/";
    if (slash_idx >= 0) {
        dir = current.substring(0, slash_idx + 1);
    }

    int dot_idx = current.lastIndexOf('.');
    String ext = "";
    if (dot_idx > 0) {
        ext = current.substring(dot_idx);
    }

    String base_new_name = dir + names;
    String new_name = base_new_name + ext;

    int index = 1;
    while (SD.exists(new_name.c_str()) && current != new_name) {
        new_name = base_new_name + "(" + String(index) + ")" + ext;
        index++;
        if (index > 99) {
            break;
        }
    }

    if (current != new_name) {
        if (SD.rename(target_filename, new_name.c_str())) {
            snprintf(target_filename, sizeof(target_filename), "%s",
                     new_name.c_str());
        }
    }
}

/**
 * @brief RAWファイルを指定されたフォーマットに変換し、既存のファイルに追記する
 */
static bool append_converted_raw_to_file(const char* target_path,
                                         const char* raw_path,
                                         RecFormat format) {
    // 1. フォーマットに応じてコンバータと一時ファイルパスを決定
    std::function<bool(const char*, const char*)> converter;
    const char* temp_path = nullptr;

    switch (format) {
        case FMT_T88:
            converter = convert_raw_to_t88;
            temp_path = "/APPEND.T88";
            break;
        case FMT_CAS:  // CAS追加
            converter = convert_raw_to_cas;
            temp_path = "/APPEND.CAS";
            break;
        default:
            return false;
    }

    // 2. RAWを一時ファイルに変換
    if (!converter(raw_path, temp_path)) {
        if (SD.exists(temp_path)) {
            SD.remove(temp_path);
        }
        return false;
    }

    String extracted = extract_names_from_file(temp_path);

    bool success = false;
    char backup_path[128];
    snprintf(backup_path, sizeof(backup_path), "%s.bak", target_path);
    if (SD.exists(backup_path)) {
        SD.remove(backup_path);
    }

    // 3. 元ファイルをバックアップにリネーム
    if (!SD.rename(target_path, backup_path)) {
        SD.remove(temp_path);
        return false;
    }

    File backup_file = SD.open(backup_path, FILE_READ);
    File new_part_file = SD.open(temp_path, FILE_READ);
    File final_file = SD.open(target_path, FILE_WRITE);

    if (backup_file && new_part_file && final_file) {
        if (format == FMT_T88) {
            // --- T88の追記処理 ---
            // T88のシグネチャは 24バイト固定
            uint8_t header[24];
            backup_file.read(header, 24);
            final_file.write(header, 24);

            // 元ファイルからタグをコピー
            while (backup_file.available()) {
                uint16_t tag_id = 0, tag_size = 0;
                if (backup_file.read((uint8_t*)&tag_id, 2) != 2) {
                    break;
                }
                if (backup_file.read((uint8_t*)&tag_size, 2) != 2) {
                    break;
                }

                // ENDタグに到達したらコピーを止める（まだファイルは閉じない）
                if (tag_id == T88_TAG_END) {
                    break;
                }

                final_file.write((uint8_t*)&tag_id, 2);
                final_file.write((uint8_t*)&tag_size, 2);

                uint8_t buffer[256];
                for (size_t i = 0; i < tag_size; i += sizeof(buffer)) {
                    size_t to_read = (tag_size - i > sizeof(buffer))
                                         ? sizeof(buffer)
                                         : tag_size - i;
                    backup_file.read(buffer, to_read);
                    final_file.write(buffer, to_read);
                }
            }

            // 2秒のブランク(無音)をタグとして挿入
            uint16_t blank_tag_id = T88_TAG_BLANK, blank_tag_size = 8;
            uint32_t blank_start_ticks = 0, blank_len_ticks = 9600;
            final_file.write((uint8_t*)&blank_tag_id, 2);
            final_file.write((uint8_t*)&blank_tag_size, 2);
            final_file.write((uint8_t*)&blank_start_ticks, 4);
            final_file.write((uint8_t*)&blank_len_ticks, 4);

            // 追記ファイル（新規録音分）からタグをコピー
            new_part_file.seek(24);  // シグネチャ(24バイト)をスキップ
            while (new_part_file.available()) {
                uint16_t tag_id = 0, tag_size = 0;
                if (new_part_file.read((uint8_t*)&tag_id, 2) != 2) {
                    break;
                }
                if (new_part_file.read((uint8_t*)&tag_size, 2) != 2) {
                    break;
                }

                // VERSIONタグは元ファイルに既にあるためスキップする
                if (tag_id == T88_TAG_VERSION) {
                    new_part_file.seek(new_part_file.position() + tag_size);
                    continue;
                }

                final_file.write((uint8_t*)&tag_id, 2);
                final_file.write((uint8_t*)&tag_size, 2);

                uint8_t buffer[256];
                for (size_t i = 0; i < tag_size; i += sizeof(buffer)) {
                    size_t to_read = (tag_size - i > sizeof(buffer))
                                         ? sizeof(buffer)
                                         : tag_size - i;
                    new_part_file.read(buffer, to_read);
                    final_file.write(buffer, to_read);
                }

                // ENDタグを書き終わったら完了
                if (tag_id == T88_TAG_END) {
                    break;
                }
            }
            success = true;

        } else {
            // CMT / CAS 等の単純連結可能なフォーマット
            if (copy_file_contents(final_file, backup_file)) {
                success = copy_file_contents(final_file, new_part_file);
            }
        }
    }

    if (backup_file) {
        backup_file.close();
    }
    if (new_part_file) {
        new_part_file.close();
    }
    if (final_file) {
        final_file.close();
    }

    if (success) {
        SD.remove(backup_path);

        if (extracted.length() > 0) {
            append_names_to_target_filename(extracted);
        }
    } else {
        // 失敗した場合はバックアップを元に戻す
        if (SD.exists(target_path)) {
            SD.remove(target_path);
        }
        SD.rename(backup_path, target_path);
    }

    SD.remove(temp_path);
    return success;
}

bool start_raw_recording(const char* filename) {
    if (SD.exists(raw_rec_path)) {
        SD.remove(raw_rec_path);
    }
    raw_rec_file = SD.open(raw_rec_path, FILE_WRITE);
    if (!raw_rec_file) {
        return false;
    }

    rec_buffer_idx = 0;

    uint32_t drain_garbage;
    while (queue_try_remove(&cmt_data_queue, &drain_garbage));

    rec_valid_pulse_count = 0;
    rec_triggered = false;
    rec_is_silence_tracking = false;
    rec_last_sound_time_ms = millis();
    rec_start_time_ms = 0;

    recording_permitted = true;
    rec_paused = false;
    recording_active = true;

    return true;
}

void stop_raw_recording_and_finalize() {
    if (!recording_active) {
        return;
    }
    recording_active = false;
    rec_paused = true;

    uint32_t raw_val;
    while (queue_try_remove(&cmt_data_queue, &raw_val)) {
        if (raw_val == 0 || raw_val == 0xFFFFFFFF) {
            continue;
        }

        bool is_high = (raw_val & 0x80000000) != 0;
        uint32_t us = raw_val & 0x7FFFFFFF;
        uint32_t pulse_ticks = US_TO_PIO_TICKS(us);

        uint16_t ticks_16 =
            (pulse_ticks >= 0x7FFF) ? 0x7FFE : (uint16_t)pulse_ticks;
        if (is_high) {
            ticks_16 |= 0x8000;
        }
        write_word_to_rec_buffer(ticks_16);
    }

    if (rec_is_silence_tracking) {
        uint32_t final_silence_ms = millis() - silence_start_time_ms;
        if (final_silence_ms > 0) {
            write_word_to_rec_buffer(0xFFFF);
            write_word_to_rec_buffer((uint16_t)final_silence_ms);
        }
    }

    flush_rec_buffer();
    if (raw_rec_file) {
        raw_rec_file.close();
    }

    draw_conversion_progress(0);
    bool success = false;

    if (selected_rec_mode == MODE_APPEND) {
        success = append_converted_raw_to_file(target_filename, raw_rec_path,
                                               default_rec_fmt);
    } else {
        std::function<bool(const char*, const char*)> converter;
        switch (default_rec_fmt) {
            case FMT_T88:
                converter = convert_raw_to_t88;
                break;
            case FMT_CMT:
                converter = convert_raw_to_cmt;
                break;
            case FMT_CAS:  // CAS追加
                converter = convert_raw_to_cas;
                break;
            default:
                converter = nullptr;
                break;
        }
        if (converter) {
            success = converter(raw_rec_path, target_filename);
            if (success) {
                // 新規作成の場合、変換後に抽出とリネームを実行
                String extracted = extract_names_from_file(target_filename);
                if (extracted.length() > 0) {
                    rename_target_filename_with_extracted_names(extracted);
                }
            }
        }
    }

    draw_conversion_progress(100);
    delay(500);
}

void handle_raw_recording_stream() {
    if (!recording_active) {
        return;
    }

    uint32_t raw_val = 0;

    while (queue_try_remove(&cmt_data_queue, &raw_val)) {
        if (raw_val == 0 || raw_val == 0xFFFFFFFF) {
            continue;
        }

        bool is_high = (raw_val & 0x80000000) != 0;
        uint32_t us = raw_val & 0x7FFFFFFF;
        uint32_t pulse_ticks = US_TO_PIO_TICKS(us);

        if (!rec_triggered) {
            if (pulse_ticks >= 80 && pulse_ticks <= 1500) {
                rec_valid_pulse_count++;
                if (rec_valid_pulse_count >= 50) {
                    rec_triggered = true;
                    rec_start_time_ms = millis();
                }
            } else {
                rec_valid_pulse_count = 0;
            }
            continue;
        }

        rec_last_sound_time_ms = millis();

        if (pulse_ticks >= REC_SILENCE_GAP_TICKS) {
            uint32_t gap_ms = pulse_ticks / 1000;
            if (gap_ms > 0) {
                uint16_t out_ms = (gap_ms > 65534) ? 65534 : (uint16_t)gap_ms;
                write_word_to_rec_buffer(0xFFFF);
                write_word_to_rec_buffer(out_ms);
            }
            continue;
        } else {
            uint16_t ticks_16 =
                (pulse_ticks >= 0x7FFF) ? 0x7FFE : (uint16_t)pulse_ticks;
            if (is_high) {
                ticks_16 |= 0x8000;
            }
            write_word_to_rec_buffer(ticks_16);
        }
    }

    if (rec_triggered) {
        if (millis() - rec_last_sound_time_ms >= SILENCE_TIMEOUT_MS) {
            stop_raw_recording_and_finalize();
        }
    }
}