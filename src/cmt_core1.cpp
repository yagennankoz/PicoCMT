#include <Arduino.h>
#include <hardware/gpio.h>

#include "cmt_globals.h"
#include "cmt_hw_config.h"
#include "cmt_pins.h"
#include "cmt_ui.h"
#include "generated/cmt_play.pio.h"
#include "generated/cmt_rec.pio.h"
#include "hardware/pio.h"
#include "pico/util/queue.h"

// 再生側ステート管理
static bool play_session_initialized = false;
static uint16_t play_last_t88_data = 0xFFFF;
static volatile uint32_t play_swap_count = 0;
static bool play_pio_running = false;
static volatile bool play_shutdown_requested = false;
static uint pio_play_program_offset = 0;

// 録音側ステート管理
static bool rec_pio_running = false;
static bool rec_buzzer_enabled = false;
// 録音中のブザーは入力の波をそのまま鳴らす(録音PIOのサイドセット)。テープの信号の周波数
// (半周期がこの範囲)でない間は止める。ピー音が終わった後、モーターが止まりかけて
// 低くなっていく音などを鳴らさないため。範囲内の半周期がREC_BUZZER_RESUME_HALVES続いたら鳴らす
#define REC_BUZZER_MIN_HALF_US 60    // MSXの2400ボー(4800Hz)の半周期約104usより短いもの
#define REC_BUZZER_MAX_HALF_US 600   // 1200Hzの半周期約417usより十分長いもの
#define REC_BUZZER_RESUME_HALVES 16
static int rec_buzzer_good_halves = 0;

static void rec_buzzer_mute() {
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_SIO);
    gpio_put(BUZZER_PIN, 0);
    rec_buzzer_enabled = false;
}
// 録音で値を取りこぼした回数 (PIO の FIFO があふれた / Core0 へのキューが一杯)
static volatile uint32_t rec_fifo_overflow_count = 0;
static volatile uint32_t rec_queue_drop_count = 0;
// High / Low が交互でなかった回数 (0 でなければ PIO の測定で値が抜けている)
static volatile uint32_t rec_same_level_count = 0;
static volatile uint32_t rec_filtered_count = 0;  // 短すぎ・長すぎで捨てた値の数
static int rec_prev_level = -1;

// ============================================================================
// 再生(PLAY) 系関数群
// ============================================================================

static void init_cmt_play_pio() {
    PIO pio = pio0;
    pio_play_program_offset = pio_add_program(pio, &cmt_play_program);
    pio_sm_config c =
        cmt_play_program_get_default_config(pio_play_program_offset);

    sm_config_set_out_pins(&c, PICO_OUT_PIN, 1);
    sm_config_set_set_pins(&c, PICO_OUT_PIN, 1);
    sm_config_set_in_pins(&c, PICO_OUT_PIN);
    sm_config_set_sideset_pins(&c, BUZZER_PIN);
    sm_config_set_sideset(&c, 1, false, false);

    pio_gpio_init(pio, PICO_OUT_PIN);
    pio_gpio_init(pio, BUZZER_PIN);

    pio_sm_set_consecutive_pindirs(pio, pio_play_sm, PICO_OUT_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, pio_play_sm, BUZZER_PIN, 1, true);

    gpio_set_drive_strength(PICO_OUT_PIN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(BUZZER_PIN, GPIO_DRIVE_STRENGTH_8MA);

    float div = (float)clock_get_hz(clk_sys) / 1000000.0f;  // 1MHz駆動(1us単位)
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(pio, pio_play_sm, pio_play_program_offset, &c);
    pio_sm_set_enabled(pio, pio_play_sm, false);
    play_pio_running = false;
}

static void stop_cmt_play_pio() {
    if (play_pio_running) {
        pio_sm_set_enabled(pio0, pio_play_sm, false);

        gpio_set_function(PICO_OUT_PIN, GPIO_FUNC_SIO);
        gpio_put(PICO_OUT_PIN, 0);
        gpio_set_function(BUZZER_PIN, GPIO_FUNC_SIO);
        gpio_put(BUZZER_PIN, 0);
    }

    play_pio_running = false;
    play_session_initialized = false;
    play_last_t88_data = 0xFFFF;
    play_swap_count = 0;
    play_shutdown_requested = false;
}

static void start_cmt_play_pio() {
    if (play_pio_running) {
        return;
    }
    play_shutdown_requested = false;

    pio_gpio_init(pio0, PICO_OUT_PIN);
    pio_gpio_init(pio0, BUZZER_PIN);

    pio_sm_clear_fifos(pio0, pio_play_sm);

    pio_sm_set_enabled(pio0, pio_play_sm, true);
    play_pio_running = true;
}

// キューから1つ取り出してPIOへ送る。キューが空なら前回値を2つ送る
// (PIOは値ごとにH/Lを反転するので、繰り返しを偶数回にして極性を保つ)
// FIFOに2つ分の空きがなく送れなかった場合は false
static bool feed_play_fifo(PIO pio) {
    uint16_t t88_data;
    if (queue_try_remove(&cmt_play_queue, &t88_data)) {
        play_last_t88_data = t88_data;
        pio_sm_put(pio, pio_play_sm, (uint32_t)t88_data);
        return true;
    }
    if (pio_sm_get_tx_fifo_level(pio, pio_play_sm) > 2) {
        return false;
    }
    pio_sm_put(pio, pio_play_sm, (uint32_t)play_last_t88_data);
    pio_sm_put(pio, pio_play_sm, (uint32_t)play_last_t88_data);
    return true;
}

void request_playback_shutdown() {
    play_shutdown_requested = true;

    gpio_set_function(PICO_OUT_PIN, GPIO_FUNC_SIO);
    gpio_put(PICO_OUT_PIN, 0);
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_SIO);
    gpio_put(BUZZER_PIN, 0);
}

