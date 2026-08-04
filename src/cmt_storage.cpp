#include "cmt_storage.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>

#include "cmt_core1.h"
#include "cmt_globals.h"
#include "cmt_hw_config.h"
#include "cmt_player.h"
#include "cmt_t88_converter.h"
#include "cmt_types.h"
#include "pico/util/queue.h"
#include "player_cas.h"
#include "player_cmt.h"
#include "player_p6.h"
#include "player_t77.h"
#include "player_t88.h"

CmtPlayer* active_player = nullptr;

volatile bool playback_seeking_now = false;
extern volatile bool play_paused;
extern volatile bool play_active;

static void drain_play_queue() {
    uint16_t dummy = 0;
    while (queue_try_remove(&cmt_play_queue, &dummy));
}

static void wait_for_core1_playback_shutdown() {
    uint32_t sync_timeout = millis();
    while (millis() - sync_timeout < 100) {
        bool session_inited = true;
        get_core1_playback_stats(&session_inited, NULL, NULL, NULL);
        if (!session_inited) {
            break;
        }
        delayMicroseconds(50);
    }
    delay(10);
}

bool jump_playback_to_blank(int direction) {
    if (!cmt_file || !active_player) {
        return false;
    }

    playback_seeking_now = true;
    play_active = false;
    playback_permitted = false;
    request_playback_shutdown();

    reset_core1_playback_buffers();
    wait_for_core1_playback_shutdown();

    if (!active_player->seek(direction, &cmt_play_queue)) {
        playback_seeking_now = false;
        play_paused = false;
        play_active = true;
        playback_permitted = true;
        init_core1_playback_prime();
        return false;
    }

    playback_seeking_now = false;
    play_paused = false;
    play_active = true;
    playback_permitted = true;

    init_core1_playback_prime();
    return true;
}

static void complete_playback_and_stay_on_screen() {
    play_active = false;
    play_paused = true;
    playback_permitted = false;
    request_playback_shutdown();

    reset_core1_playback_buffers();
    wait_for_core1_playback_shutdown();

    manual_ok_override = false;
    playback_user_paused = false;

    if (cmt_file) {
        cmt_file.close();
    }
    current_state = STATE_PLAY_STANDBY;
}

void stop_playback_and_return_idle() {
    play_active = false;
    play_paused = true;
    playback_permitted = false;
    request_playback_shutdown();
    manual_ok_override = false;
    playback_user_paused = false;
    drain_play_queue();
    cmt_file.close();

    if (active_player) {
        active_player->stop();
        delete active_player;
        active_player = nullptr;
    }
    current_state = STATE_IDLE;
}

void get_playback_status(PlaybackStatus* out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(PlaybackStatus));

    out->play_active = play_active;
    out->play_paused = play_paused;

    if (active_player) {
        active_player->get_status(out);
    }

    out->file_size = cmt_file ? cmt_file.size() : 0;
    out->queue_level = queue_get_level(&cmt_play_queue);
    out->queue_free_space = PLAY_QUEUE_LENGTH - out->queue_level;

    get_core1_playback_stats(
        &out->core1_play_session_initialized, &out->core1_play_swap_count,
        &out->core1_play_last_t88_data, &out->core1_play_using_buffer_a);
    get_core1_playback_io_stats(&out->core1_pio_enabled, &out->core1_dma_busy,
                                &out->core1_gpio_level);
}

void handle_sd_playback_stream() {
    if (!play_active || play_paused || playback_seeking_now || !active_player) {
        return;
    }

    active_player->process(&cmt_play_queue);

    if (active_player->is_exhausted() && queue_is_empty(&cmt_play_queue)) {
        complete_playback_and_stay_on_screen();
    }
}

