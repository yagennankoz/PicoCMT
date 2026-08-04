#include "player_t77.h"

#include <string.h>

#include "cmt_globals.h"

PlayerT77::PlayerT77() {
    cmt_file = nullptr;
    buffered_reader = nullptr;
}

PlayerT77::~PlayerT77() {
    if (buffered_reader) {
        delete buffered_reader;
    }
}

void PlayerT77::set_log(const char* text) {
    snprintf(playback_log, sizeof(playback_log), "%s", text);
}

bool PlayerT77::enqueue_pulse(queue_t* q, uint16_t us) {
    if (queue_get_level(q) >= PLAY_QUEUE_LENGTH) {
        return false;
    }
    bool res = queue_try_add(q, &us);
    if (res) {
        total_enqueued_pulses++;
    }
    return res;
}

bool PlayerT77::init(File* f, const char* filename) {
    cmt_file = f;

    if (buffered_reader) {
        delete buffered_reader;
    }
    buffered_reader = new SdBufferedRead(cmt_file, 4096);

    if (!cmt_file || cmt_file->size() <= 16) {
        return false;
    }

    uint8_t header[16];
    buffered_reader->seek(0);
    if (buffered_reader->read(header, 16) != 16) {
        return false;
    }

    if (memcmp(header, "XM7 TAPE IMAGE", 14) != 0) {
        return false;
    }

    end_reached = false;
    exhausted = false;
    accumulated_clocks = 0;
    waiting_for_silence = false;
    silence_start_ms = 0;
    silence_target_ms = 0;

    init_position_tracking();

    set_log("T77 Playing");
    return true;
}

void PlayerT77::process(queue_t* play_queue) {
    if (end_reached) {
        if (!exhausted) {
            exhausted = true;
        }
        return;
    }

    if (waiting_for_silence) {
        if (millis() - silence_start_ms >= silence_target_ms) {
            waiting_for_silence = false;
        } else {
            return;
        }
    }

    int iterations = 0;
    while ((PLAY_QUEUE_LENGTH - queue_get_level(play_queue)) > 16) {
        if (++iterations > 256) {
            break;
        }

        uint16_t val;
        if (!buffered_reader->read_word(val)) {
            end_reached = true;
            return;
        }

        if (val == 0x7FFF) {
            accumulated_clocks += 32767;
            continue;
        }

        accumulated_clocks += val;

        uint32_t total_us = accumulated_clocks / 2;
        accumulated_clocks = 0;

        if (total_us > 65000) {
            enqueue_pulse(play_queue, 2);
            waiting_for_silence = true;
            silence_start_ms = millis();
            silence_target_ms = total_us / 1000;
            return;
        } else {
            uint16_t pulse_us = (total_us < 2) ? 2 : (uint16_t)total_us;
            enqueue_pulse(play_queue, pulse_us);
        }
    }
}

bool PlayerT77::seek(int direction, queue_t* play_queue) {
    buffered_reader->seek(16);
    end_reached = false;
    exhausted = false;
    accumulated_clocks = 0;
    waiting_for_silence = false;
    set_log("T77 Seek 0");
    return true;
}

void PlayerT77::stop() {}

bool PlayerT77::is_end_reached() { return end_reached; }
bool PlayerT77::is_exhausted() { return exhausted; }

void PlayerT77::get_status(PlaybackStatus* out) {
    out->playback_t88_mode = false;
    out->playback_t88_end_reached = end_reached;
    out->playback_source_exhausted = exhausted;
    out->file_position =
        buffered_reader ? buffered_reader->display_position() : 0;
    out->file_size = cmt_file ? cmt_file->size() : 0;
    snprintf(out->playback_log, sizeof(out->playback_log), "%s", playback_log);
}