void reset_core1_playback_buffers() {
    PIO pio = pio0;

    pio_sm_set_enabled(pio, pio_play_sm, false);
    play_pio_running = false;

    pio_sm_clear_fifos(pio, pio_play_sm);
    pio_sm_restart(pio, pio_play_sm);
    pio_sm_clkdiv_restart(pio, pio_play_sm);
    pio_sm_exec(pio, pio_play_sm, pio_encode_jmp(pio_play_program_offset));
    pio_sm_exec(pio, pio_play_sm, pio_encode_set(pio_x, 0));

    gpio_set_function(PICO_OUT_PIN, GPIO_FUNC_SIO);
    gpio_put(PICO_OUT_PIN, 0);
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_SIO);
    gpio_put(BUZZER_PIN, 0);

    uint16_t dummy_data;
    while (queue_try_remove(&cmt_play_queue, &dummy_data)) {
        asm volatile("" : : : "memory");
    }

    play_session_initialized = false;
    play_swap_count = 0;
    play_last_t88_data = 0xFFFF;
}

void init_core1_playback_prime() {
    PIO pio = pio0;

    // FIFOを事前にある程度満たしておく
    while (!pio_sm_is_tx_fifo_full(pio, pio_play_sm)) {
        if (!feed_play_fifo(pio)) {
            break;
        }
    }

    play_session_initialized = true;
    play_swap_count = 1;

    if (play_active && playback_permitted) {
        pio_sm_set_enabled(pio, pio_play_sm, true);
        play_pio_running = true;
    } else {
        play_pio_running = false;
    }
}

static void core1_play_loop() {
    if (play_shutdown_requested) {
        gpio_put(PICO_OUT_PIN, 0);
        gpio_put(BUZZER_PIN, 0);
        return;
    }

    if (!play_session_initialized) {
        while (!pio_sm_is_tx_fifo_full(pio0, pio_play_sm)) {
            if (!feed_play_fifo(pio0)) {
                break;
            }
        }
        play_session_initialized = true;
        play_swap_count = 0;
        return;
    }

    if (!play_active) {
        gpio_put(PICO_OUT_PIN, 0);
        gpio_put(BUZZER_PIN, 0);
        return;
    }

    // TX FIFOに空きがある限り、キューから取り出してPIOへ供給
    while (!pio_sm_is_tx_fifo_full(pio0, pio_play_sm)) {
        // キューが間に合わない(空)場合は、前回のパルス幅を2つずつ繰り返す
        // (T88/CASの無音区間もこの繰り返しで出力している)
        if (!feed_play_fifo(pio0)) {
            break;
        }
        play_swap_count++;
    }
}