bool start_playback_file(const char* filename) {
    if (play_active) {
        play_active = false;
        playback_permitted = false;
        request_playback_shutdown();
        reset_core1_playback_buffers();
        wait_for_core1_playback_shutdown();
    } else {
        play_active = false;
        playback_permitted = false;
        drain_play_queue();
        reset_core1_playback_buffers();
    }

    cmt_file = SD.open(filename, FILE_READ);
    if (!cmt_file) {
        return false;
    }

    snprintf(target_filename, sizeof(target_filename), "%s", filename);

    if (active_player) {
        delete active_player;
        active_player = nullptr;
    }

    // 拡張子を見てプラグインを切り替える
    const char* ext = strrchr(filename, '.');
    if (ext) {
        if (strcasecmp(ext, ".T88") == 0) {
            active_player = new PlayerT88();
        } else if (strcasecmp(ext, ".CAS") == 0) {
            active_player = new PlayerCAS();
            ((PlayerCAS*)active_player)->set_baud_rate(cas_selected_baud_rate);
        } else if (strcasecmp(ext, ".T77") == 0) {
            active_player = new PlayerT77();
        } else if (strcasecmp(ext, ".CMT") == 0) {
            active_player = new PlayerCMT();
            ((PlayerCMT*)active_player)->set_baud_rate(cmt_selected_baud_rate);
        } else if (strcasecmp(ext, ".P6") == 0) {
            active_player = new PlayerP6();
        }
    }

    if (!active_player) {
        cmt_file.close();
        return false;  // 非対応フォーマット
    }

    if (!active_player->init(&cmt_file, filename)) {
        delete active_player;
        active_player = nullptr;
        cmt_file.close();
        return false;
    }

    playback_seeking_now = false;
    drain_play_queue();

    // プレチャージ
    int secure_load_count = 0;
    while (queue_get_level(&cmt_play_queue) < (BUFFER_SIZE * 2) &&
           !active_player->is_end_reached()) {
        active_player->process(&cmt_play_queue);
        if (++secure_load_count > 128) {
            break;
        }
    }

    if (queue_is_empty(&cmt_play_queue) && active_player->is_end_reached()) {
        cmt_file.close();
        delete active_player;
        active_player = nullptr;
        return false;
    }

    play_active = true;
    play_paused = false;
    playback_permitted = true;
    playback_started = false;

    init_core1_playback_prime();
    return true;
}

void monitor_playback_finalize() {
    if (current_state == STATE_PLAY_STANDBY && !play_active) {
        return;
    }
    if (!play_active && queue_is_empty(&cmt_play_queue)) {
        delay(100);
        stop_playback_and_return_idle();
    }
}

bool load_directory(const String& path) {
    total_items = 0;

    File dir = SD.open(path.c_str());
    if (!dir || !dir.isDirectory()) {
        return false;
    }

    while (total_items < MAX_FILES) {
        File entry = dir.openNextFile();
        if (!entry) {
            break;
        }

        const char* fname = entry.name();

        if (fname[0] == '.') {
            entry.close();
            continue;
        }

        if (strcasecmp(fname, "System Volume Information") == 0 ||
            strcasecmp(fname, "$RECYCLE.BIN") == 0) {
            entry.close();
            continue;
        }

        if (!entry.isDirectory()) {
            const char* ext = strrchr(fname, '.');
            if (!ext ||
                (strcasecmp(ext, ".T88") != 0 && strcasecmp(ext, ".CAS") != 0 &&
                 strcasecmp(ext, ".T77") != 0 && strcasecmp(ext, ".CMT") != 0 &&
                 strcasecmp(ext, ".P6") != 0)) {
                entry.close();
                continue;
            }
        }

        snprintf(file_list[total_items].name, MAX_NAME_LEN, "%s", fname);
        file_list[total_items].is_dir = entry.isDirectory();
        total_items++;
        entry.close();
    }
    dir.close();

    for (int i = 0; i < total_items - 1; i++) {
        for (int j = i + 1; j < total_items; j++) {
            bool swap_needed = false;

            if (file_list[i].is_dir && !file_list[j].is_dir) {
                swap_needed = false;
            } else if (!file_list[i].is_dir && file_list[j].is_dir) {
                swap_needed = true;
            } else {
                if (strcasecmp(file_list[i].name, file_list[j].name) > 0) {
                    swap_needed = true;
                }
            }

            if (swap_needed) {
                FileItem temp = file_list[i];
                file_list[i] = file_list[j];
                file_list[j] = temp;
            }
        }
    }

    if (total_items == 0) {
        SD.end();
        if (!SD.begin(PICO_SD_CS_PIN, SD_SCK_MHZ(16), SPI1)) {
            return false;
        }
    }
    return true;
}
