#pragma once

#include <Arduino.h>
#include "cmt_types.h"

void handle_sd_storage_stream();
void handle_sd_playback_stream();
void stop_playback_and_return_idle();
void get_playback_status(PlaybackStatus* out);
bool start_playback_file(const char* filename);
bool jump_playback_to_blank(int direction);
bool start_recording_file(const char* filename, RecMode mode);
void monitor_playback_finalize();
bool load_directory(const String& path);
void fill_playback_queue_from_t88();