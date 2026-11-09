#pragma once

#include <stdint.h>
#include <string.h>

// ----------------------------------------------------------------------------
// WAVファイルの読み取り。テープの音声を録ったWAVから、波の半周期(レベルと長さ)を取り出す
// ----------------------------------------------------------------------------

// WAVの形式(リニアPCMだけ扱う)
struct WavFormat {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;          // 8/16/24/32
    uint32_t data_offset = 0;   // 音声データの先頭の位置
    uint32_t data_size = 0;     // 音声データのバイト数
    uint32_t block_align = 0;   // 1サンプル(全チャンネル)のバイト数
};

// RIFFのヘッダを読む。read_at(位置,読み先,長さ)は読めたバイト数を返す関数
template <typename ReadAt>
bool wav_parse_header(ReadAt read_at, uint32_t file_size, WavFormat* out) {
    uint8_t h[12];
    if (file_size < 44 || read_at(0, h, 12) != 12 || memcmp(h, "RIFF", 4) != 0 ||
        memcmp(h + 8, "WAVE", 4) != 0) {
        return false;
    }
    auto le16 = [](const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8)); };
    auto le32 = [&](const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); };
    bool have_fmt = false;
    uint32_t pos = 12;
    while (pos + 8 <= file_size) {
        uint8_t c[8];
        if (read_at(pos, c, 8) != 8) {
            return false;
        }
        uint32_t len = le32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0) {
            uint8_t f[40];
            uint32_t n = (len < sizeof(f)) ? len : sizeof(f);
            if (n < 16 || read_at(pos + 8, f, n) != (int)n) {
                return false;
            }
            uint32_t tag = le16(f);
            // WAVE_FORMAT_EXTENSIBLEはサブフォーマットの先頭2バイトで判断する
            if (tag == 0xFFFE && n >= 26) {
                tag = le16(f + 24);
            }
            if (tag != 1) {
                return false;  // リニアPCM以外(浮動小数点・圧縮)は扱わない
            }
            out->channels = (uint16_t)le16(f + 2);
            out->sample_rate = le32(f + 4);
            out->block_align = le16(f + 12);
            out->bits = (uint16_t)le16(f + 14);
            have_fmt = true;
        } else if (memcmp(c, "data", 4) == 0) {
            if (!have_fmt) {
                return false;
            }
            out->data_offset = pos + 8;
            uint32_t rest = file_size - out->data_offset;
            out->data_size = (len < rest) ? len : rest;
            break;
        }
        pos += 8 + len + (len & 1);
    }
    if (!have_fmt || out->data_offset == 0 || out->channels == 0 || out->sample_rate < 8000 ||
        (out->bits != 8 && out->bits != 16 && out->bits != 24 && out->bits != 32) ||
        out->block_align < out->channels * (out->bits / 8)) {
        return false;
    }
    return true;
}

// 1サンプル(先頭のチャンネル)を、符号付き16bitの大きさにそろえて取り出す
inline int32_t wav_sample_value(const uint8_t* p, uint16_t bits) {
    switch (bits) {
        case 8:
            return ((int32_t)p[0] - 128) << 8;
        case 16:
            return (int16_t)(p[0] | (p[1] << 8));
        case 24:
            return (int16_t)(p[1] | (p[2] << 8));
        default:
            return (int16_t)(p[2] | (p[3] << 8));
    }
}

// サンプルの並びから、波が0を横切る時刻を求めて半周期に分ける。
// 直流分をハイパスフィルタで除き、振幅に合わせたヒステリシスで細かい揺れを無視する。
// 振幅がそれまでの信号の1/4より小さい間(無音の間の雑音)は、波として拾わない。
// 時刻は1/256サンプル単位(0を横切る点を前後のサンプルで補間する)
class WavEdgeDetector {
   public:
    void begin(uint32_t sample_rate) {
        // ハイパスの効きを約100〜200Hzにする(rate/(2π×2^shift))
        shift_ = 1;
        while (shift_ < 12 && (sample_rate >> (shift_ + 1)) / 6 > 200) {
            shift_++;
        }
        // 振幅は約10msで下がり、信号の大きさは約1.5秒かけて忘れる
        env_shift_ = 1;
        while (env_shift_ < 14 && (1u << (env_shift_ + 1)) <= sample_rate / 100) {
            env_shift_++;
        }
        peak_shift_ = env_shift_ + 7;
        peak_ = 0;
        lp_ = 0;
        env_ = 0;
        prev_ = 0;
        index_ = 0;
        state_ = 0;
        last_zero_q8_ = 0;
    }

    // サンプルを1つ入れる。波のレベルが切り替わったらtrueを返し、
    // *edge_q8に切り替わりの時刻、*level_highに切り替わる前のレベルを入れる
    bool feed(int32_t sample, uint64_t* edge_q8, bool* level_high) {
        int32_t x = sample << 8;
        lp_ += (x - lp_) >> shift_;
        int32_t y = (x - lp_) >> 8;

        // 振幅(包絡線)と信号の大きさを追い、ヒステリシスの幅を振幅の1/4にする
        // (小さすぎる値は雑音とみなす)
        // (どちらも256倍の値で持ち、少しずつ減らす)
        int32_t a = ((y < 0) ? -y : y) << 8;
        if (a > env_) {
            env_ = a;
        } else {
            env_ -= env_ >> env_shift_;
        }
        if (a > peak_) {
            peak_ = a;
        } else {
            peak_ -= peak_ >> peak_shift_;
        }
        int32_t hyst = (env_ >> 8) / 4;
        if (hyst < 64) {
            hyst = 64;
        }
        bool quiet = env_ < peak_ / 4;

        // 0を横切った時刻を補間して覚えておく
        if (index_ > 0 && ((prev_ < 0) != (y < 0))) {
            int32_t d = prev_ - y;
            uint32_t frac = (d != 0) ? (uint32_t)(((int64_t)prev_ * 256) / d) : 0;
            if (frac > 255) {
                frac = 255;
            }
            last_zero_q8_ = ((uint64_t)(index_ - 1) << 8) + frac;
        }
        prev_ = y;
        index_++;

        int new_state = state_;
        if (quiet) {
            // 無音の間は今のレベルのまま
        } else if (y > hyst) {
            new_state = 1;
        } else if (y < -hyst) {
            new_state = -1;
        }
        if (new_state != state_) {
            int old = state_;
            state_ = new_state;
            if (old != 0) {
                *edge_q8 = last_zero_q8_;
                *level_high = (old > 0);
            } else {
                // 最初にレベルが決まったところ。それまでは反対のレベルだったことにする
                *edge_q8 = (index_ - 1) << 8;
                *level_high = (new_state < 0);
            }
            return true;
        }
        return false;
    }

    // これまでに入れたサンプルの数
    uint64_t samples() const { return index_; }
    // いまの波のレベル(まだ決まっていなければ0)
    int state() const { return state_; }

   private:
    int shift_ = 5;
    int env_shift_ = 9;
    int peak_shift_ = 16;
    int32_t lp_ = 0;
    int32_t env_ = 0;
    int32_t peak_ = 0;
    int32_t prev_ = 0;
    uint64_t index_ = 0;
    int state_ = 0;
    uint64_t last_zero_q8_ = 0;
};
