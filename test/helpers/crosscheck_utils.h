// ============================================================================
// 公開ツールとのクロスチェック用ユーティリティ (tools/crosscheck.py と組み合わせて使う)
//   - XCHECK_OUT_DIR : PicoCmt が作ったファイル (テープイメージ・再生の WAV) の書き出し先
//   - XCHECK_IN_DIR  : 公開ツールが作ったファイル (WAV など) の読み込み元
//   どちらも環境変数が無ければ何もしない (普段の pio test には影響しない)
//   - tools/external/ : tools/fetch_tools.py で取得した公開ツールのデータ
// ============================================================================
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "tape_test_utils.h"

// tools/external/<pkg>/<sub>/<name>
inline std::string external_path(const char* pkg, const char* sub, const char* name) {
    std::string f = __FILE__;
    size_t p = f.find_last_of("/\\");
    std::string dir = (p == std::string::npos) ? std::string(".") : f.substr(0, p);
    return dir + "/../../tools/external/" + pkg + "/" + sub + "/" + name;
}

inline bool read_host_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return false;
    }
    out.clear();
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    fclose(fp);
    return true;
}

inline std::string xcheck_dir(const char* env_name) {
    const char* dir = getenv(env_name);
    return (dir != nullptr) ? std::string(dir) : std::string();
}

// PicoCmt が作ったファイルを XCHECK_OUT_DIR に書き出す
inline void xcheck_dump(const char* name, const std::vector<uint8_t>& d) {
    std::string dir = xcheck_dir("XCHECK_OUT_DIR");
    if (dir.empty()) {
        return;
    }
    std::string path = dir + "/" + name;
    FILE* fp = fopen(path.c_str(), "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path.c_str());
    fwrite(d.data(), 1, d.size(), fp);
    fclose(fp);
}

// 公開ツールが作ったファイルを XCHECK_IN_DIR から読む (無ければ false)
inline bool xcheck_input(const char* name, std::vector<uint8_t>& out) {
    std::string dir = xcheck_dir("XCHECK_IN_DIR");
    return !dir.empty() && read_host_file(dir + "/" + name, out);
}

// 波形を 8bit モノラルの WAV にする (High = 0xC0, Low = 0x40)
// silence_us 以上続くレベルは無音 (0x80) にする。実機の出力はコンデンサで直流分が切れ、
// 長く続くレベルは中心に戻るため (公開ツールの多くは無音でブロックを区切る)
inline std::vector<uint8_t> wave_to_wav(const std::vector<Half>& wave, uint32_t rate = 44100,
                                        double silence_us = 5000) {
    std::vector<uint8_t> pcm;
    double t = 0, next = 0;
    for (const Half& h : wave) {
        next += h.us;
        uint8_t v = (h.us >= silence_us) ? 0x80 : (h.high ? 0xC0 : 0x40);
        while (t < next) {
            pcm.push_back(v);
            t += 1e6 / rate;
        }
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
    u32(36 + (uint32_t)pcm.size());
    str("WAVE");
    str("fmt ");
    u32(16);
    u16(1);  // PCM
    u16(1);  // モノラル
    u32(rate);
    u32(rate);
    u16(1);
    u16(8);
    str("data");
    u32((uint32_t)pcm.size());
    d.insert(d.end(), pcm.begin(), pcm.end());
    return d;
}

// 8bit モノラルの WAV を、コンパレータ (中心 0x80、ヒステリシス ±hyst) に通した波形にする
inline std::vector<Half> wav_to_halves(const std::vector<uint8_t>& d, int hyst = 2) {
    TEST_ASSERT_TRUE(d.size() > 44);
    TEST_ASSERT_EQUAL_MEMORY("RIFF", d.data(), 4);
    TEST_ASSERT_EQUAL_MEMORY("WAVE", d.data() + 8, 4);
    auto le16 = [&](size_t p) { return (uint32_t)(d[p] | (d[p + 1] << 8)); };
    auto le32 = [&](size_t p) { return le16(p) | (le16(p + 2) << 16); };
    uint32_t rate = 0, bits = 0, channels = 0;
    size_t data_pos = 0, data_len = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        uint32_t len = le32(p + 4);
        if (memcmp(d.data() + p, "fmt ", 4) == 0) {
            channels = le16(p + 10);
            rate = le32(p + 12);
            bits = le16(p + 22);
        } else if (memcmp(d.data() + p, "data", 4) == 0) {
            data_pos = p + 8;
            data_len = std::min((size_t)len, d.size() - data_pos);
            break;
        }
        p += 8 + len + (len & 1);
    }
    TEST_ASSERT_EQUAL_UINT32(1, channels);
    TEST_ASSERT_EQUAL_UINT32(8, bits);
    TEST_ASSERT_TRUE(rate > 0 && data_len > 0);

    const int center = 0x80;
    double sample_us = 1e6 / rate;
    std::vector<Half> out;
    bool level = false;
    double last_edge = 0;
    for (size_t i = 1; i < data_len; i++) {
        int v = d[data_pos + i];
        bool flip = level ? (v <= center - hyst) : (v >= center + hyst);
        if (!flip) {
            continue;
        }
        // 中心を横切った位置を前のサンプルとの間で補間する
        int prev = d[data_pos + i - 1];
        double frac = (prev == v) ? 1.0 : (double)(center - prev) / (double)(v - prev);
        frac = std::max(0.0, std::min(1.0, frac));
        double edge = ((double)(i - 1) + frac) * sample_us;
        out.push_back({level, edge - last_edge});
        last_edge = edge;
        level = !level;
    }
    out.push_back({level, data_len * sample_us - last_edge});
    return out;
}
