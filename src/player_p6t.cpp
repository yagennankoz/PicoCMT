#include "player_p6t.h"

#include <string.h>

#include "cmt_globals.h"

extern volatile uint32_t playback_base_time_ms;
extern volatile uint32_t playback_tag_start_system_ms;
extern volatile bool playback_time_valid;

// 1tick = 1/4800秒 (2400Hz の半周期)。'1' = 1tick の半周期、'0' = 2tick の半周期
static constexpr uint32_t TICK_HZ = 4800;
static constexpr int STOP_BITS = 3;  // PC-6001 のストップビット
static constexpr int BITS_PER_BYTE = 1 + 8 + STOP_BITS;

// ヘッダとプログラムの間に入れるピー音 (PlayerCMT の P6 と同じ。半周期の数、1秒)
static constexpr uint32_t MID_LEADER_HALVES = 4800;
// 最後のブロックの後のピー音 (半周期の数、0.5秒)
static constexpr uint32_t TRAILER_HALVES = 2400;
static constexpr int HEADER_D3_COUNT = 10;
static constexpr int HEADER_NAME_LEN = 6;
// 頭出し (戻る) で、ブロックの頭に戻るか前のブロックに戻るかの境目
static constexpr uint32_t SEEK_BACK_RESTART_MS = 3000;

// tick 数から再生キューに入れる値 (us) を求める (再生PIOの命令ぶんを引く)。
// 1tick (208.33us) を毎回丸めると長いピー音で誤差が積み上がるので、
// 出した tick の合計から時間を決めて丸める
uint16_t PlayerP6T::ticks_to_queue_us(uint32_t ticks, bool high) {
    uint64_t target = ((total_ticks + ticks) * 1000000ULL + TICK_HZ / 2) / TICK_HZ;
    uint64_t start = (total_ticks * 1000000ULL + TICK_HZ / 2) / TICK_HZ;
    uint32_t us = (uint32_t)(target - start);
    uint32_t overhead = high ? PLAY_PIO_OVERHEAD_HIGH_US : PLAY_PIO_OVERHEAD_LOW_US;
    return (uint16_t)((us > overhead) ? us - overhead : 0);
}

PlayerP6T::PlayerP6T() {
    cmt_file = nullptr;
    reader = nullptr;
    end_reached = true;
    exhausted = true;
    current_pos = 0;
}

PlayerP6T::~PlayerP6T() {
    if (reader) {
        delete reader;
    }
}

void PlayerP6T::set_log(const char* text) {
    snprintf(playback_log, sizeof(playback_log), "%s", text);
}

bool PlayerP6T::init(File* f, const char* filename) {
    (void)filename;
    cmt_file = f;
    if (!cmt_file || cmt_file->size() == 0) {
        return false;
    }
    if (!p6t_read_info(*cmt_file, p6t) || p6t.blocks.empty()) {
        return false;
    }
    if (reader) {
        delete reader;
    }
    reader = new SdBufferedRead(cmt_file, 4096);

    init_position_tracking();
    end_reached = false;
    exhausted = false;
    start_block(0);
    set_log(p6t.has_footer ? "P6T Init" : "P6T(P6) Init");
    return true;
}

// ブロックの再生を最初 (無音) から始める
void PlayerP6T::start_block(int index) {
    block_index = index;
    state = STATE_BLOCK_START;
    bit_index = -1;
    bit_active = false;
    d3_run = 0;
    name_left = -1;
    mid_leader_pending = false;
    silence_started = false;
    block_started_ms = millis();
    total_ticks = 0;
}

bool PlayerP6T::enqueue(queue_t* q, uint16_t us) {
    if (queue_get_level(q) >= PLAY_QUEUE_LENGTH) {
        return false;
    }
    if (!queue_try_add(q, &us)) {
        return false;
    }
    total_enqueued_pulses++;
    return true;
}

// ブロックの先頭までの再生時間 (時間表示用)
uint32_t PlayerP6T::block_start_time_ms(int index) const {
    uint64_t ms = 0;
    for (int i = 0; i < index && i < (int)p6t.blocks.size(); i++) {
        const P6TBlock& b = p6t.blocks[i];
        ms += b.stime_ms + b.ptime_ms;
        ms += (uint64_t)b.size * BITS_PER_BYTE * 1000ULL / b.baud;
    }
    return (uint32_t)ms;
}

// データを1半周期ぶん出す。ブロックのデータを出し終えたら false
bool PlayerP6T::emit_data(queue_t* q) {
    if (!bit_active) {
        if (bit_index < 0) {
            if (mid_leader_pending) {
                // ヘッダの後にプログラムが続くので、間にピー音を入れる
                mid_leader_pending = false;
                state = STATE_LEADER;
                halves_remaining = MID_LEADER_HALVES;
                return true;
            }
            if (block_remaining == 0 || !reader->read_byte(current_byte)) {
                return false;
            }
            block_remaining--;

            // ヘッダ (D3 x10 + ファイル名6バイト) の直後にデータが残っていればピー音を入れる
            if (name_left > 0) {
                if (--name_left == 0) {
                    name_left = -1;
                    mid_leader_pending = (block_remaining > 0);
                }
            } else {
                d3_run = (current_byte == 0xD3) ? d3_run + 1 : 0;
                if (d3_run == HEADER_D3_COUNT) {
                    d3_run = 0;
                    name_left = HEADER_NAME_LEN;
                }
            }
            bit_index = 0;
        }
        if (bit_index == 0) {
            bit_value = 0;  // スタートビット
        } else if (bit_index <= 8) {
            bit_value = (current_byte >> (bit_index - 1)) & 1;  // LSB ファースト
        } else {
            bit_value = 1;  // ストップビット
        }
        bit_ticks_remaining = (block_baud == 600) ? 8 : 4;
        bit_active = true;
    }

    uint32_t half_ticks = bit_value ? 1 : 2;
    uint32_t emit = (bit_ticks_remaining < half_ticks) ? bit_ticks_remaining : half_ticks;
    // PIO は値ごとに High/Low を反転する。どのビットも半周期の数が偶数なので、
    // 半周期の位置 (tick の残りから分かる) で High/Low が決まる
    bool high = (((bit_ticks_remaining / half_ticks) % 2) == 0);
    if (!enqueue(q, ticks_to_queue_us(emit, high))) {
        return true;  // キューが一杯。次回に続ける
    }
    bit_ticks_remaining -= emit;
    total_ticks += emit;
    if (bit_ticks_remaining == 0) {
        bit_active = false;
        if (++bit_index >= BITS_PER_BYTE) {
            bit_index = -1;
        }
    }
    return true;
}