// ============================================================================
// 録音(REC) 系関数群
// ============================================================================

static void init_cmt_rec_pio() {
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &cmt_rec_program);
    pio_sm_config c = cmt_rec_program_get_default_config(offset);

    sm_config_set_jmp_pin(&c, PICO_IN_PIN);
    sm_config_set_in_pins(&c, PICO_IN_PIN);
    sm_config_set_sideset_pins(&c, BUZZER_PIN);
    sm_config_set_sideset(&c, 1, false, false);

    pio_gpio_init(pio, PICO_IN_PIN);
    pio_gpio_init(pio, BUZZER_PIN);

    gpio_set_input_hysteresis_enabled(PICO_IN_PIN, true);
    // 入力の同期回路は通す (バイパスすると、クロックの瞬間に入力が切り替わったとき
    // jmp pin の飛び先が乱れ、測定中の値が丸ごと消えることがあった。約1us遅れるだけで、
    // 両方のエッジが同じだけ遅れるのでパルスの長さは変わらない)
    hw_clear_bits(&pio->input_sync_bypass, 1u << PICO_IN_PIN);

    pio_sm_set_consecutive_pindirs(pio, pio_rec_sm, PICO_IN_PIN, 1, false);
    pio_sm_set_consecutive_pindirs(pio, pio_rec_sm, BUZZER_PIN, 1, true);

    gpio_set_drive_strength(BUZZER_PIN, GPIO_DRIVE_STRENGTH_8MA);

    // 2MHz 駆動 (1サイクル 0.5us)。システムクロックは 133MHz なので、決め打ちの 62.5
    // (125MHz 用) では 2.128MHz になり、録音したパルスが約6.4%長く測られていた
    float div = (float)clock_get_hz(clk_sys) / 2000000.0f;
    sm_config_set_clkdiv(&c, div);

    // 受信 FIFO を8段にして、Core1 の読み出しの遅れに余裕を持たせる
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

    pio_sm_init(pio, pio_rec_sm, offset, &c);
    pio_sm_set_enabled(pio, pio_rec_sm, false);
    rec_pio_running = false;
}

static void stop_cmt_rec_pio() {
    if (!rec_pio_running) {
        return;
    }

    pio_sm_set_enabled(pio0, pio_rec_sm, false);

    gpio_set_function(BUZZER_PIN, GPIO_FUNC_SIO);
    gpio_put(BUZZER_PIN, 0);

    rec_pio_running = false;
}

static void start_cmt_rec_pio() {
    if (rec_pio_running) {
        return;
    }

    // 録音スタート直後はブザーの制御権をCPU(SIO)にして、LOW(ミュート)に固定する
    rec_buzzer_mute();
    rec_buzzer_good_halves = 0;

    uint32_t dummy;
    while (queue_try_remove(&cmt_data_queue, &dummy));

    pio_sm_clear_fifos(pio0, pio_rec_sm);
    pio0->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + pio_rec_sm);
    rec_fifo_overflow_count = 0;
    rec_queue_drop_count = 0;
    rec_same_level_count = 0;
    rec_filtered_count = 0;
    rec_prev_level = -1;
    pio_sm_restart(pio0, pio_rec_sm);
    pio_sm_clkdiv_restart(pio0, pio_rec_sm);
    pio_sm_set_enabled(pio0, pio_rec_sm, true);

    rec_pio_running = true;
}

