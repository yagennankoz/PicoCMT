#pragma once

#include <stdint.h>

#include <functional>

// ============================================================================
// FM-7 のカセット信号の復調器
//   1ビット = 1周期 (半周期2つ)。0 = 短い周期 (約2400Hz)、1 = 長い周期 (約1200Hz)
//   1バイト = スタートビット(0) + 8bit (LSBファースト) + ストップビット(1) x2
//   (XM7 の周辺ツール t772wav / wav2t77 / t77dec と、XM7 で SAVE した T77 で確認)
//
//   半周期を短い/長いに分け、同じ種類の半周期が2つ続いたら1ビットとする。
//   種類の違う半周期が並んだら、周期の区切りがずれているので1つ捨てて合わせ直す。
//   どちらの半周期から1周期が始まるかを決めないので、信号の極性が逆でも読める
// ============================================================================
class Fm7Demodulator {
   public:
    // 半周期の長さの判定 (us)。XM7 の出力は 短い 約230us / 長い 約440us、
    // 実機の公称値は 208us / 417us
    static constexpr uint32_t HALF_MIN_US = 120;
    static constexpr uint32_t HALF_THRESHOLD_US = 320;
    static constexpr uint32_t HALF_MAX_US = 700;

    std::function<void(uint8_t)> on_byte_decoded = nullptr;

    void reset() {
        pending = HALF_NONE;
        state = WAIT_START;
    }

    // 半周期を1つ入れる (レベルは見ない)
    void feed_half(uint32_t us) {
        if (us < HALF_MIN_US || us > HALF_MAX_US) {
            reset();  // 信号ではない (ノイズ・無音)
            return;
        }
        HalfKind kind = (us < HALF_THRESHOLD_US) ? HALF_SHORT : HALF_LONG;
        if (pending == kind) {
            pending = HALF_NONE;
            on_bit(kind == HALF_LONG ? 1 : 0);
        } else {
            pending = kind;  // 1つ目、または区切りのずれを合わせ直す
        }
    }

   private:
    enum HalfKind { HALF_NONE, HALF_SHORT, HALF_LONG };
    enum State { WAIT_START, DATA, STOP };

    HalfKind pending = HALF_NONE;
    State state = WAIT_START;
    int bit_count = 0;
    uint8_t current = 0;

    void on_bit(int b) {
        switch (state) {
            case WAIT_START:  // 1 (ストップビット・リーダー) の後の 0 を待つ
                if (b == 0) {
                    state = DATA;
                    bit_count = 0;
                    current = 0;
                }
                break;
            case DATA:
                current |= (uint8_t)(b << bit_count);
                if (++bit_count >= 8) {
                    state = STOP;
                }
                break;
            case STOP:
                // 1つ目のストップビットが 1 でなければフレームがずれている
                if (b == 1 && on_byte_decoded) {
                    on_byte_decoded(current);
                }
                state = WAIT_START;
                break;
        }
    }
};