void PlayerP6T::process(queue_t* q) {
    if (end_reached) {
        exhausted = true;
        return;
    }

    int iterations = 0;
    while (queue_get_level(q) < PLAY_QUEUE_LENGTH) {
        if (++iterations > 512) {
            break;
        }
        switch (state) {
            case STATE_BLOCK_START: {
                if (block_index >= (int)p6t.blocks.size()) {
                    state = STATE_TRAILER;
                    halves_remaining = TRAILER_HALVES;
                    set_log("P6T Trailer");
                    break;
                }
                const P6TBlock& b = p6t.blocks[block_index];
                reader->seek(b.offset);
                block_remaining = b.size;
                block_baud = b.baud;
                halves_remaining = ((uint32_t)b.ptime_ms * TICK_HZ / 1000) & ~1u;  // 偶数にする
                if (b.stime_ms > 0) {
                    // 無音 (Core1 がキューの最後の値を2つずつ繰り返す)。半周期の数は
                    // いつも偶数なので、2つ入れれば無音の後も High から始まる。
                    // 2つとも入る空きができるまで待つ (1つだけだと位相が入れ替わる)
                    if (PLAY_QUEUE_LENGTH - queue_get_level(q) < 2) {
                        return;
                    }
                    enqueue(q, 2);
                    enqueue(q, 2);
                    silence_started = false;
                    state = STATE_SILENCE;
                    set_log("P6T Blank");
                } else {
                    state = STATE_LEADER;
                    set_log("P6T Leader");
                }
                break;
            }

            case STATE_SILENCE: {
                // 無音は、キューに残っている信号を出し切ってから計り始める
                if (!silence_started) {
                    if (!queue_is_empty(q)) {
                        return;
                    }
                    silence_started = true;
                    silence_start_ms = millis();
                }
                if (millis() - silence_start_ms < p6t.blocks[block_index].stime_ms) {
                    return;
                }
                state = STATE_LEADER;
                set_log("P6T Leader");
                break;
            }

            case STATE_LEADER:
            case STATE_TRAILER: {
                if (halves_remaining == 0) {
                    if (state == STATE_TRAILER) {
                        state = STATE_END;
                    } else {
                        state = STATE_DATA;
                        set_log("P6T Data");
                    }
                    break;
                }
                bool high = (halves_remaining % 2) == 0;
                if (!enqueue(q, ticks_to_queue_us(1, high))) {
                    return;
                }
                total_ticks++;
                halves_remaining--;
                break;
            }

            case STATE_DATA:
                if (!emit_data(q)) {
                    start_block(block_index + 1);
                }
                break;

            case STATE_END:
                end_reached = true;
                return;
        }
    }

    // 表示用のファイル位置 (ベタイメージの中の位置)
    if (block_index < (int)p6t.blocks.size()) {
        const P6TBlock& b = p6t.blocks[block_index];
        current_pos = b.offset + (b.size - block_remaining);
    } else {
        current_pos = p6t.data_size;
    }
}

bool PlayerP6T::seek(int direction, queue_t* play_queue) {
    (void)play_queue;
    int n = (int)p6t.blocks.size();
    int target;
    if (direction < 0) {
        // 再生を始めてすぐならひとつ前のブロック、そうでなければ今のブロックの頭へ
        bool just_started = (millis() - block_started_ms < SEEK_BACK_RESTART_MS);
        target = (block_index >= n) ? n - 1 : block_index;
        if (just_started && target > 0) {
            target--;
        }
    } else {
        target = block_index + 1;
        if (target >= n) {
            return false;
        }
    }
    if (target < 0) {
        target = 0;
    }
    // 頭出しのたびに Core1 と PIO はやり直すので、半周期の偶奇も最初からになる
    end_reached = false;
    exhausted = false;
    start_block(target);

    playback_base_time_ms = block_start_time_ms(target);
    playback_tag_start_system_ms = millis();
    playback_time_valid = true;
    set_log("P6T Seek");
    return true;
}

void PlayerP6T::stop() {}

bool PlayerP6T::is_end_reached() { return end_reached; }
bool PlayerP6T::is_exhausted() { return exhausted; }

void PlayerP6T::get_status(PlaybackStatus* out) {
    out->playback_t88_mode = false;
    out->playback_t88_end_reached = end_reached;
    out->playback_source_exhausted = exhausted;
    out->file_position = current_pos;
    out->file_size = p6t.data_size;
    snprintf(out->playback_log, sizeof(out->playback_log), "%s", playback_log);
}
