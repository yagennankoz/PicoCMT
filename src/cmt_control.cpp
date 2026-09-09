#include "cmt_control.h"

#include <Arduino.h>

#include "cmt_globals.h"
#include "cmt_hw_config.h"
#include "cmt_pins.h"
#include "cmt_storage_record.h"
#include "pico/util/queue.h"

static bool remote_hardware_on = false;

void update_system_run_permissions() {
    if (current_state == STATE_PLAYING || current_state == STATE_PLAY_STANDBY) {
        if (remote_hardware_on || manual_ok_override) {
            // 再生再開時にシステムタイマーの起点を取り直す
            if (play_paused) {
                playback_tag_start_system_ms = millis();
            }
            playback_permitted = true;
            play_paused = false;
            current_state = STATE_PLAYING;
            playback_started = true;
        } else {
            // リモートオフ時に停止するまでの経過時間を加算
            if (!play_paused && playback_time_valid) {
                playback_base_time_ms +=
                    (millis() - playback_tag_start_system_ms);
            }
            playback_permitted = false;
            play_paused = true;
            if (current_state == STATE_PLAYING) {
                current_state = STATE_PLAY_STANDBY;
                oled_need_refresh = true;
            }
        }
    }
    if (current_state == STATE_RECORDING ||
        current_state == STATE_REC_STANDBY) {
        if (remote_hardware_on || manual_ok_override) {
            recording_permitted = true;
            rec_paused = false;
            current_state = STATE_RECORDING;
        } else {
            if (current_state == STATE_RECORDING) {
                stop_raw_recording_and_finalize();
            }
            recording_permitted = false;
            rec_paused = true;
        }
    }
}

void handle_remote_change_event(RemoteState new_state) {
    if (new_state == REMOTE_ON) {
        remote_hardware_on = true;
    } else {
        remote_hardware_on = false;
        if (manual_ok_override) {
            manual_ok_override = false;
        }
    }
    update_system_run_permissions();
}

void scan_remote_hardware() {
    uint32_t now = millis();
    bool raw_reading = (gpio_get(REMOTE_PIN) == 0);
    RemoteState target_state = raw_reading ? REMOTE_ON : REMOTE_OFF;

    if (target_state != current_remote_state) {
        if (remote_stable_time == 0) {
            remote_stable_time = now;
        } else {
            // チャタリング対策: ON時はボタンと同じ(DEBOUNCE_MS)速度で反応し、
            // OFF時はノイズとリレーのバタつきを無視するため 500ms
            // 間連続で安定するのを待つ
            uint32_t required_stable_time =
                (target_state == REMOTE_OFF) ? 500 : DEBOUNCE_MS;

            if (now - remote_stable_time > required_stable_time) {
                current_remote_state = target_state;
                remote_stable_time = 0;

                if (current_remote_state == REMOTE_ON) {
                    remote_hardware_on = true;
                } else {
                    remote_hardware_on = false;
                    if (manual_ok_override) {
                        manual_ok_override = false;
                    }
                }
                update_system_run_permissions();
            }
        }
    } else {
        // 状態が一致（安定）している場合はタイマーをリセット
        remote_stable_time = 0;
    }
}

void init_cores_communication() {
    queue_init(&cmt_data_queue, sizeof(uint32_t), PLAY_QUEUE_LENGTH);
    queue_init(&cmt_play_queue, sizeof(uint16_t), PLAY_QUEUE_LENGTH);
}