// フラッシュの読み込み待ちで止まらないよう RAM に置く
static void __not_in_flash_func(core1_rec_loop)() {
    if (!rec_pio_running) {
        return;
    }

    // FIFO が一杯のときの push noblock は値を捨て、RXSTALL を立てる
    const uint32_t rxstall_bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + pio_rec_sm);
    if (pio0->fdebug & rxstall_bit) {
        pio0->fdebug = rxstall_bit;
        rec_fifo_overflow_count++;
    }

    // 録音が始まり(rec_triggered)、テープの信号が続いているときだけブザーを鳴らす
    if (rec_triggered && !rec_buzzer_enabled &&
        rec_buzzer_good_halves >= REC_BUZZER_RESUME_HALVES) {
        pio_gpio_init(pio0, BUZZER_PIN);
        rec_buzzer_enabled = true;
    }

    while (!pio_sm_is_rx_fifo_empty(pio0, pio_rec_sm)) {
        uint32_t val = pio_sm_get(pio0, pio_rec_sm);

        bool is_high = ((int32_t)val >= 0);
        uint32_t pulse_width_us =
            is_high ? val : ~val;  // 負の数なら反転して時間を取り出す

        int level = is_high ? 1 : 0;
        if (level == rec_prev_level) {
            rec_same_level_count++;
        }
        rec_prev_level = level;

        if (pulse_width_us >= REC_BUZZER_MIN_HALF_US && pulse_width_us <= REC_BUZZER_MAX_HALF_US) {
            if (rec_buzzer_good_halves < REC_BUZZER_RESUME_HALVES) {
                rec_buzzer_good_halves++;
            }
        } else {
            rec_buzzer_good_halves = 0;
            if (rec_buzzer_enabled) {
                rec_buzzer_mute();
            }
        }

        if (pulse_width_us < 3 || pulse_width_us > 0x00FFFFFF) {
            rec_filtered_count++;
            continue;
        }

        uint32_t queue_val = pulse_width_us;
        if (is_high) {
            queue_val |= 0x80000000;
        }

        if (recording_active && recording_permitted && !rec_paused) {
            if (!queue_try_add(&cmt_data_queue, &queue_val)) {
                rec_queue_drop_count++;
            }
        }
    }
}

void log_core1_rec_stats() {
    Serial.printf(
        "[REC] dropped: fifo_overflow=%lu queue_full=%lu same_level=%lu filtered=%lu\n",
        (unsigned long)rec_fifo_overflow_count,
        (unsigned long)rec_queue_drop_count,
        (unsigned long)rec_same_level_count,
        (unsigned long)rec_filtered_count);
}

// ============================================================================
// Core0 から呼ばれるステータス取得関数
// ============================================================================

void get_core1_playback_stats(bool* session_initialized, uint32_t* swap_count,
                              uint16_t* last_t88_data) {
    if (session_initialized != NULL) {
        *session_initialized = play_session_initialized;
    }
    if (swap_count != NULL) {
        *swap_count = play_swap_count;
    }
    if (last_t88_data != NULL) {
        *last_t88_data = play_last_t88_data;
    }
}

void get_core1_playback_io_stats(bool* pio_enabled, uint8_t* gpio_level) {
    if (pio_enabled != NULL) {
        *pio_enabled = play_session_initialized;
    }
    if (gpio_level != NULL) {
        uint32_t current_outputs = sio_hw->gpio_out;
        *gpio_level = (current_outputs & (1u << PICO_OUT_PIN)) ? 1 : 0;
    }
}

// ============================================================================
// Core 1 Main Loop
// ============================================================================

void setup1() {
    init_cmt_rec_pio();
    init_cmt_play_pio();

    rec_paused = true;
    play_paused = true;
}

void loop1() {
    static bool prev_play_active = false;

    if (!recording_active && !play_active) {
        stop_cmt_rec_pio();
        stop_cmt_play_pio();
        prev_play_active = false;
        core1_blink_wifi_icon();
        return;
    }

    if (recording_active && !rec_paused) {
        if (!rec_pio_running) {
            start_cmt_rec_pio();
        }
        core1_rec_loop();
    } else {
        if (rec_pio_running) {
            stop_cmt_rec_pio();
        }
    }

    if (!play_active && prev_play_active) {
        stop_cmt_play_pio();
    }

    if (play_active) {
        if (playback_permitted) {
            if (!play_pio_running) {
                start_cmt_play_pio();
            }
            core1_play_loop();
        } else {
            if (play_pio_running) {
                pio_sm_set_enabled(pio0, pio_play_sm, false);
                play_pio_running = false;
                gpio_put(PICO_OUT_PIN, 0);
                gpio_put(BUZZER_PIN, 0);
            }
        }
    }

    prev_play_active = play_active;
}