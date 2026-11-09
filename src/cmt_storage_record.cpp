#include "cmt_storage_record.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>

#include <functional>

#include "cmt_cas_converter.h"
#include "cmt_cmt_converter.h"
#include "cmt_core1.h"
#include "cmt_globals.h"
#include "cmt_p6t_converter.h"
#include "cmt_t77_converter.h"
#include "cmt_t77_format.h"
#include "cmt_t88_converter.h"
#include "cmt_wav_converter.h"
#include "cmt_ui.h"
#include "hardware/watchdog.h"
#include "pico/util/queue.h"

#define REC_WRITE_BUF_SIZE 2048
static uint16_t rec_sector_buffer[REC_WRITE_BUF_SIZE];
static uint32_t rec_buffer_idx = 0;

#define SILENCE_TIMEOUT_MS 8000

volatile uint32_t rec_last_sound_time_ms = 0;
volatile uint32_t rec_start_time_ms = 0;
volatile bool rec_triggered = false;

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
 * @brief ファイルからファイルへ内容を安全にコピーするヘルパー関数
 */
static bool copy_file_contents(File& dest, File& src, uint32_t start = 0) {
    if (!dest || !src) {
        return false;
    }
    uint8_t buffer[512];
    src.seek(start);
    while (src.available()) {
        int bytes_read = src.read(buffer, sizeof(buffer));
        if (bytes_read <= 0) {
            break;
        }
        if (dest.write(buffer, bytes_read) != (size_t)bytes_read) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 抽出したファイル名(len バイト)をファイル名に使える形に整え、names
 * に重複なく "_" 区切りで追加する
 */
static void add_extracted_name(String& names, const char* raw, int len) {
    char name_buf[T77_NAME_LEN + 1] = {0};
    if (len > T77_NAME_LEN) {
        len = T77_NAME_LEN;
    }
    memcpy(name_buf, raw, len);

    // 不正文字をアンダースコアに置換
    for (int i = 0; i < len; i++) {
        char ch = name_buf[i];
        if (ch < 0x20 || ch > 0x7E || ch == '/' || ch == '\\' || ch == ':' ||
            ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' ||
            ch == '|') {
            name_buf[i] = '_';
        }
    }
    // 末尾のスペースとアンダースコアを削除
    for (int i = len - 1; i >= 0; i--) {
        if (name_buf[i] == ' ' || name_buf[i] == '_') {
            name_buf[i] = '\0';
        } else {
            break;
        }
    }

    if (strlen(name_buf) == 0) {
        return;
    }
    String new_name = String(name_buf);
    // 重複チェック
    if (names == new_name || names.startsWith(new_name + "_") ||
        names.endsWith("_" + new_name) ||
        names.indexOf("_" + new_name + "_") != -1) {
        return;
    }
    if (names.length() > 0) {
        names += "_";
    }
    names += new_name;
}

/**
 * @brief T77変換時にヘッダブロックから拾ったファイル名を連結する
 */
static String names_from_t77_headers() {
    String names = "";
    const T77HeaderNames& found = t77_last_header_names();
    for (int i = 0; i < found.count; i++) {
        add_extracted_name(names, found.name[i], T77_NAME_LEN);
    }
    return names;
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
    int match_ea_count = 0;
    int extract_count = 0;
    char name_buf[7] = {0};
    uint8_t cnt = 0;

    while (f.available() && cnt < 100) {
        uint8_t c = f.read();

        // 名前抽出中
        if (extract_count > 0) {
            name_buf[6 - extract_count] = (char)c;
            extract_count--;
            if (extract_count == 0) {
                add_extracted_name(names, name_buf, 6);
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

        // パターン3: D0 x 10以上 に続く6バイト
        if (c == 0xEA) {
            match_ea_count++;
            match_p2_state = 0;
        } else {
            if (match_ea_count >= 10) {
                name_buf[0] = (char)c;
                extract_count = 5;  // 既に1文字読んだので残り5バイト
                match_ea_count = 0;
                match_p2_state = 0;
                continue;
            }
            match_ea_count = 0;
        }
        cnt++;
    }
    f.close();
    return names;
}

/**
 * @brief 既存のtarget_filenameの末尾に抽出した名前を付加してリネームする
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
 * @brief ファイル(path)を抽出した名前にリネームし、pathを新しい名前にする
 * (同じ名前があれば(1)〜(99)を付ける)
 */
static void rename_file_with_extracted_names(char* path, size_t path_size,
                                             const String& names) {
    if (names.length() == 0) {
        return;
    }

    String current(path);
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
        if (SD.rename(path, new_name.c_str())) {
            snprintf(path, path_size, "%s", new_name.c_str());
        }
    }
}

/**
 * @brief デフォルトのtarget_filenameを抽出した名前にリネームする
 */
static void rename_target_filename_with_extracted_names(const String& names) {
    rename_file_with_extracted_names(target_filename, sizeof(target_filename), names);
}

/**
 * @brief 追記先ファイルの拡張子から、追記するデータの形式を決める
 * (追記できるのは T88 / T77 / CAS / P6T)
 */
static bool append_format_from_filename(const char* path, RecFormat* out) {
    const char* ext = strrchr(path, '.');
    if (ext == nullptr) {
        return false;
    }
    if (strcasecmp(ext, ".T88") == 0) {
        *out = FMT_T88;
    } else if (strcasecmp(ext, ".T77") == 0) {
        *out = FMT_T77;
    } else if (strcasecmp(ext, ".CAS") == 0) {
        *out = FMT_CAS;
    } else if (strcasecmp(ext, ".P6T") == 0) {
        *out = FMT_P6T;
    } else {
        return false;
    }
    return true;
}

bool is_appendable_filename(const char* path) {
    RecFormat fmt;
    return path != nullptr && append_format_from_filename(path, &fmt);
}

/**
 * @brief RAWファイルを変換し、既存のファイルに安全に追記・結合する
 */
static bool append_converted_raw_to_file(const char* target_path,
                                         const char* raw_path,
                                         RecFormat format) {
    std::function<bool(const char*, const char*)> converter;
    const char* temp_path = nullptr;

    switch (format) {
        case FMT_T88:
            converter = convert_raw_to_t88;
            temp_path = "/APPEND.T88";
            break;
        case FMT_CAS:
            converter = convert_raw_to_cas;
            temp_path = "/APPEND.CAS";
            break;
        case FMT_P6:
            converter = convert_raw_to_cmt;
            temp_path = "/APPEND.P6";
            break;
        case FMT_T77:
            converter = convert_raw_to_t77;
            temp_path = "/APPEND.T77";
            break;
        case FMT_P6T:
            converter = convert_raw_to_p6t;
            temp_path = "/APPEND.P6T";
            break;
        default:
            return false;
    }

    // 1. RAWを一時ファイルに変換
    if (!converter(raw_path, temp_path)) {
        if (SD.exists(temp_path)) {
            SD.remove(temp_path);
        }
        return false;
    }

    // T77は波形データなので、変換時にヘッダブロックから拾った名前を使う
    String extracted = (format == FMT_T77) ? names_from_t77_headers()
                                           : extract_names_from_file(temp_path);

    File old_file = SD.open(target_path, FILE_READ);
    File new_part_file = SD.open(temp_path, FILE_READ);

    const char* merge_path = "/APPEND.MRG";
    if (SD.exists(merge_path)) {
        SD.remove(merge_path);
    }
    File merge_file = SD.open(merge_path, FILE_WRITE);

    bool success = false;

    if (old_file && new_part_file && merge_file) {
        if (format == FMT_P6T || format == FMT_T88) {
            // P6T はベタイメージとフッタのブロック情報を、T88 はタグを結合する
            // (ファイル名で開き直す)
            old_file.close();
            new_part_file.close();
            merge_file.close();
            success = (format == FMT_P6T) ? p6t_append(target_path, temp_path, merge_path)
                                          : t88_append(target_path, temp_path, merge_path);
        } else if (format == FMT_T77) {
            // T77用のマージ処理（ヘッダの二重化を防ぐ）
            // 既存ファイルをヘッダごとコピーし、追加分はヘッダとその直後の 0x0000 を
            // 除いたデータ部を結合する (境界にカウント0のワードは入れない)
            uint8_t header[T77_HEADER_LEN];
            if (old_file.read(header, T77_HEADER_LEN) == T77_HEADER_LEN &&
                memcmp(header, T77_HEADER_STR, T77_HEADER_LEN) == 0) {
                success =
                    copy_file_contents(merge_file, old_file) &&
                    copy_file_contents(merge_file, new_part_file, T77_DATA_OFFSET);
            }

        } else {
            // CMT / CAS / P6 等の単純連結
            if (copy_file_contents(merge_file, old_file)) {
                success = copy_file_contents(merge_file, new_part_file);
            }
        }
        if (merge_file) {  // P6T / T88 は閉じてある
            merge_file.flush();
        }
    }

    if (old_file) {
        old_file.close();
    }
    if (new_part_file) {
        new_part_file.close();
    }
    if (merge_file) {
        merge_file.close();
    }

    // SDのキャッシュフラッシュを確実に待つ
    delay(50);

    if (success) {
        SD.remove(target_path);
        SD.rename(merge_path, target_path);

        if (extracted.length() > 0) {
            append_names_to_target_filename(extracted);
        }
    } else {
        // 失敗時はマージ用ファイルを消すだけ
        SD.remove(merge_path);
    }

    SD.remove(temp_path);
    return success;
}

// 形式ごとのRAWからの変換関数(RAWはnullptr)
static std::function<bool(const char*, const char*)> converter_for_format(RecFormat fmt) {
    switch (fmt) {
        case FMT_T88:
            return convert_raw_to_t88;
        case FMT_CMT:
        case FMT_P6:
            return convert_raw_to_cmt;
        case FMT_CAS:
            return convert_raw_to_cas;
        case FMT_T77:
            return convert_raw_to_t77;
        case FMT_P6T:
            return convert_raw_to_p6t;
        default:
            return nullptr;
    }
}

// 録音したRAWをそのまま別の名前で残す(RAW_TEMP.BINは最後の録音として残す)
static bool copy_raw_to_file(const char* src_path, const char* dst_path) {
    File src = SD.open(src_path, FILE_READ);
    if (!src) {
        return false;
    }
    if (SD.exists(dst_path)) {
        SD.remove(dst_path);
    }
    File dst = SD.open(dst_path, FILE_WRITE);
    if (!dst) {
        src.close();
        return false;
    }
    uint32_t total = src.size();
    uint32_t done = 0;
    bool ok = true;
    // 録音が終わって空いている書き込み用のバッファを使う
    uint8_t* buf = (uint8_t*)rec_sector_buffer;
    const size_t buf_size = sizeof(rec_sector_buffer);
    int last_percent = -1;
    while (ok && done < total) {
        watchdog_update();
        int n = src.read(buf, buf_size);
        if (n <= 0) {
            ok = false;
            break;
        }
        if (dst.write(buf, n) != (size_t)n) {
            ok = false;
            break;
        }
        done += n;
        int percent = (int)((uint64_t)done * 100 / total);
        if (percent != last_percent) {
            draw_conversion_progress(percent);
            last_percent = percent;
        }
    }
    src.close();
    dst.close();
    if (!ok) {
        SD.remove(dst_path);
    }
    return ok;
}

bool start_raw_recording() {
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

    // 録音中に値を取りこぼした回数 (0 でなければ RAW の High/Low の並びが崩れている)
    log_core1_rec_stats();

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

    flush_rec_buffer();
    if (raw_rec_file) {
        raw_rec_file.close();
    }

    draw_conversion_progress(0);
    bool success = false;

    if (selected_rec_mode == MODE_APPEND) {
        // 追記するデータは、設定の Rec Fmt ではなく追記先と同じ形式で変換する
        RecFormat append_fmt;
        if (append_format_from_filename(target_filename, &append_fmt)) {
            success = append_converted_raw_to_file(target_filename, raw_rec_path,
                                                   append_fmt);
        }
    } else {
        std::function<bool(const char*, const char*)> converter =
            converter_for_format(default_rec_fmt);
        if (default_rec_fmt == FMT_RAW) {
            // 変換せず、録音したままの波形を残す
            success = copy_raw_to_file(raw_rec_path, target_filename);
        } else if (converter) {
            success = converter(raw_rec_path, target_filename);
            if (success) {
                // 新規作成の場合、変換後に抽出とリネームを実行
                String extracted =
                    (default_rec_fmt == FMT_T77)
                        ? names_from_t77_headers()
                        : extract_names_from_file(target_filename);
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
            if (pulse_ticks >= REC_VALID_PULSE_MIN_TICKS &&
                pulse_ticks <= REC_VALID_PULSE_MAX_TICKS) {
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

// ============================================================================
// RAW/WAVからの変換(ファイル一覧の変換メニュー)
// ============================================================================
#define WAV_TEMP_RAW_PATH "/WAV_TEMP.BIN"

bool is_convertible_source(const char* path) {
    const char* ext = (path != nullptr) ? strrchr(path, '.') : nullptr;
    return ext != nullptr && (strcasecmp(ext, ".RAW") == 0 || strcasecmp(ext, ".WAV") == 0);
}

bool convert_file_to_format(const char* src_path, RecFormat fmt, char* out_path,
                            size_t out_size) {
    static const char* const ext_list[] = {".T88", ".CMT", ".CAS", ".P6", ".T77", ".P6T", ".RAW"};
    static_assert(sizeof(ext_list) / sizeof(ext_list[0]) == FMT_MAX, "ext_list");
    std::function<bool(const char*, const char*)> converter = converter_for_format(fmt);
    if (!is_convertible_source(src_path) || !converter || out_path == nullptr || out_size == 0) {
        return false;
    }

    // 書き出す名前:元のファイルの拡張子を形式のものに替える。あれば_2, _3 ...を付ける
    String base = String(src_path);
    int dot = base.lastIndexOf('.');
    if (dot > 0) {
        base = base.substring(0, dot);
    }
    String candidate = base + ext_list[fmt];
    for (int i = 2; SD.exists(candidate.c_str()) && i <= 99; i++) {
        candidate = base + "_" + String(i) + ext_list[fmt];
    }
    if (SD.exists(candidate.c_str())) {
        return false;
    }
    snprintf(out_path, out_size, "%s", candidate.c_str());

    const char* ext = strrchr(src_path, '.');
    bool ok;
    if (strcasecmp(ext, ".WAV") == 0) {
        // WAVは一度RAWにしてから変換する
        ok = convert_wav_to_raw(src_path, WAV_TEMP_RAW_PATH) &&
             converter(WAV_TEMP_RAW_PATH, out_path);
        SD.remove(WAV_TEMP_RAW_PATH);
    } else {
        ok = converter(src_path, out_path);
    }
    if (!ok) {
        if (SD.exists(out_path)) {
            SD.remove(out_path);
        }
        return false;
    }
    // 録音と同じく、テープの中の名前(ヘッダのファイル名)が見つかればその名前にする
    String extracted = (fmt == FMT_T77) ? names_from_t77_headers() : extract_names_from_file(out_path);
    if (extracted.length() > 0) {
        rename_file_with_extracted_names(out_path, out_size, extracted);
    }
    return true;
}
